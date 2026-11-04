// test_pattern_replace_pass.cpp — Unit tests for PatternReplacePass.
// Verifies all 9 pattern-matching rules plus boundary and changed() semantics.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/pattern_replace_pass.hpp"

using qplayer::Circuit;
using qplayer::PatternReplacePass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;

// ── Case 1: T·T → S (2-gate pattern) ─────────────────────────────────────────

TEST(PatternReplacePass, TT_ReducesToS) {
    Circuit c(1);
    c.t(0).t(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::S);
}

// ── Case 2: S·S → Z (2-gate pattern) ─────────────────────────────────────────

TEST(PatternReplacePass, SS_ReducesToZ) {
    Circuit c(1);
    c.s(0).s(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 3: T·T·T·T → Z (4-gate pattern) ─────────────────────────────────────

TEST(PatternReplacePass, TTTT_ReducesToZ) {
    Circuit c(1);
    c.t(0).t(0).t(0).t(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    // T·T → S; then S·T·T is two more t's from the original second T·T pair
    // but after first T·T merges to S the remaining two T's form another T·T → S
    // Actually the 4-gate pattern T·T·T·T should fire as a 4-gate match first.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 4: Tdg·Tdg → Sdg (2-gate pattern) ───────────────────────────────────

TEST(PatternReplacePass, TdgTdg_ReducesToSdg) {
    Circuit c(1);
    c.tdg(0).tdg(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Sdg);
}

// ── Case 5: Sdg·Sdg → Z (2-gate pattern) ─────────────────────────────────────

TEST(PatternReplacePass, SdgSdg_ReducesToZ) {
    Circuit c(1);
    c.sdg(0).sdg(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 6: H·H → identity (ops removed) ─────────────────────────────────────

TEST(PatternReplacePass, HH_CollapsesToIdentity) {
    Circuit c(1);
    c.h(0).h(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 7: X·X → identity (ops removed) ─────────────────────────────────────

TEST(PatternReplacePass, XX_CollapsesToIdentity) {
    Circuit c(1);
    c.x(0).x(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 8: H·X·H → Z (3-gate pattern) ───────────────────────────────────────

TEST(PatternReplacePass, HXH_ReducesToZ) {
    Circuit c(1);
    c.h(0).x(0).h(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 9: H·Z·H → X (3-gate pattern) ───────────────────────────────────────

TEST(PatternReplacePass, HZH_ReducesToX) {
    Circuit c(1);
    c.h(0).z(0).h(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::X);
}

// ── Case 10: barrier boundary — T·barrier·T not matched ──────────────────────

TEST(PatternReplacePass, Barrier_Boundary_PreventsMerge) {
    Circuit c(1);
    c.t(0).barrier({0}).t(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: t, barrier, t
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 11: changed() false when no pattern matches ─────────────────────────

TEST(PatternReplacePass, ChangedFlag_FalseWhenNoMatch) {
    // H·X·Z — no matching pattern
    Circuit c(1);
    c.h(0).x(0).z(0);

    PatternReplacePass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 12: name() returns expected string ───────────────────────────────────

TEST(PatternReplacePass, Name_ReturnsExpectedString) {
    PatternReplacePass pass;
    EXPECT_EQ(pass.name(), std::string{"PatternReplacePass"});
}

// ── Case 13: multi-qubit circuit — pattern on one qubit, others untouched ────

TEST(PatternReplacePass, MultiQubit_PatternOnOneQubit_OthersUntouched) {
    // q[0]: T·T → S;  q[1]: X (unchanged)
    Circuit c(2);
    c.t(0).t(0).x(1);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    // T·T matched → S on q[0], X on q[1] remain → 2 ops
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_TRUE(pass.changed());
}

// ── Case 14: measure boundary — T·measure·T not matched ──────────────────────

TEST(PatternReplacePass, Measure_Boundary_PreventsMerge) {
    Circuit c(1, 1);
    c.t(0).measure(0, 0).t(0);

    PatternReplacePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: t, measure, t
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}
