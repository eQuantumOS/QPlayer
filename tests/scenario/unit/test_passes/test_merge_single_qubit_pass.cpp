// test_merge_single_qubit_pass.cpp — Unit tests for MergeSingleQubitPass.
// Verifies pattern-based merging of consecutive same-qubit 1Q gates.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/merge_single_qubit_pass.hpp"

using qplayer::Circuit;
using qplayer::MergeSingleQubitPass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;

// ── Case 1: H·X·H → Z (3-gate pattern) ───────────────────────────────────────

TEST(MergeSingleQubitPass, HXH_MergedToZ) {
    Circuit c(1);
    c.h(0).x(0).h(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    const auto& ops = out.ops();
    EXPECT_TRUE(std::get<GateOp>(ops[0]).type == GateType::Z);
}

// ── Case 2: H·Z·H → X (3-gate pattern) ───────────────────────────────────────

TEST(MergeSingleQubitPass, HZH_MergedToX) {
    Circuit c(1);
    c.h(0).z(0).h(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    const auto& ops = out.ops();
    EXPECT_TRUE(std::get<GateOp>(ops[0]).type == GateType::X);
}

// ── Case 3: S·S → Z (2-gate pattern) ─────────────────────────────────────────

TEST(MergeSingleQubitPass, SS_MergedToZ) {
    Circuit c(1);
    c.s(0).s(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 4: T·T → S (2-gate pattern) ─────────────────────────────────────────

TEST(MergeSingleQubitPass, TT_MergedToS) {
    Circuit c(1);
    c.t(0).t(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::S);
}

// ── Case 5: Tdg·Tdg → Sdg (2-gate pattern) ───────────────────────────────────

TEST(MergeSingleQubitPass, TdgTdg_MergedToSdg) {
    Circuit c(1);
    c.tdg(0).tdg(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Sdg);
}

// ── Case 6: Sdg·Sdg → Z (2-gate pattern) ─────────────────────────────────────

TEST(MergeSingleQubitPass, SdgSdg_MergedToZ) {
    Circuit c(1);
    c.sdg(0).sdg(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 7: H·X unchanged (no matching pattern for 2-gate H·X) ───────────────

TEST(MergeSingleQubitPass, HX_NoPattern_Unchanged) {
    Circuit c(1);
    c.h(0).x(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 8: Barrier boundary — H·X·H across barrier must not merge ───────────

TEST(MergeSingleQubitPass, Barrier_Boundary_NoMerge) {
    Circuit c(1);
    // h, barrier, x, h: barrier breaks the 3-gate window
    c.h(0).barrier({0}).x(0).h(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // h + barrier + x + h = 4 ops (no merge across barrier).
    EXPECT_EQ(out.num_ops(), std::size_t{4});
    EXPECT_FALSE(pass.changed());
}

// ── Case 9: Rotation gate acts as boundary ────────────────────────────────────

TEST(MergeSingleQubitPass, RotationGate_Boundary_NoMerge) {
    Circuit c(1);
    c.h(0).rz(1.0, 0).h(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // rz acts as boundary for the window — no merge.
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 10: changed() false when no pattern matches ─────────────────────────

TEST(MergeSingleQubitPass, Changed_FalseWhenNoMatch) {
    Circuit c(2);
    c.h(0).cx(0, 1);

    MergeSingleQubitPass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 11: Multi-qubit gate breaks per-qubit window ────────────────────────

TEST(MergeSingleQubitPass, CXGate_BreaksWindowForInvolvedQubits) {
    // h(0), cx(0,1), h(0): cx involves qubit 0, so the window for q0 is
    // broken by cx and H·...·H cannot span cx.
    Circuit c(2);
    c.h(0).cx(0, 1).h(0);

    MergeSingleQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: h, cx, h (no merge across cx for qubit 0).
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 12: name() returns expected string ───────────────────────────────────

TEST(MergeSingleQubitPass, Name_ReturnsExpectedString) {
    MergeSingleQubitPass pass;
    EXPECT_EQ(pass.name(), std::string{"MergeSingleQubitPass"});
}
