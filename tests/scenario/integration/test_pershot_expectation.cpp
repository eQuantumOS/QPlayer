// test_pershot_expectation.cpp — nshot-averaged Result.expectation on the
// stochastic execution paths.
//
// model A defines Result.expectation as <P> = (1/N) * sum_j
// <psi_j|P|psi_j> over trajectories. On the fast path a single final
// state exists so one evaluation suffices (covered by test_bell_expectation).
// These tests cover the two stochastic paths, where each shot ends in a
// *different* state and only a per-shot average is meaningful:
// - per-shot path (IfOp / classical control)
// - trajectory path (NoiseModel present)
// Until per-shot wiring these paths skipped accumulation entirely and
// left the dict empty, so has_expectation() assertions below are the regression
// guard. The <IZ> case additionally discriminates the correct nshot average
// (~0) from the last-shot value (+-1) that a naive implementation would report.
// Arrange/Act/Assert; EXPECT_NEAR tolerance rationale.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"
#include "../../framework/backend_params.hpp"  // TEST_BACKENDS,

#include <cmath>
#include <cstdint>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/qregister.hpp"
#include "../../../include/result.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::Result;
using qplayer::SimType;

namespace {

// h(q0); measure q0->c0; if(c0==1) x(q1) — q1 ends up equal to q0 every shot.
// Classical control routes execution to the per-shot path.
Circuit correlated_ifop_circuit() {
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    GateOp x_gate;
    x_gate.type   = GateType::X;
    x_gate.qubits = {1};
    circ.if_then(0, 1, x_gate);
    circ.measure(1, 1);
    return circ;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// per-shot path (IfOp)
// ─────────────────────────────────────────────────────────────────────────────

// <ZZ> on the IfOp-correlated state is +1 in *every* shot (both qubits collapse
// to the same basis value), so the shot average is exactly +1 — an algebraic
// check with no statistical slack.
TEST_BACKENDS(PerShotExpectation, IfOpPath_CorrelatedZZ_AveragesToOne) {
    // Arrange
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    qr.set_seed(7);
    Circuit circ = correlated_ifop_circuit();
    circ.add_expectation("ZZ");
    EXPECT_TRUE(circ.has_classical_control());  // confirms the route

    // Act
    Result r = qr.run(circ, 400);

    // Assert: every shot contributes exactly +1, so the mean is exact.
    // 1e-10 = algebraic identity, not a statistical bound (testing-rules).
    EXPECT_TRUE(r.has_expectation("ZZ"));
    EXPECT_NEAR(r.expectation("ZZ"), 1.0, 1e-10);
}

// <IZ> (Z on q0, Qiskit order) is +1 or -1 with equal probability
// shot, so the nshot average is ~0. This separates the quantity
// the last-shot value: reporting a single trajectory would give +-1 here.
TEST_BACKENDS(PerShotExpectation, IfOpPath_SingleQubitZ_AveragesToZero) {
    // Arrange
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    qr.set_seed(7);
    Circuit circ = correlated_ifop_circuit();
    circ.add_expectation("IZ");  // Z acts on qubit 0 (pauli[n-1-k] order)

    // Act
    const std::uint32_t nshots = 4000;
    Result r = qr.run(circ, nshots);

    // Assert: each shot draws +-1 with p=0.5, so the mean has sigma =
    // 1/sqrt(4000) ~= 0.0158. A 0.08 bound is ~5 sigma — statistical, and far
    // tighter than the +-1 a last-shot implementation would return.
    EXPECT_TRUE(r.has_expectation("IZ"));
    EXPECT_NEAR(r.expectation("IZ"), 0.0, 0.08);
}

// ─────────────────────────────────────────────────────────────────────────────
// Monte Carlo trajectory path (NoiseModel)
// ─────────────────────────────────────────────────────────────────────────────

// Amplitude damping on |1> gives a two-outcome trajectory ensemble whose <Z>
// average is analytically 2*gamma - 1. Runs on CPU and GPU (both have their own
// trajectory loop, and both must produce the same nshot average).
TEST_BACKENDS(PerShotExpectation, NoisyPath_AmplitudeDampingZ_MatchesTheory) {
    // Arrange — X takes |0> to |1>, then amplitude_damping(gamma) relaxes it to
    // |0> with probability gamma. Each trajectory ends in a basis state, so its
    // <Z> is +1 (relaxed) or -1 (survived) and the ensemble mean is
    //   <Z> = gamma*(+1) + (1-gamma)*(-1) = 2*gamma - 1 = 0.8 at gamma = 0.9.
    const double        gamma  = 0.9;
    const std::uint32_t nshots = 4000;

    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(1, 1);
    circ.x(0).measure(0, 0);
    circ.add_expectation("Z");

    NoiseModel nm;
    nm.add_amplitude_damping("x", gamma);
    qr.set_seed(2026);
    qr.set_noise_model(nm);

    // Act
    Result r = qr.run(circ, nshots, /*enable_fusion=*/false);

    // Assert: samples are +-1 with p(+1) = gamma, so variance = 1 - (2g-1)^2 =
    // 0.36 and sigma = 0.6/sqrt(4000) ~= 0.0095. A 0.05 bound is ~5 sigma.
    EXPECT_TRUE(r.has_expectation("Z"));
    EXPECT_NEAR(r.expectation("Z"), 2.0 * gamma - 1.0, 0.05);
}

// The trajectory average must not depend on thread count: samples are summed in
// trajectory order rather than reduced per thread (double addition is
// not associative). Two runs with the same seed must agree bit-for-bit.
TEST(PerShotExpectation, NoisyPath_SameSeed_ExpectationBitReproducible) {
    // Arrange
    auto run_once = [](std::uint64_t seed) {
        QRegister qr(SimType::FullState, ProcType::CPU);
        Circuit circ(2, 2);
        circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
        circ.add_expectation("ZZ");
        NoiseModel nm;
        nm.add_depolarizing("cx", 0.3);
        qr.set_seed(seed);
        qr.set_noise_model(nm);
        return qr.run(circ, 1000, /*enable_fusion=*/false).expectation("ZZ");
    };

    // Act
    const double a = run_once(31337);
    const double b = run_once(31337);

    // Assert: identical seed -> identical trajectories -> identical sum order.
    // 0.0 tolerance asserts bit equality, which the ordered sum guarantees.
    EXPECT_NEAR(a, b, 0.0);
}
