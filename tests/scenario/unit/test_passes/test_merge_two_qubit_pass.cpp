// test_merge_two_qubit_pass.cpp — Unit tests for MergeTwoQubitPass.
// Verifies same-kind 2Q gate pair merging: self-inverse cancellation, rotation
// accumulation, qubit-pair order sensitivity, and boundary semantics.

#include <cmath>

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/merge_two_qubit_pass.hpp"

using qplayer::Circuit;
using qplayer::MergeTwoQubitPass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;

static constexpr double kPi  = 3.14159265358979323846;
static constexpr double kTol = 1e-10;

// ── Case 1: CX(0,1)·CX(0,1) → identity (both ops removed) ───────────────────

TEST(MergeTwoQubitPass, CX_SameOrder_Cancelled) {
    Circuit c(2);
    c.cx(0, 1).cx(0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 2: CX(0,1)·CX(1,0) — different qubit order, NOT cancelled ───────────

TEST(MergeTwoQubitPass, CX_ReverseOrder_NotCancelled) {
    // CX(0,1) and CX(1,0) are distinct gates; must not be merged.
    Circuit c(2);
    c.cx(0, 1).cx(1, 0);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 3: SWAP(0,1)·SWAP(0,1) → identity ───────────────────────────────────

TEST(MergeTwoQubitPass, SWAP_SameOrder_Cancelled) {
    Circuit c(2);
    c.swap(0, 1).swap(0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 4: CRZ(π/4, 0,1)·CRZ(π/4, 0,1) → CRZ(π/2, 0,1) ───────────────────

TEST(MergeTwoQubitPass, CRZ_AccumulatesAngle) {
    Circuit c(2);
    c.crz(kPi / 4.0, 0, 1).crz(kPi / 4.0, 0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    const GateOp& g = std::get<GateOp>(out.ops()[0]);
    EXPECT_TRUE(g.type == GateType::CRZ);
    EXPECT_NEAR(g.params[0], kPi / 2.0, kTol);
}

// ── Case 5: CU1(π/6, 0,1)·CU1(π/6, 0,1) → CU1(π/3, 0,1) ───────────────────

TEST(MergeTwoQubitPass, CU1_AccumulatesAngle) {
    Circuit c(2);
    c.cu1(kPi / 6.0, 0, 1).cu1(kPi / 6.0, 0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    const GateOp& g = std::get<GateOp>(out.ops()[0]);
    EXPECT_TRUE(g.type == GateType::CU1);
    EXPECT_NEAR(g.params[0], kPi / 3.0, kTol);
}

// ── Case 6: barrier boundary — CX·barrier·CX not merged ──────────────────────

TEST(MergeTwoQubitPass, Barrier_Boundary_PreventsMerge) {
    Circuit c(2);
    c.cx(0, 1).barrier({0, 1}).cx(0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: cx, barrier, cx
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 7: 1Q gate between two CX invalidates the pair ──────────────────────

TEST(MergeTwoQubitPass, OneQGateBetween_Invalidates) {
    // CX(0,1) · X(0) · CX(0,1): X on q[0] (shared qubit) invalidates pending CX.
    Circuit c(2);
    c.cx(0, 1).x(0).cx(0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain — no merge
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 8: changed() false when nothing merged ───────────────────────────────

TEST(MergeTwoQubitPass, ChangedFlag_FalseWhenNoMerge) {
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    MergeTwoQubitPass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 9: name() returns expected string ────────────────────────────────────

TEST(MergeTwoQubitPass, Name_ReturnsExpectedString) {
    MergeTwoQubitPass pass;
    EXPECT_EQ(pass.name(), std::string{"MergeTwoQubitPass"});
}

// ── Case 10: CZ·CZ same qubit pair → identity ────────────────────────────────

TEST(MergeTwoQubitPass, CZ_SameOrder_Cancelled) {
    Circuit c(2);
    c.cz(0, 1).cz(0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 11: CCX(0,1,2)·CCX(0,1,2) → identity ───────────────────────────────

TEST(MergeTwoQubitPass, CCX_SameQubits_Cancelled) {
    Circuit c(3);
    c.ccx(0, 1, 2).ccx(0, 1, 2);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 12: measure boundary — CX·measure·CX not merged ─────────────────────

TEST(MergeTwoQubitPass, Measure_Boundary_PreventsMerge) {
    Circuit c(2, 1);
    c.cx(0, 1).measure(0, 0).cx(0, 1);

    MergeTwoQubitPass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops remain: cx, measure, cx
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}
