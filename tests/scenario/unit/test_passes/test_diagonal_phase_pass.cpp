// test_diagonal_phase_pass.cpp — Unit tests for DiagonalPhasePass.
// Verifies diagonal-gate phase accumulation and single-gate reduction.

#include <cmath>

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/diagonal_phase_pass.hpp"

using qplayer::Circuit;
using qplayer::DiagonalPhasePass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;

static constexpr double kPi  = 3.14159265358979323846;
static constexpr double kTol = 1e-10;

// ── Case 1: T·T → S (π/4 + π/4 = π/2) ───────────────────────────────────────

TEST(DiagonalPhasePass, TT_ReducesToS) {
    Circuit c(1);
    c.t(0).t(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::S);
}

// ── Case 2: S·S → Z (π/2 + π/2 = π) ─────────────────────────────────────────

TEST(DiagonalPhasePass, SS_ReducesToZ) {
    Circuit c(1);
    c.s(0).s(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 3: Z·Z → identity (π + π = 2π ≡ 0) → op removed ───────────────────

TEST(DiagonalPhasePass, ZZ_CollapsesToIdentity_OpRemoved) {
    Circuit c(1);
    c.z(0).z(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 4: T·Tdg → identity (π/4 - π/4 = 0) → op removed ──────────────────

TEST(DiagonalPhasePass, TTdg_CollapsesToIdentity_OpRemoved) {
    Circuit c(1);
    c.t(0).tdg(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 5: S·Sdg → identity (π/2 - π/2 = 0) → op removed ──────────────────

TEST(DiagonalPhasePass, SSdg_CollapsesToIdentity_OpRemoved) {
    Circuit c(1);
    c.s(0).sdg(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 6: T·T·T·T → Z (4 × π/4 = π) ───────────────────────────────────────

TEST(DiagonalPhasePass, TTTT_ReducesToZ) {
    Circuit c(1);
    c.t(0).t(0).t(0).t(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Z);
}

// ── Case 7: Barrier boundary — T·Barrier·T must not accumulate ───────────────

TEST(DiagonalPhasePass, Barrier_Boundary_NoAccumulation) {
    Circuit c(1);
    c.t(0).barrier({0}).t(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: t, barrier, t
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 8: Non-diagonal gate breaks the window ───────────────────────────────

TEST(DiagonalPhasePass, NonDiagonalGate_BreaksWindow) {
    // T·H·T: H is not diagonal, so T and T are in separate windows.
    Circuit c(1);
    c.t(0).h(0).t(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: t, h, t (no accumulation across H).
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 9: changed() false when only single diagonal ops (no multi-op run) ───

TEST(DiagonalPhasePass, SingleDiagonalOp_ChangedFalse) {
    Circuit c(1);
    c.t(0);

    DiagonalPhasePass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 10: Tdg·Tdg → Sdg (-π/4 - π/4 = -π/2) ─────────────────────────────

TEST(DiagonalPhasePass, TdgTdg_ReducesToSdg) {
    Circuit c(1);
    c.tdg(0).tdg(0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::Sdg);
}

// ── Case 11: RZ(π/4)·RZ(π/4) → T (π/4 + π/4 = π/2 → ... wait: S) ──────────
// RZ(π/4)·RZ(π/4): net phase = π/2 → S

TEST(DiagonalPhasePass, RZ_Accumulation_ReducesToS) {
    Circuit c(1);
    c.rz(kPi / 4.0, 0).rz(kPi / 4.0, 0);

    DiagonalPhasePass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    // net = π/4 + π/4 = π/2 → S
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::S);
}

// ── Case 12: name() returns expected string ───────────────────────────────────

TEST(DiagonalPhasePass, Name_ReturnsExpectedString) {
    DiagonalPhasePass pass;
    EXPECT_EQ(pass.name(), std::string{"DiagonalPhasePass"});
}
