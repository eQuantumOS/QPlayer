// test_remove_idle_qubit_pass.cpp — Unit tests for RemoveIdleQubitPass.
// Verifies that gates acting on unmeasured, non-entangled qubits are removed
// while measured and entangled qubits (and their gates) are preserved.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/remove_idle_qubit_pass.hpp"

using qplayer::Circuit;
using qplayer::RemoveIdleQubitPass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;
using qplayer::detail_circuit_push_op;

// ── Case 1: purely idle qubit gate removed ────────────────────────────────────

TEST(RemoveIdleQubitPass, PurelyIdleQubit_GateRemoved) {
    // q[0]: H·X (never measured, no entanglement) → removed
    // q[1]: H → measure(1,0) → kept
    Circuit c(2, 1);
    c.h(0).x(0).h(1).measure(1, 0);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // H(0) and X(0) removed; H(1) and measure(1,0) remain
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_TRUE(pass.changed());
}

// ── Case 2: all qubits measured — nothing removed ─────────────────────────────

TEST(RemoveIdleQubitPass, AllQubitsMeasured_NothingRemoved) {
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{4});
    EXPECT_FALSE(pass.changed());
}

// ── Case 3: CX with measured target — control qubit kept ─────────────────────

TEST(RemoveIdleQubitPass, CX_MeasuredTarget_ControlKept) {
    // q[0] ctrl (not directly measured), q[1] tgt (measured)
    // CX(0,1) → q[0] is entangled with q[1] → kept
    Circuit c(2, 1);
    c.h(0).cx(0, 1).measure(1, 0);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // All 3 ops (H, CX, measure) preserved because q[0] is kept via entanglement
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 4: idle qubit before entanglement — idle gate removed ────────────────

TEST(RemoveIdleQubitPass, IdleGateBeforeEntanglement_RemovedWhenQubitStaysIdle) {
    // q[0]: X gate only (no measure, no entanglement)
    // q[1]: measure only
    Circuit c(2, 1);
    c.x(0).measure(1, 0);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // X(0) removed; measure(1,0) remains
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::holds_alternative<qplayer::MeasureOp>(out.ops()[0]));
}

// ── Case 5: changed() false when nothing removed ──────────────────────────────

TEST(RemoveIdleQubitPass, ChangedFlag_FalseWhenNothingRemoved) {
    Circuit c(1, 1);
    c.h(0).measure(0, 0);

    RemoveIdleQubitPass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 6: name() returns expected string ────────────────────────────────────

TEST(RemoveIdleQubitPass, Name_ReturnsExpectedString) {
    RemoveIdleQubitPass pass;
    EXPECT_EQ(pass.name(), std::string{"RemoveIdleQubitPass"});
}

// ── Case 7: num_qubits preserved even after idle gate removal ─────────────────

TEST(RemoveIdleQubitPass, NumQubits_PreservedAfterRemoval) {
    Circuit c(3, 1);
    // q[0] idle, q[1] idle, q[2] measured
    c.x(0).y(1).h(2).measure(2, 0);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // num_qubits stays 3 (qreg declaration not changed)
    EXPECT_EQ(out.num_qubits(), qplayer::QubitIndex{3});
    EXPECT_TRUE(pass.changed());
}

// ── Case 8: barrier preserved conservatively ──────────────────────────────────

TEST(RemoveIdleQubitPass, Barrier_PreservedConservatively) {
    // Even if q[0] is idle, barrier{0} is kept (conservative policy)
    Circuit c(2, 1);
    c.barrier({0, 1}).measure(1, 0);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // barrier and measure both present (changed_ stays false — no GateOps removed)
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 9: 3-qubit circuit with 2 idle qubits — both idle gates removed ──────

TEST(RemoveIdleQubitPass, ThreeQubits_TwoIdle_BothRemoved) {
    // q[0] and q[2] idle; q[1] measured
    Circuit c(3, 1);
    c.x(0).z(2).measure(1, 0);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // X(0) and Z(2) removed; measure(1,0) remains
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
}

// ── Case 10: opaque GateOp on idle qubit is preserved (-2) ────────
//
// Opaque gates have unknown semantics so the pass must never silently delete
// them even when every operand qubit looks idle. The other eight F-OPT passes
// enforce the same guard; this case pins it for RemoveIdleQubitPass too.

TEST(RemoveIdleQubitPass, OpaqueGate_OnIdleQubit_Preserved) {
    Circuit c(2, 1);
    c.declare_opaque("myop", 1);

    // q[0] hosts an opaque gate and is never measured/entangled with q[1].
    // Without the is_opaque guard, the pass would treat q[0] as idle and
    // drop the gate.
    GateOp opaque_g;
    opaque_g.type        = GateType::Opaque;
    opaque_g.name        = "myop";
    opaque_g.opaque_name = "myop";
    opaque_g.qubits      = {0};
    opaque_g.is_opaque   = true;
    detail_circuit_push_op(c, Op{opaque_g});

    c.h(1).measure(1, 0);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // Opaque op + H(1) + measure(1,0) all remain — no silent deletion.
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 11: no observation declared at all — pass skipped ────────
//
// This is the audit finding #3 repro reduced to pass level: a Bell circuit with
// no measure was wiped to zero ops, so <XX> read back +1 -> 0. A statevector or
// expectation read happens after run() and is invisible here, so a circuit that
// declares no observation must be left untouched.

TEST(RemoveIdleQubitPass, NoObservationDeclared_PassSkipped) {
    Circuit c(2);
    c.h(0).cx(0, 1);

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 12: expectation target keeps its qubits ──────────────────

TEST(RemoveIdleQubitPass, ExpectationTarget_KeepsNamedQubits) {
    // No measure anywhere, but <ZZ> is pre-registered so both qubits
    // are observed and neither gate may be dropped.
    Circuit c(2);
    c.h(0).cx(0, 1).add_expectation("ZZ");

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 13: partial Pauli — only the named qubit is observed ─────

TEST(RemoveIdleQubitPass, PartialPauliTarget_UnnamedQubitsStillIdle) {
    // "IIZ" in Qiskit big-endian order names qubit 0 only: pauli[0]
    // acts on the highest qubit (2), pauli[2] on qubit 0. So gates on q[1]/q[2]
    // stay idle and are dropped, while H(0) is kept.
    Circuit c(3);
    c.h(0).x(1).y(2).add_expectation("IIZ");

    RemoveIdleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_EQ(out.num_qubits(), qplayer::QubitIndex{3});  // num_qubits unchanged
}
