// test_cancel_inverse_pass.cpp — Unit tests for CancelInversePass.
// Verifies adjacent self-inverse / mutually-inverse gate pair cancellation.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/cancel_inverse.hpp"

using qplayer::Circuit;
using qplayer::CancelInversePass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;
using qplayer::detail_circuit_push_op;

// ── Case 1: H·H on same qubit → 0 ops (cancel) ──────────────────────────────

TEST(CancelInversePass, HH_SameQubit_Cancelled) {
    Circuit c(1);
    c.h(0).h(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 2: X·X on same qubit → 0 ops ────────────────────────────────────────

TEST(CancelInversePass, XX_SameQubit_Cancelled) {
    Circuit c(1);
    c.x(0).x(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 3: S·Sdg on same qubit → 0 ops (mutually inverse) ──────────────────

TEST(CancelInversePass, SSdg_SameQubit_Cancelled) {
    Circuit c(1);
    c.s(0).sdg(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 4: T·Tdg on same qubit → 0 ops (mutually inverse) ──────────────────

TEST(CancelInversePass, TTdg_SameQubit_Cancelled) {
    Circuit c(1);
    c.t(0).tdg(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 5: H·X different gates — no cancellation ────────────────────────────

TEST(CancelInversePass, HX_NoCancellation) {
    Circuit c(1);
    c.h(0).x(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 6: Barrier boundary — H·Barrier·H must not cancel ──────────────────

TEST(CancelInversePass, Barrier_Boundary_HBarrierH_NoCancellation) {
    // Arrange: h(0), barrier({0}), h(0)
    Circuit c(1);
    c.h(0).barrier({0}).h(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops survive: h, barrier, h
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 7: changed() false when no cancellation occurs ──────────────────────

TEST(CancelInversePass, ChangedFalse_WhenNoOpsModified) {
    Circuit c(2);
    c.h(0).cx(0, 1);

    CancelInversePass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 8: CX·CX same ctrl/tgt → cancelled ──────────────────────────────────

TEST(CancelInversePass, CXCX_SameQubits_Cancelled) {
    Circuit c(2);
    c.cx(0, 1).cx(0, 1);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 9: CX with different qubits — no cancellation ───────────────────────

TEST(CancelInversePass, CX_DifferentQubits_NoCancellation) {
    Circuit c(3);
    // cx(0,1) then cx(1,2) — different qubit sets, no cancel
    c.cx(0, 1).cx(1, 2);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── ECR is self-inverse (Hermitian, ECR^2 = I): same operands cancel ─

TEST(CancelInversePass, EcrEcr_SameOperands_Cancelled) {
    Circuit c(2);
    c.ecr(0, 1).ecr(0, 1);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── ECR is NOT symmetric: reversed operands must NOT cancel ────────

TEST(CancelInversePass, EcrEcr_ReversedOperands_NoCancellation) {
    Circuit c(2);
    c.ecr(0, 1).ecr(1, 0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 10: H·H·H → single H (first pair cancels, one survives) ─────────────

TEST(CancelInversePass, HHH_ReducesToOneH) {
    Circuit c(1);
    c.h(0).h(0).h(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    // First two H cancel; third H survives.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    const auto& ops = out.ops();
    EXPECT_TRUE(std::holds_alternative<qplayer::GateOp>(ops[0]));
    EXPECT_TRUE(std::get<qplayer::GateOp>(ops[0]).type == GateType::H);
}

// ── Case 11: Measure boundary — H·Measure·H must not cancel ──────────────────

TEST(CancelInversePass, Measure_Boundary_HMeasureH_NoCancellation) {
    Circuit c(1, 1);
    c.h(0).measure(0, 0).h(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops survive: h, measure, h
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 12: Rotation gate acts as boundary — H·RZ·H must not cancel ─────────

TEST(CancelInversePass, RotationGate_Boundary_HRzH_NoCancellation) {
    Circuit c(1);
    c.h(0).rz(1.0, 0).h(0);

    CancelInversePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops survive: h, rz, h (rz acts as a boundary, so the two H's are in
    // separate windows and cannot cancel).
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}
