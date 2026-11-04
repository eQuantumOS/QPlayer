// test_commutation_reorder_pass.cpp — Unit tests for CommutationReorderPass.
// Verifies that disjoint-qubit adjacent gates are reordered by the pass and
// that boundary ops are never crossed.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/commutation_reorder_pass.hpp"

using qplayer::Circuit;
using qplayer::CommutationReorderPass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;

// ── Case 1: Disjoint-qubit pair reordered when enum ordering demands it ───────
// h(1) (H=4) followed by x(0) (X=1): since X enum < H enum and qubits disjoint,
// the pass should swap them. changed() must be true.

TEST(CommutationReorderPass, DisjointQubits_LowerEnumFirst_Swapped) {
    // Arrange: h(1), x(0) — H > X in enum, qubits disjoint → swap expected.
    Circuit c(2);
    c.h(1).x(0);

    CommutationReorderPass pass;
    Circuit out = pass.run(std::move(c));

    // The pass performs a swap so x(0) appears before h(1).
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_TRUE(pass.changed());
    const auto& ops = out.ops();
    EXPECT_TRUE(std::get<GateOp>(ops[0]).type == GateType::X);
    EXPECT_EQ(std::get<GateOp>(ops[0]).qubits[0], qplayer::QubitIndex{0});
    EXPECT_TRUE(std::get<GateOp>(ops[1]).type == GateType::H);
    EXPECT_EQ(std::get<GateOp>(ops[1]).qubits[0], qplayer::QubitIndex{1});
}

// ── Case 2: Same-qubit gates — no reorder permitted ──────────────────────────

TEST(CommutationReorderPass, SameQubit_NoReorder) {
    Circuit c(1);
    c.h(0).x(0);

    CommutationReorderPass pass;
    Circuit out = pass.run(std::move(c));

    // Same qubit — not disjoint, no swap.
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
    const auto& ops = out.ops();
    EXPECT_TRUE(std::get<GateOp>(ops[0]).type == GateType::H);
    EXPECT_TRUE(std::get<GateOp>(ops[1]).type == GateType::X);
}

// ── Case 3: Barrier boundary — disjoint gates must not swap across barrier ────

TEST(CommutationReorderPass, Barrier_StopsReorder) {
    Circuit c(2);
    // h(1), barrier, x(0): barrier between them prevents reorder.
    c.h(1).barrier({}).x(0);

    CommutationReorderPass pass;
    Circuit out = pass.run(std::move(c));

    // 3 ops unchanged.
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 4: Already lower-enum-first order — no swap (already optimal) ────────

TEST(CommutationReorderPass, AlreadyOrdered_NoChange) {
    // x(0) (X=1) followed by h(1) (H=4): X < H and qubits disjoint.
    // The pass only swaps when enum_j < enum_i so this is already ordered.
    Circuit c(2);
    c.x(0).h(1);

    CommutationReorderPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 5: changed() false on empty circuit ─────────────────────────────────

TEST(CommutationReorderPass, EmptyCircuit_ChangedFalse) {
    Circuit c(2);

    CommutationReorderPass pass;
    pass.run(std::move(c));

    EXPECT_FALSE(pass.changed());
}

// ── Case 6: name() returns expected string ────────────────────────────────────

TEST(CommutationReorderPass, Name_ReturnsExpectedString) {
    CommutationReorderPass pass;
    EXPECT_EQ(pass.name(), std::string{"CommutationReorderPass"});
}

// ── Case 7: Multi-qubit gate involving shared qubit — no reorder ──────────────

TEST(CommutationReorderPass, SharedQubitInCX_NoReorder) {
    // cx(0,1) then h(1): qubit 1 is shared, not disjoint → no swap.
    Circuit c(2);
    c.cx(0, 1).h(1);

    CommutationReorderPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
    const auto& ops = out.ops();
    EXPECT_TRUE(std::get<GateOp>(ops[0]).type == GateType::CX);
    EXPECT_TRUE(std::get<GateOp>(ops[1]).type == GateType::H);
}
