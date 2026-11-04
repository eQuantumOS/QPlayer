// test_device_noise.cpp — Phase 8 DeviceModel-driven noise integration
// Exercises the full device pipeline:
// DeviceModel JSON -> NoiseModel::load_device (gate relaxation) and the idle
// path DurationAnnotationPass -> IdleNoiseInserterPass -> backend idle_channels.
// Verifies relaxation shifts the measured distribution in the physical direction
// and that DeviceModel noise is reproducible under a fixed seed.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "../../../include/backend/simulator_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/noise/device_model.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/result.hpp"
#include "../../../include/transpiler/passes/analysis/duration_annotation_pass.hpp"
#include "../../../include/transpiler/passes/scheduling/idle_noise_inserter_pass.hpp"
#include "../../../include/types.hpp"

using qplayer::Circuit;
using qplayer::DeviceModel;
using qplayer::DurationAnnotationPass;
using qplayer::IdleNoiseInserterPass;
using qplayer::make_backend;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::Result;
using qplayer::SimType;

namespace {

// Probability that the qubit measured into cubit `c` reads 1 (marginal over the
// MSB-first bitstring whose position for cubit c is index total_c-1-c).
double prob_cubit_one(const std::map<std::string, std::uint64_t>& counts,
                      std::size_t cubit, std::uint32_t nshots) {
    std::uint64_t ones = 0;
    for (const auto& kv : counts) {
        const std::string& bs = kv.first;          // MSB-first, length total_c
        const std::size_t pos = bs.size() - 1 - cubit;
        if (pos < bs.size() && bs[pos] == '1') ones += kv.second;
    }
    return static_cast<double>(ones) / nshots;
}

#if defined(QPLAYER_BUILD_GPU)
extern "C" int qplayer_cuda_device_count();
#endif
bool gpu_available() {
#if defined(QPLAYER_BUILD_GPU)
    return qplayer_cuda_device_count() > 0;
#else
    return false;
#endif
}

}  // namespace

// ── Gate relaxation: amplitude damping after a gate decays the excited state ──

TEST(DeviceNoise, GateRelaxation_ExcitedStateDecaysTowardGround) {
    // Arrange — device: qubit0 T1=100ns, gate "x" duration 50ns
    // -> gamma = 1 - exp(-50/100) = 0.3935. After x(0) -> |1>, the registered
    // relaxation channel decays |1> to |0> with probability ~0.3935.
    const std::uint32_t nshots = 4000;
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 100.0, "T2": 80.0}],
      "gates": {"x": {"duration_ns": 50.0}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);

    auto backend = make_backend(SimType::FullState, ProcType::CPU);
    backend->set_seed(424242ULL);

    // Act
    Result r = backend->execute(circ, nshots, &nm);
    const double p0 = 1.0 - prob_cubit_one(r.counts(), 0, nshots);

    // Assert — measured "0" probability tracks gamma = 0.3935.
    // sigma = sqrt(0.39*0.61/4000) ~ 0.0077; +-0.05 ~ 6.5 sigma.
    EXPECT_NEAR(p0, 0.3935, 0.05);
}

// ── Idle noise: an excited qubit idling during another gate relaxes ─

TEST(DeviceNoise, IdleNoise_ExcitedIdlingQubitRelaxes) {
    // Arrange — q1 is excited (x), then idles while a long h runs on q0.
    // Device: q1 T1=100ns, "h" duration 200ns -> idle gamma = 1-exp(-2) = 0.865,
    // so q1 strongly relaxes toward |0> during the idle window.
    const std::uint32_t nshots = 4000;
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 100.0, "T2": 80.0}, {"T1": 100.0, "T2": 80.0}],
      "gates": {"x": {"duration_ns": 10.0}, "h": {"duration_ns": 200.0}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    Circuit circ(2, 2);
    circ.x(1).h(0).measure(0, 0).measure(1, 1);

    // Full device pipeline ( will wire this into transpile()).
    Circuit annotated = DurationAnnotationPass(dev).run(circ);
    Circuit with_idle = IdleNoiseInserterPass().run(annotated);

    auto backend = make_backend(SimType::FullState, ProcType::CPU);
    backend->set_seed(0xBEEFULL);

    // Act
    Result r = backend->execute(with_idle, nshots, &nm);
    const double p_q1_one = prob_cubit_one(r.counts(), 1, nshots);

    // Assert — without idle noise q1 would read 1 deterministically; with strong
    // idle relaxation a substantial fraction decays to 0, so p(q1=1) < 0.85.
    EXPECT_TRUE(p_q1_one < 0.85);
}

// ── Reproducibility: DeviceModel noise is deterministic under a fixed seed ────

TEST(DeviceNoise, SameSeed_GivesIdenticalCounts) {
    // Arrange
    const std::uint32_t nshots = 2000;
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 100.0, "T2": 80.0}],
      "gates": {"x": {"duration_ns": 50.0}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);

    auto b1 = make_backend(SimType::FullState, ProcType::CPU);
    auto b2 = make_backend(SimType::FullState, ProcType::CPU);
    b1->set_seed(13579ULL);
    b2->set_seed(13579ULL);

    // Act
    const auto c1 = b1->execute(circ, nshots, &nm).counts();
    const auto c2 = b2->execute(circ, nshots, &nm).counts();

    // Assert — exact equality (deterministic seed derivation).
    EXPECT_EQ(c1.size(), c2.size());
    for (const auto& kv : c1) {
        const std::uint64_t v2 = c2.count(kv.first) ? c2.at(kv.first) : 0ULL;
        EXPECT_EQ(kv.second, v2);
    }
}

// ── : load_device auto-registers readout error -> distribution shift ─────
// Device: infinite coherence (no relaxation), only asymmetric readout on q0.
// x(0) -> true |1>; readout p01 = P(report 0 | true 1) = 0.1 flips ~10% to "0".
TEST(DeviceNoise, LoadDeviceReadout_FlipsReportedOutcome) {
    const std::uint32_t nshots = 8000;
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"prob_meas0_prep1": 0.10, "prob_meas1_prep0": 0.02}]
    })");
    NoiseModel nm;
    nm.load_device(dev);

    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);

    auto backend = make_backend(SimType::FullState, ProcType::CPU);
    backend->set_seed(2024ULL);
    Result r = backend->execute(circ, nshots, &nm);
    const double p0 = 1.0 - prob_cubit_one(r.counts(), 0, nshots);

    // ~10% of the true-|1> shots are reported as 0. sigma ~ 0.0034; +-0.03 safe.
    EXPECT_NEAR(p0, 0.10, 0.03);
}

// ── : two-qubit (4x4) depolarizing Kraus application ─────────────────────
// cz(0,1) leaves |00> unchanged, so attaching a 2Q depolarizing channel (p=0.5)
// to "cz" isolates the depolarizing distribution: on |00> the channel yields
// P(00) = (1-p) + p/5 and P(01)=P(10)=P(11) = 4p/15 (derived from the 16 Paulis).
TEST(DeviceNoise, TwoQubitDepolarizing_MixesGroundState) {
    const std::uint32_t nshots = 40000;
    // Infinite coherence -> pure depol; error_rate 0.4 -> p = 1.25*0.4 = 0.5.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "gates": {"cz_0_1": {"duration_ns": 200.0, "error_rate": 0.4}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    Circuit circ(2, 2);
    circ.cz(0, 1).measure(0, 0).measure(1, 1);   // cz|00> = |00>

    auto backend = make_backend(SimType::FullState, ProcType::CPU);
    backend->set_seed(77ULL);
    const auto counts = backend->execute(circ, nshots, &nm).counts();

    auto frac = [&](const char* bs) {
        auto it = counts.find(bs);
        return it == counts.end() ? 0.0 : static_cast<double>(it->second) / nshots;
    };
    const double p = 0.5;
    EXPECT_NEAR(frac("00"), (1.0 - p) + p / 5.0, 0.02);   // 0.6
    EXPECT_NEAR(frac("01"), 4.0 * p / 15.0, 0.02);        // 0.1333
    EXPECT_NEAR(frac("10"), 4.0 * p / 15.0, 0.02);
    EXPECT_NEAR(frac("11"), 4.0 * p / 15.0, 0.02);
}

// ── : 2Q depolarizing CPU<->GPU parity (shared apply_kraus_2q) ───────────
TEST(DeviceNoise, TwoQubitDepolarizing_CpuGpuParity) {
    if (!gpu_available()) return;

    const std::uint32_t nshots = 40000;
    const std::uint64_t seed   = 555ULL;
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "gates": {"cz_0_1": {"duration_ns": 200.0, "error_rate": 0.3}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    Circuit circ(2, 2);
    circ.cz(0, 1).measure(0, 0).measure(1, 1);

    auto cpu = make_backend(SimType::FullState, ProcType::CPU);
    auto gpu = make_backend(SimType::FullState, ProcType::GPU);
    if (gpu->name() != std::string_view("FullStateGPU")) return;
    cpu->set_seed(seed);
    gpu->set_seed(seed);
    const auto cc = cpu->execute(circ, nshots, &nm).counts();
    const auto gc = gpu->execute(circ, nshots, &nm).counts();

    auto frac = [&](const std::map<std::string, std::uint64_t>& c, const char* bs) {
        auto it = c.find(bs);
        return it == c.end() ? 0.0 : static_cast<double>(it->second) / nshots;
    };
    for (const char* bs : {"00", "01", "10", "11"})
        EXPECT_NEAR(frac(cc, bs), frac(gc, bs), 0.02);
}

// ── : full device-noise path (relaxation + excess depol + readout) ───────
// CPU vs GPU distribution parity — the synthesised channels apply identically on
// both backends (mechanism), now driven end-to-end from load_device.
TEST(DeviceNoise, LoadDeviceFullSynthesis_CpuGpuParity) {
    if (!gpu_available()) return;   // skip without a CUDA device

    const std::uint32_t nshots = 20000;
    const std::uint64_t seed   = 98765ULL;
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 90000.0, "T2": 70000.0,
                  "prob_meas0_prep1": 0.05, "prob_meas1_prep0": 0.04}],
      "basis_gates": ["sx", "x", "rz", "ecr"],
      "gates": {"sx_0": {"duration_ns": 35.0, "error_rate": 0.0008},
                "x_0":  {"duration_ns": 35.0, "error_rate": 0.0008}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    Circuit circ(1, 1);
    circ.sx(0).sx(0).x(0).measure(0, 0);   // native-basis gates carry the noise

    auto cpu = make_backend(SimType::FullState, ProcType::CPU);
    auto gpu = make_backend(SimType::FullState, ProcType::GPU);
    if (gpu->name() != std::string_view("FullStateGPU")) return;
    cpu->set_seed(seed);
    gpu->set_seed(seed);

    const double cpu_p1 = prob_cubit_one(cpu->execute(circ, nshots, &nm).counts(), 0, nshots);
    const double gpu_p1 = prob_cubit_one(gpu->execute(circ, nshots, &nm).counts(), 0, nshots);

    // ±0.02 absorbs CPU-SIMD vs GPU-kernel boundary flips (parity band).
    EXPECT_NEAR(cpu_p1, gpu_p1, 0.02);
}
