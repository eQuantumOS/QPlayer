// test_golden_gpu.cpp — FullState GPU Golden test (noiseless path).
// Verifies the GPU backend reproduces the CPU backend's Bell/GHZ/QFT results:
// statevector amplitudes match within ±1e-10 (exact arithmetic) and measurement
// distributions match theory within the nshots=10000 statistical tolerance.
// Compiled/run only when QPLAYER_BUILD_GPU is defined and a CUDA device exists.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/qregister.hpp"
#include "../../../include/types.hpp"

using qplayer::Circuit;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::SimType;
using qplayer::StateVector;

#if defined(QPLAYER_BUILD_GPU)

extern "C" int qplayer_cuda_device_count();

namespace {

// Measure-free circuits so get_statevector() returns the final |psi> directly.
Circuit bell_state_circuit() {
    Circuit c(2);
    c.h(0).cx(0, 1);
    return c;
}

Circuit ghz5_state_circuit() {
    Circuit c(5);
    c.h(0).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4);
    return c;
}

// 4-qubit QFT (matches make_qft4() in test_golden.cpp): H + cu1 ladder + swaps.
Circuit qft4_circuit() {
    const double pi = 3.14159265358979323846;
    Circuit c(4);
    c.h(0);
    c.cu1(pi / 2.0, 1, 0);
    c.cu1(pi / 4.0, 2, 0);
    c.cu1(pi / 8.0, 3, 0);
    c.h(1);
    c.cu1(pi / 2.0, 2, 1);
    c.cu1(pi / 4.0, 3, 1);
    c.h(2);
    c.cu1(pi / 2.0, 3, 2);
    c.h(3);
    c.swap(0, 3);
    c.swap(1, 2);
    return c;
}

// Run a measure-free circuit on the given proc type and return |psi_final>.
StateVector run_statevector(const Circuit& circ, ProcType proc) {
    QRegister qr(SimType::FullState, proc);
    qr.run(circ, 1);  // nshots=1, no measurement
    return qr.get_statevector();
}

// Assert two statevectors agree within ±1e-10 (exact arithmetic).
void expect_statevectors_match(const StateVector& a, const StateVector& b) {
    EXPECT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(a[i].real(), b[i].real(), 1e-10);
        EXPECT_NEAR(a[i].imag(), b[i].imag(), 1e-10);
    }
}

bool gpu_present() { return qplayer_cuda_device_count() > 0; }

}  // namespace

// ── Statevector parity (GPU vs CPU, ±1e-10) ─────────────────────────────────

TEST(GoldenGpu, Bell_Statevector_MatchesCpu) {
    if (!gpu_present()) return;
    const Circuit c = bell_state_circuit();
    expect_statevectors_match(run_statevector(c, ProcType::GPU),
                              run_statevector(c, ProcType::CPU));
}

TEST(GoldenGpu, GHZ5_Statevector_MatchesCpu) {
    if (!gpu_present()) return;
    const Circuit c = ghz5_state_circuit();
    expect_statevectors_match(run_statevector(c, ProcType::GPU),
                              run_statevector(c, ProcType::CPU));
}

TEST(GoldenGpu, QFT4_Statevector_MatchesCpu) {
    if (!gpu_present()) return;
    const Circuit c = qft4_circuit();
    const StateVector gpu = run_statevector(c, ProcType::GPU);
    const StateVector cpu = run_statevector(c, ProcType::CPU);
    expect_statevectors_match(gpu, cpu);
    // QFT|0000> is the uniform superposition: every amplitude is 0.25 (real).
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        EXPECT_NEAR(gpu[i].real(), 0.25, 1e-10);
        EXPECT_NEAR(gpu[i].imag(), 0.0, 1e-10);
    }
}

// ── Measurement distribution (GPU backend, nshots=10000 ±0.01) ──────────────

TEST(GoldenGpu, Bell_Counts_GpuDistributionMatchesTheory) {
    if (!gpu_present()) return;
    QRegister qr(SimType::FullState, ProcType::GPU);
    qr.set_seed(12345);
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    const auto counts = qr.run(c, 10000).counts();
    auto frac = [&](const std::string& k) {
        const auto it = counts.find(k);
        return it == counts.end() ? 0.0 : static_cast<double>(it->second) / 10000.0;
    };
    EXPECT_NEAR(frac("00"), 0.5, 0.01);  // Bell: |00> + |11>, 50% each
    EXPECT_NEAR(frac("11"), 0.5, 0.01);
    EXPECT_NEAR(frac("01"), 0.0, 0.01);
    EXPECT_NEAR(frac("10"), 0.0, 0.01);
}

TEST(GoldenGpu, GHZ5_Counts_GpuDistributionMatchesTheory) {
    if (!gpu_present()) return;
    QRegister qr(SimType::FullState, ProcType::GPU);
    qr.set_seed(67890);
    Circuit c(5, 5);
    c.h(0).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4).measure_all();

    const auto counts = qr.run(c, 10000).counts();
    auto frac = [&](const std::string& k) {
        const auto it = counts.find(k);
        return it == counts.end() ? 0.0 : static_cast<double>(it->second) / 10000.0;
    };
    EXPECT_NEAR(frac("00000"), 0.5, 0.01);  // GHZ: |0...0> + |1...1>, 50% each
    EXPECT_NEAR(frac("11111"), 0.5, 0.01);
}

#endif  // QPLAYER_BUILD_GPU
