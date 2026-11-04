// test_noise_cpu_gpu_parity.cpp — Phase 8 CPU<->GPU noise parity
// The same circuit + NoiseModel + master seed run
// on FullState CPU and FullState GPU must yield the same measurement
// distribution within ±0.01. Both backends derive per-trajectory RNG from the
// identical seed map (detail::thread_seed(seed, traj)) and apply the identical
// Monte Carlo Kraus algorithm, so the distributions are near-exact; the ±0.01
// band absorbs the rare CPU-SIMD vs GPU-kernel floating-point boundary flip.
// Skipped when no CUDA device is present.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "../../../include/backend/simulator_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/result.hpp"
#include "../../../include/types.hpp"

using qplayer::Circuit;
using qplayer::make_backend;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::Result;
using qplayer::SimType;

#if defined(QPLAYER_BUILD_GPU)
extern "C" int qplayer_cuda_device_count();
#endif

namespace {

// Run a circuit + noise model on a given ProcType and return measured probs.
std::map<std::string, double> run_probs(ProcType proc, const Circuit& circ,
                                        const NoiseModel& nm,
                                        std::uint32_t nshots, std::uint64_t seed) {
    auto backend = make_backend(SimType::FullState, proc);
    backend->set_seed(seed);
    Result r = backend->execute(circ, nshots, &nm);
    std::map<std::string, double> probs;
    for (const auto& kv : r.counts()) {
        probs[kv.first] = static_cast<double>(kv.second) / nshots;
    }
    return probs;
}

bool gpu_available() {
#if defined(QPLAYER_BUILD_GPU)
    return qplayer_cuda_device_count() > 0;
#else
    return false;
#endif
}

// Assert two probability maps agree within tol on every bitstring (union).
void expect_distributions_close(const std::map<std::string, double>& a,
                                const std::map<std::string, double>& b,
                                double tol) {
    std::set<std::string> keys;
    for (const auto& kv : a) keys.insert(kv.first);
    for (const auto& kv : b) keys.insert(kv.first);
    for (const std::string& k : keys) {
        const double pa = a.count(k) ? a.at(k) : 0.0;
        const double pb = b.count(k) ? b.at(k) : 0.0;
        EXPECT_NEAR(pa, pb, tol);
    }
}

}  // namespace

// ── Depolarizing noise: CPU vs GPU distribution parity ───────────────────────

TEST(NoiseParity, Depolarizing_BellCpuVsGpu_WithinTolerance) {
    if (!gpu_available()) return;  // skip without a CUDA device

    // Arrange — Bell circuit with depolarizing noise on both gates.
    const std::uint32_t nshots = 8000;
    const std::uint64_t seed   = 20260629ULL;
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.05).add_depolarizing("cx", 0.05);

    // Act
    const auto cpu = run_probs(ProcType::CPU, circ, nm, nshots, seed);
    const auto gpu = run_probs(ProcType::GPU, circ, nm, nshots, seed);

    // Assert — identical seed map + identical Monte Carlo algorithm -> the two
    // distributions match within ±0.01 (sampling/boundary-flip band).
    expect_distributions_close(cpu, gpu, 0.01);
}

// ── Amplitude damping: CPU vs GPU distribution parity ────────────────────────

TEST(NoiseParity, AmplitudeDamping_CpuVsGpu_WithinTolerance) {
    if (!gpu_available()) return;

    // Arrange — excited state with amplitude damping; both backends relax it.
    const std::uint32_t nshots = 8000;
    const std::uint64_t seed   = 99887766ULL;
    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);
    NoiseModel nm;
    nm.add_amplitude_damping("x", 0.2);

    // Act
    const auto cpu = run_probs(ProcType::CPU, circ, nm, nshots, seed);
    const auto gpu = run_probs(ProcType::GPU, circ, nm, nshots, seed);

    // Assert
    expect_distributions_close(cpu, gpu, 0.01);
}

// ── Leading MeasureOp: pinned host buffers must be downloaded first ──────────
//
// Audit #4: the GPU trajectory loop started with the "host buffers are current"
// flag set to false, so a MeasureOp before any gate skipped the D2H and folded
// *uninitialized* pinned memory into the Born-rule sum (or, from trajectory 1 on,
// the previous trajectory's final state). It then uploaded that garbage, wiping
// |0...0>, and the trajectory aborted with
//   SimulatorError: [FullStateGPU] numerical divergence at trajectory 0:
//   |psi|^2 = 0.000000
// while CPU produced the correct distribution. The flag now starts stale.

TEST(NoiseParity, LeadingMeasure_CpuVsGpu_WithinTolerance) {
    if (!gpu_available()) return;

    // Arrange — the reported repro shape: measure is the very first op, so
    // nothing has touched the host buffers yet.
    const std::uint32_t nshots = 8000;
    const std::uint64_t seed   = 20260729ULL;
    Circuit circ(2, 2);
    circ.measure(0, 0).h(1).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.05);

    // Act — this threw SimulatorError on GPU before the fix.
    const auto cpu = run_probs(ProcType::CPU, circ, nm, nshots, seed);
    const auto gpu = run_probs(ProcType::GPU, circ, nm, nshots, seed);

    // Assert — q0 stays |0>, q1 is an even superposition: 00/01 near 0.5 each.
    expect_distributions_close(cpu, gpu, 0.01);
    EXPECT_TRUE(gpu.count("00") > 0);
    EXPECT_NEAR(gpu.count("00") ? gpu.at("00") : 0.0, 0.5, 0.05);
}

// A measure-only circuit has no gate at all, so the end-of-trajectory norm check
// is the first reader of the host buffers — the same staleness bug reached it.
TEST(NoiseParity, MeasureOnlyCircuit_Gpu_NormCheckPasses) {
    if (!gpu_available()) return;

    const std::uint32_t nshots = 512;
    const std::uint64_t seed   = 4242ULL;
    Circuit circ(1, 1);
    circ.measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.05);  // registered but never triggered

    const auto gpu = run_probs(ProcType::GPU, circ, nm, nshots, seed);

    // |0> measured in the Z basis is deterministic: every shot reports "0".
    EXPECT_NEAR(gpu.count("0") ? gpu.at("0") : 0.0, 1.0, 1e-12);
}
