// test_noise.cpp — Phase 8 unit tests for Monte Carlo trajectory noise
// execution on FullState CPU and GPU backends.
// Tests cover: bit-flip p=1, depolarizing distribution shift, amplitude damping,
// idle_channels() API, reproducibility, noiseless-through-noise-path parity,
// and GPU noisy execution (guard-skipped when no CUDA device is present).
// Arrange/Act/Assert; EXPECT_NEAR tolerance rationale.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../../../include/backend/simulator_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/noise/device_model.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/noise/t1t2_calculator.hpp"
#include "../../../include/qregister.hpp"
#include "../../../include/result.hpp"
#include "../../../include/types.hpp"

using qplayer::Circuit;
using qplayer::DeviceModel;
using qplayer::KrausChannel;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::Result;
using qplayer::SimType;
using qplayer::make_backend;

// Forward-declare the CUDA device count probe at file scope inside a GPU-build
// guard. The symbol is defined in full_state_gpu.cu. Placing extern "C" at
// namespace scope satisfies GCC C++17 [dcl.link] requirements.
#if defined(QPLAYER_BUILD_GPU)
extern "C" int qplayer_cuda_device_count();
#endif

namespace {

// Sum all count values (= total shots accumulated across all bitstrings).
std::uint64_t total_shots(const std::map<std::string, std::uint64_t>& counts) {
    std::uint64_t n = 0;
    for (const auto& kv : counts) n += kv.second;
    return n;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// CPU: bit-flip p=1 after each X gate flips the state back → always measures 0
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, BitFlip_P1_AfterXGate_MeasuresZeroAlways) {
    // Arrange
    // Starting from |0⟩ → X gate → |1⟩ → bit-flip(p=1) applies X → |0⟩.
    // Deterministic result: every trajectory measures "0".
    // Tolerance: exact (not statistical) — K_1=X applied with probability 1.
    const std::uint32_t nshots = 300;
    QRegister qr(SimType::FullState, ProcType::CPU);
    Circuit circ(1, 1);
    circ.x(0);
    circ.measure(0, 0);

    NoiseModel nm;
    nm.add_bit_flip("x", 1.0);  // K_0=0*I, K_1=1*X — always applies X

    qr.set_seed(42);
    qr.set_noise_model(nm);

    // Act
    const auto result = qr.run(circ, nshots, /*enable_fusion=*/false);
    const auto counts = result.counts();

    // Assert — X|0⟩=|1⟩, then bit-flip(1)|1⟩=X|1⟩=|0⟩: every shot is "0".
    const std::uint64_t count0 = counts.count("0") ? counts.at("0") : 0ULL;
    EXPECT_EQ(count0, static_cast<std::uint64_t>(nshots));  // deterministic
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU: depolarizing noise shifts Bell distribution — "01"/"10" become visible
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, Depolarizing_ShiftsBellDistribution) {
    // Arrange — Bell circuit with strong depolarizing on CX (p=0.3).
    // Noiseless Bell: only "00" and "11" at ~50% each.
    // With p=0.3 depolarizing: "01" and "10" leak in measurably.
    const std::uint32_t nshots = 4000;
    QRegister qr(SimType::FullState, ProcType::CPU);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    NoiseModel nm;
    nm.add_depolarizing("cx", 0.3);  // strong noise for measurable leakage

    qr.set_seed(1234);
    qr.set_noise_model(nm);

    // Act
    const auto result = qr.run(circ, nshots, /*enable_fusion=*/false);
    const auto counts = result.counts();

    // Assert — "01"+"10" must be non-negligible (>1%).
    // With p=0.3 depolarizing on CX we expect ~10-20% in "01"+"10". The
    // 1% threshold is extremely conservative (P(0 shots in "01"+"10" | n=4000)
    // is negligibly small by Poisson estimate).
    const double p01 = counts.count("01")
                         ? static_cast<double>(counts.at("01")) / nshots : 0.0;
    const double p10 = counts.count("10")
                         ? static_cast<double>(counts.at("10")) / nshots : 0.0;
    EXPECT_TRUE(p01 + p10 > 0.01);  // noise must be measurable
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU: amplitude damping with gamma=0.9 relaxes |1⟩ → ~90% measure 0
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, AmplitudeDamping_RelaxesExcitedState) {
    // Arrange — start in |1⟩ (X on |0⟩), apply amplitude_damping(gamma=0.9).
    // K_1 = sqrt(0.9)|0⟩⟨1| relaxes |1⟩ to |0⟩ with probability 0.9.
    // Expected: ~90% of trajectories measure 0.
    const std::uint32_t nshots = 3000;
    QRegister qr(SimType::FullState, ProcType::CPU);
    Circuit circ(1, 1);
    circ.x(0);
    circ.measure(0, 0);

    NoiseModel nm;
    nm.add_amplitude_damping("x", 0.9);  // gamma=0.9: high relaxation probability

    qr.set_seed(99);
    qr.set_noise_model(nm);

    // Act
    const auto result = qr.run(circ, nshots, /*enable_fusion=*/false);
    const auto counts = result.counts();

    // Assert — p(measure=0) ≈ gamma = 0.9.
    // Statistical tolerance ±0.05: sigma ≈ sqrt(0.9*0.1/3000) ≈ 0.0055; 9σ ≈ 0.05.
    const double p0 = counts.count("0")
                        ? static_cast<double>(counts.at("0")) / nshots : 0.0;
    EXPECT_NEAR(p0, 0.9, 0.05);  // statistical tolerance, 9σ safety margin
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}

// ─────────────────────────────────────────────────────────────────────────────
// NoiseModel::idle_channels() — returns T1/T2 relaxation channels for finite T1
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, IdleChannels_FiniteCoherence_ReturnsRelaxation) {
    // Arrange — device with finite T1=50 µs, T2=30 µs for qubit 0.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    // Act — compute idle channels for a 300 ns idle window.
    const auto chans = nm.idle_channels(static_cast<QubitIndex>(0), 300.0);

    // Assert — at least one channel returned (T1/T2 are finite).
    const auto expected =
        qplayer::T1T2Calculator::compute(50000.0, 30000.0, 300.0, 0);
    EXPECT_EQ(chans.size(), expected.size());
    EXPECT_TRUE(!chans.empty());  // finite coherence → relaxation channels

    // Verify trace-preserving: Σ K_i^† K_i = I for each channel.
    for (const KrausChannel& ch : chans) {
        const auto& ops = ch.operators;
        if (ops.empty()) continue;
        const std::size_t dim = ops.front().size();
        for (std::size_t r = 0; r < dim; ++r) {
            for (std::size_t c = 0; c < dim; ++c) {
                std::complex<double> acc{0.0, 0.0};
                for (const auto& K : ops) {
                    for (std::size_t k = 0; k < dim; ++k) {
                        acc += std::conj(K[k][r]) * K[k][c];
                    }
                }
                const std::complex<double> target =
                    (r == c) ? std::complex<double>{1.0, 0.0}
                             : std::complex<double>{0.0, 0.0};
                // tolerance 1e-10: exact numerical result
                EXPECT_NEAR(std::abs(acc - target), 0.0, 1e-10);
            }
        }
    }
}

TEST(NoiseCpu, IdleChannels_OutOfRangeQubit_ReturnsEmpty) {
    // Arrange — device with 1 qubit; ask for qubit 5 (out of range).
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    // Act + Assert — out-of-range qubit → no data → empty channel list.
    // Exact: no channel for qubit 5.
    const auto chans = nm.idle_channels(static_cast<QubitIndex>(5), 300.0);
    EXPECT_TRUE(chans.empty());
}

TEST(NoiseCpu, IdleChannels_NoDeviceLoaded_ReturnsEmpty) {
    // Arrange — NoiseModel with gate channels but no load_device() call.
    // t1_/t2_ vectors remain empty → idle_channels() returns {} for any qubit.
    NoiseModel nm;
    nm.add_depolarizing("h", 0.01);  // gate noise present but no device T1/T2

    // Act + Assert.
    const auto chans = nm.idle_channels(static_cast<QubitIndex>(0), 100.0);
    EXPECT_TRUE(chans.empty());  // exact: t1_/t2_ empty
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU: same master seed → identical counts (reproducibility)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, Reproducibility_SameSeedGivesIdenticalCounts) {
    // Arrange — two separate QRegister instances with identical seed and model.
    const std::uint32_t nshots = 400;
    const std::uint64_t fixed_seed = 12345678ULL;

    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    NoiseModel nm;
    nm.add_depolarizing("h", 0.05).add_depolarizing("cx", 0.05);

    QRegister qr1(SimType::FullState, ProcType::CPU);
    qr1.set_seed(fixed_seed);
    qr1.set_noise_model(nm);

    QRegister qr2(SimType::FullState, ProcType::CPU);
    qr2.set_seed(fixed_seed);
    qr2.set_noise_model(nm);

    // Act
    const auto r1 = qr1.run(circ, nshots, /*enable_fusion=*/false);
    const auto r2 = qr2.run(circ, nshots, /*enable_fusion=*/false);

    // Assert — identical seeds → identical per-trajectory RNG → identical counts.
    // Tolerance: none (exact equality — deterministic from seed).
    const auto c1 = r1.counts();
    const auto c2 = r2.counts();
    EXPECT_EQ(c1.size(), c2.size());
    for (const auto& kv : c1) {
        const std::uint64_t v2 = c2.count(kv.first) ? c2.at(kv.first) : 0ULL;
        EXPECT_EQ(kv.second, v2);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU: empty NoiseModel through noise path matches noiseless Bell distribution
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, EmptyNoiseModel_NoiselessPathParity) {
    // Arrange — empty NoiseModel selects the Monte Carlo trajectory loop without
    // applying any channels. The resulting distribution must match the noiseless
    // ideal Bell distribution within statistical tolerance.
    const std::uint32_t nshots = 2000;

    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(7777);
    qr.set_noise_model(NoiseModel{});  // empty model — trajectory path, no channels

    // Act
    const auto result = qr.run(circ, nshots, /*enable_fusion=*/false);
    const auto counts = result.counts();

    // Assert — Bell state: "00" ≈ 0.5, "11" ≈ 0.5.
    // Statistical tolerance ±0.05: sigma ≈ sqrt(0.5*0.5/2000) ≈ 0.011; 4.5σ ≈ 0.05.
    const double p00 = counts.count("00")
                         ? static_cast<double>(counts.at("00")) / nshots : 0.0;
    const double p11 = counts.count("11")
                         ? static_cast<double>(counts.at("11")) / nshots : 0.0;
    EXPECT_NEAR(p00, 0.5, 0.05);  // 4.5σ coverage for nshots=2000
    EXPECT_NEAR(p11, 0.5, 0.05);
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU: empty NoiseModel produces Bell distribution (GPU trajectory path)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseGpu, EmptyNoiseModel_ProducesBellDistribution) {
    // Guard — skip if no CUDA device is present; test body is irrelevant on
    // CPU-only machines. The guard must be inside the test body (not at file
    // scope) to keep test registration consistent across builds.
#if defined(QPLAYER_BUILD_GPU)
    if (qplayer_cuda_device_count() <= 0) return;
#else
    return;
#endif

    // Arrange — Bell circuit through the GPU noisy trajectory path (empty model).
    const std::uint32_t nshots = 800;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);

    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    NoiseModel nm;  // empty — activates trajectory path, applies no channels
    backend->set_seed(42ULL);

    // Act
    Result result;
    EXPECT_NO_THROW(result = backend->execute(circ, nshots, &nm));
    const auto counts = result.counts();

    // Assert — Bell distribution: "00" ≈ 0.5, "11" ≈ 0.5.
    // Statistical tolerance ±0.06: sigma ≈ 0.018 for nshots=800; 3.5σ ≈ 0.06.
    const double p00 = counts.count("00")
                         ? static_cast<double>(counts.at("00")) / nshots : 0.0;
    const double p11 = counts.count("11")
                         ? static_cast<double>(counts.at("11")) / nshots : 0.0;
    EXPECT_NEAR(p00, 0.5, 0.06);  // 3.5σ coverage for nshots=800
    EXPECT_NEAR(p11, 0.5, 0.06);
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU: depolarizing noise on Bell circuit gives sane (normalised) distribution
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseGpu, NoisyBellCircuit_GivesSaneDistribution) {
    // Guard — skip if no CUDA device is present.
#if defined(QPLAYER_BUILD_GPU)
    if (qplayer_cuda_device_count() <= 0) return;
#else
    return;
#endif

    // Arrange — mild depolarizing noise (p=0.1) on CX via GPU trajectory path.
    // "00"+"11" must still dominate (Bell structure preserved under mild noise).
    const std::uint32_t nshots = 500;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);

    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    NoiseModel nm;
    nm.add_depolarizing("cx", 0.1);  // mild noise

    backend->set_seed(777ULL);

    // Act
    Result result;
    EXPECT_NO_THROW(result = backend->execute(circ, nshots, &nm));
    const auto counts = result.counts();

    // Assert — total shots preserved; "00"+"11" cover at least 70%.
    // With p=0.1 depolarizing, expected "00"+"11" ≈ 85%; threshold 70% is
    // conservative: P(ratio<0.70 | expected=0.85, n=500) ≈ 1e-8.
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
    const double p00 = counts.count("00")
                         ? static_cast<double>(counts.at("00")) / nshots : 0.0;
    const double p11 = counts.count("11")
                         ? static_cast<double>(counts.at("11")) / nshots : 0.0;
    EXPECT_TRUE(p00 + p11 > 0.7);  // mild noise preserves Bell structure
}

// ─────────────────────────────────────────────────────────────────────────────
// NoiseModel: readout error accessor returns registered probs and {0,0} default
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseModelReadout, Accessor_ReturnsRegisteredAndDefault) {
    // Arrange
    NoiseModel nm;
    nm.add_readout_error(2, 0.15, 0.07);

    // Act / Assert — registered qubit returns its {p01, p10}; others return {0,0}.
    // Tolerance: exact (stored values round-trip without arithmetic, 1e-12 guard).
    EXPECT_TRUE(nm.has_readout_errors());
    const auto [p01, p10] = nm.readout_error(2);
    EXPECT_NEAR(p01, 0.15, 1e-12);
    EXPECT_NEAR(p10, 0.07, 1e-12);

    const auto [d01, d10] = nm.readout_error(0);  // unregistered qubit
    EXPECT_NEAR(d01, 0.0, 1e-12);
    EXPECT_NEAR(d10, 0.0, 1e-12);

    EXPECT_FALSE(NoiseModel{}.has_readout_errors());  // empty model
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU: readout error P(0|1) shifts the reported bit of a true |1> toward "0"
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, ReadoutError_MisreadsExcitedStateAsZero) {
    // Arrange — X|0>=|1> (true outcome always 1); readout P(0|1)=0.3 corrupts
    // the reported classical bit so ~30% of shots report "0". The quantum
    // collapse stays on the true outcome.
    const std::uint32_t nshots = 4000;
    QRegister qr(SimType::FullState, ProcType::CPU);
    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);

    NoiseModel nm;
    nm.add_readout_error(0, /*p01=*/0.30, /*p10=*/0.0);

    qr.set_seed(2026);
    qr.set_noise_model(nm);

    // Act
    const auto counts = qr.run(circ, nshots, /*enable_fusion=*/false).counts();

    // Assert — P(report 0) ≈ 0.30. sigma = sqrt(0.3*0.7/4000) ≈ 0.0072; 0.04 ≈ 5.5σ.
    const double p0 = counts.count("0")
                        ? static_cast<double>(counts.at("0")) / nshots : 0.0;
    EXPECT_NEAR(p0, 0.30, 0.04);
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU: readout error P(1|0) flips a true |0> report toward "1" (other direction)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, ReadoutError_MisreadsGroundStateAsOne) {
    // Arrange — no gate: true outcome always 0; readout P(1|0)=0.20 reports "1"
    // on ~20% of shots. Confirms the p10 branch of apply_readout_error.
    const std::uint32_t nshots = 4000;
    QRegister qr(SimType::FullState, ProcType::CPU);
    Circuit circ(1, 1);
    circ.measure(0, 0);

    NoiseModel nm;
    nm.add_readout_error(0, /*p01=*/0.0, /*p10=*/0.20);

    qr.set_seed(99);
    qr.set_noise_model(nm);

    // Act
    const auto counts = qr.run(circ, nshots, /*enable_fusion=*/false).counts();

    // Assert — P(report 1) ≈ 0.20. sigma = sqrt(0.2*0.8/4000) ≈ 0.0063; 0.04 ≈ 6σ.
    const double p1 = counts.count("1")
                        ? static_cast<double>(counts.at("1")) / nshots : 0.0;
    EXPECT_NEAR(p1, 0.20, 0.04);
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU: readout error is reproducible under a fixed seed (RNG draw)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseCpu, ReadoutError_SameSeedGivesIdenticalCounts) {
    // Arrange — identical seed + readout model on two registers.
    const std::uint32_t nshots = 600;
    const std::uint64_t fixed_seed = 555ULL;

    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);

    NoiseModel nm;
    nm.add_readout_error(0, 0.25, 0.10);

    QRegister qr1(SimType::FullState, ProcType::CPU);
    qr1.set_seed(fixed_seed);
    qr1.set_noise_model(nm);
    QRegister qr2(SimType::FullState, ProcType::CPU);
    qr2.set_seed(fixed_seed);
    qr2.set_noise_model(nm);

    // Act
    const auto c1 = qr1.run(circ, nshots, /*enable_fusion=*/false).counts();
    const auto c2 = qr2.run(circ, nshots, /*enable_fusion=*/false).counts();

    // Assert — exact equality (deterministic readout draw from the same seed).
    EXPECT_EQ(c1.size(), c2.size());
    for (const auto& kv : c1) {
        const std::uint64_t v2 = c2.count(kv.first) ? c2.at(kv.first) : 0ULL;
        EXPECT_EQ(kv.second, v2);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU: readout error P(0|1) shifts the reported bit (parity with CPU path)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NoiseGpu, ReadoutError_MisreadsExcitedStateAsZero) {
    // Guard — skip if no CUDA device is present.
#if defined(QPLAYER_BUILD_GPU)
    if (qplayer_cuda_device_count() <= 0) return;
#else
    return;
#endif

    // Arrange — same readout scenario as the CPU test, GPU trajectory path.
    const std::uint32_t nshots = 4000;
    auto backend = make_backend(SimType::FullState, ProcType::GPU);

    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);

    NoiseModel nm;
    nm.add_readout_error(0, /*p01=*/0.30, /*p10=*/0.0);
    backend->set_seed(2026ULL);

    // Act
    Result result;
    EXPECT_NO_THROW(result = backend->execute(circ, nshots, &nm));
    const auto counts = result.counts();

    // Assert — P(report 0) ≈ 0.30 (same helper as CPU → matching statistics).
    const double p0 = counts.count("0")
                        ? static_cast<double>(counts.at("0")) / nshots : 0.0;
    EXPECT_NEAR(p0, 0.30, 0.04);
    EXPECT_EQ(total_shots(counts), static_cast<std::uint64_t>(nshots));
}
