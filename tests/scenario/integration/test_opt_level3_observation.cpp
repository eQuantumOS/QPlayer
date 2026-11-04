// test_opt_level3_observation.cpp — optimization_level=3 must not change what an
// observation reports (Phase 8-F).
//
// Regression for the 2026-07-28 audit finding #3: RemoveIdleQubitPass treated
// MeasureOp as the only observation, so a measure-less circuit was wiped to zero
// ops at level 3 and <XX> silently read +1 -> 0 while <ZZ> stayed +1 by accident
// (|00> has <ZZ> = +1 too, which is why the bug hid). These cases pin the four
// reported repros: bare Bell, barrier-split rotations, cx-split rotations, and
// the pre-registered expectation target.
//
// Levels 0..3 must agree because none of these circuits has an unobserved qubit
// that a correct pass could drop.
//

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"
#include "../../framework/backend_params.hpp"  // TEST_BACKENDS,

#include <string>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"

using qplayer::Circuit;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::Result;
using qplayer::SimType;
using qplayer::StateVector;

namespace {

// Runs `circ` through transpile(level) and returns the resulting statevector.
// nshots=1: the fast path keeps |psi_final> without sampling cost.
StateVector statevector_at_level(SimType sim, ProcType proc, const Circuit& circ,
                                 int level) {
    QRegister qr(sim, proc);
    qr.set_seed(1);
    Circuit compiled = qr.transpile(circ, level);
    qr.run(compiled, 1);
    return qr.get_statevector();
}

// Max |a - b| over two equal-length amplitude vectors.
double max_abs_diff(const StateVector& a, const StateVector& b) {
    if (a.size() != b.size()) return 1.0;  // size mismatch is a hard failure
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = std::abs(a[i] - b[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

}  // namespace

// ── Bell state, no measure — the headline repro ───────────────────────────────
//
// Before this yielded ops=0 and statevector [1,0,0,0] at level 3.

TEST_BACKENDS(OptLevel3Observation, BareBell_AllLevelsAgree) {
    // Arrange
    Circuit circ(2);
    circ.h(0).cx(0, 1);

    // Act
    const StateVector ref = statevector_at_level(SimType::FullState, proc, circ, 0);
    const StateVector l3  = statevector_at_level(SimType::FullState, proc, circ, 3);

    // Assert: level 3 reproduces level 0 exactly (same gate sequence, tol 1e-12
    // covers only floating-point reassociation, not a dropped gate).
    EXPECT_NEAR(max_abs_diff(ref, l3), 0.0, 1e-12);
    // Bell amplitudes survive: |00> and |11> at 1/sqrt(2), |01>/|10> at 0.
    EXPECT_NEAR(std::abs(l3[0]), 0.70710678118654752, 1e-12);
    EXPECT_NEAR(std::abs(l3[3]), 0.70710678118654752, 1e-12);
}

// <XX> on the Bell state is the value the audit saw collapse to 0 at level 3.
TEST_BACKENDS(OptLevel3Observation, BareBell_ExpectationXX_StaysOne) {
    Circuit circ(2);
    circ.h(0).cx(0, 1);

    QRegister qr(SimType::FullState, proc);
    qr.set_seed(1);
    Circuit compiled = qr.transpile(circ, 3);
    qr.run(compiled, 1);

    // <XX> = +1 and <ZZ> = +1 on |Phi+>; <ZZ> alone cannot detect the bug since
    // |00> also gives +1 (that accidental match is why it went unnoticed).
    EXPECT_NEAR(qr.expectation_value("XX"), 1.0, 1e-10);
    EXPECT_NEAR(qr.expectation_value("ZZ"), 1.0, 1e-10);
}

// ── Pre-registered expectation target keeps its qubits ───────────────────────

TEST_BACKENDS(OptLevel3Observation, RegisteredExpectation_SurvivesLevel3) {
    Circuit circ(2);
    circ.h(0).cx(0, 1).add_expectation("XX");

    QRegister qr(SimType::FullState, proc);
    qr.set_seed(1);
    Circuit compiled = qr.transpile(circ, 3);
    Result  r        = qr.run(compiled, 1);

    EXPECT_TRUE(r.has_expectation("XX"));
    EXPECT_NEAR(r.expectation("XX"), 1.0, 1e-10);
}

// ── Rotation runs split by a barrier / a cx — reported maxdiff 0.68 ~ 1.0 ─────

TEST_BACKENDS(OptLevel3Observation, BarrierSplitRotations_AllLevelsAgree) {
    // h; barrier; rz(1.0); rz(0.5); barrier; h  on a single qubit.
    Circuit circ(1);
    circ.h(0).barrier({0}).rz(1.0, 0).rz(0.5, 0).barrier({0}).h(0);

    const StateVector ref = statevector_at_level(SimType::FullState, proc, circ, 0);
    for (int level : {1, 2, 3}) {
        const StateVector sv = statevector_at_level(SimType::FullState, proc, circ, level);
        EXPECT_NEAR(max_abs_diff(ref, sv), 0.0, 1e-12);
    }
}

TEST_BACKENDS(OptLevel3Observation, CxSplitRotations_AllLevelsAgree) {
    // h; cx; rz(1.0); rz(0.5); cx; h — level 3 previously removed every op.
    Circuit circ(2);
    circ.h(0).cx(0, 1).rz(1.0, 0).rz(0.5, 0).cx(0, 1).h(0);

    const StateVector ref = statevector_at_level(SimType::FullState, proc, circ, 0);
    const StateVector l3  = statevector_at_level(SimType::FullState, proc, circ, 3);
    EXPECT_NEAR(max_abs_diff(ref, l3), 0.0, 1e-12);
}

// ── Measured circuits keep the optimization ──────────────────────────────────
//
// The skip gate must not disable where it is legitimate: q[1] here is
// unobserved, so its gate is still dropped at level 3.

TEST(OptLevel3Observation, MeasuredCircuit_IdleQubitGateStillDropped) {
    Circuit circ(2, 1);
    circ.h(0).x(1).measure(0, 0);

    QRegister qr(SimType::FullState, ProcType::CPU);
    Circuit compiled = qr.transpile(circ, 3);

    // H(0) + measure(0,0) remain; X(1) is dropped (q[1] is never observed).
    EXPECT_EQ(compiled.num_ops(), std::size_t{2});
    EXPECT_EQ(compiled.num_qubits(), qplayer::QubitIndex{2});  // num_qubits unchanged
}
