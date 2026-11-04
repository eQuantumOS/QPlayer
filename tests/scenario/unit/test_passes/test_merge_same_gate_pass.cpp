// test_merge_same_gate_pass.cpp — Unit tests for MergeSameGatePass.
// Verifies same-kind 1Q gate pair merging: self-inverse collapse, fixed phase
// table, and rotation angle accumulation.

#include <cmath>

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/merge_same_gate_pass.hpp"

using qplayer::Circuit;
using qplayer::MergeSameGatePass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;

static constexpr double kPi  = 3.14159265358979323846;
static constexpr double kTol = 1e-10;

// ── Case 1: X·X → identity (op removed) ─────────────────────────────────────

TEST(MergeSameGatePass, XX_CollapsesToIdentity) {
    Circuit c(1);
    c.x(0).x(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    // Both ops removed (identity has no explicit op in the output)
    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 2: H·H → identity (op removed) ─────────────────────────────────────

TEST(MergeSameGatePass, HH_CollapsesToIdentity) {
    Circuit c(1);
    c.h(0).h(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 3: S·S → Z ──────────────────────────────────────────────────────────

TEST(MergeSameGatePass, SS_ReducesToZ) {
    Circuit c(1);
    c.s(0).s(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 4: T·T → S ──────────────────────────────────────────────────────────

TEST(MergeSameGatePass, TT_ReducesToS) {
    Circuit c(1);
    c.t(0).t(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::S);
}

// ── Case 5: Tdg·Tdg → Sdg ────────────────────────────────────────────────────

TEST(MergeSameGatePass, TdgTdg_ReducesToSdg) {
    Circuit c(1);
    c.tdg(0).tdg(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Sdg);
}

// ── Case 6: RZ(π/4)·RZ(π/4) → RZ(π/2) (angle accumulation) ─────────────────

TEST(MergeSameGatePass, RZ_RZ_AccumulatesAngle) {
    Circuit c(1);
    c.rz(kPi / 4.0, 0).rz(kPi / 4.0, 0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    const GateOp& g = std::get<GateOp>(out.ops()[0]);
    EXPECT_TRUE(g.type == GateType::RZ);
    // accumulated angle should be π/2 within tolerance
    EXPECT_NEAR(g.params[0], kPi / 2.0, kTol);
}

// ── Case 7: U1(π/6)·U1(π/6) → U1(π/3) (angle accumulation) ─────────────────

TEST(MergeSameGatePass, U1_U1_AccumulatesAngle) {
    Circuit c(1);
    c.u1(kPi / 6.0, 0).u1(kPi / 6.0, 0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    const GateOp& g = std::get<GateOp>(out.ops()[0]);
    EXPECT_TRUE(g.type == GateType::U1);
    EXPECT_NEAR(g.params[0], kPi / 3.0, kTol);
}

// ── Case 8: mixed-kind pair not merged ────────────────────────────────────────

TEST(MergeSameGatePass, MixedKind_NotMerged) {
    // H·X: different kinds — should not merge
    Circuit c(1);
    c.h(0).x(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 9: barrier boundary prevents merge across it ────────────────────────

TEST(MergeSameGatePass, Barrier_Boundary_PreventsMerge) {
    // X·barrier·X — barrier acts as merge boundary
    Circuit c(1);
    c.x(0).barrier({0}).x(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: x, barrier, x
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 10: changed() false when no merges occur ────────────────────────────

TEST(MergeSameGatePass, ChangedFlag_FalseWhenNoMerge) {
    Circuit c(2, 2);  // 2 classical bits for measure(0,0) and measure(1,1)
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    MergeSameGatePass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 11: name() returns expected string ───────────────────────────────────

TEST(MergeSameGatePass, Name_ReturnsExpectedString) {
    MergeSameGatePass pass;
    EXPECT_EQ(pass.name(), std::string{"MergeSameGatePass"});
}

// ── Case 12: Y·Y → identity ──────────────────────────────────────────────────

TEST(MergeSameGatePass, YY_CollapsesToIdentity) {
    Circuit c(1);
    c.y(0).y(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 13: Z·Z → identity ──────────────────────────────────────────────────

TEST(MergeSameGatePass, ZZ_CollapsesToIdentity) {
    Circuit c(1);
    c.z(0).z(0);

    MergeSameGatePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}
