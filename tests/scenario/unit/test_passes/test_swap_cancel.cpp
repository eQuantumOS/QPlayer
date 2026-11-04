// test_swap_cancel.cpp — Phase 8 unit tests for SwapCancelPass
// Covers the commutation-aware redundant SWAP-pair
// cancellation, the blocking cases (an intervening op touches the swapped
// pair), order-independence of the pair, and pass-through of unrelated ops.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <variant>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/swap_cancel_pass.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;
using qplayer::SwapCancelPass;

namespace {

std::size_t count_swaps(const Circuit& c) {
    std::size_t n = 0;
    for (const Op& op : c.ops()) {
        if (std::holds_alternative<GateOp>(op) &&
            std::get<GateOp>(op).type == GateType::SWAP) {
            ++n;
        }
    }
    return n;
}

}  // namespace

// ── Commutation-aware cancellation ────────────────────────────────────────────

TEST(SwapCancel, PairAcrossDisjointOps_Cancels) {
    // SWAP(0,1) ; H(2) ; SWAP(0,1) — H(2) touches neither 0 nor 1 -> cancel.
    Circuit circ(3, 0);
    circ.swap(0, 1).h(2).swap(0, 1);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_TRUE(pass.changed());
    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(0));
    EXPECT_EQ(out.num_ops(), static_cast<std::size_t>(1));  // only H(2) remains
}

TEST(SwapCancel, ReversedPairOrder_Cancels) {
    // SWAP(0,1) ; X(2) ; SWAP(1,0) — same unordered pair -> cancel.
    Circuit circ(3, 0);
    circ.swap(0, 1).x(2).swap(1, 0);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(0));
}

// ── Blocking cases ────────────────────────────────────────────────────────────

TEST(SwapCancel, InterveningGateOnPair_DoesNotCancel) {
    // SWAP(0,1) ; H(0) ; SWAP(0,1) — H(0) touches the pair -> no cancellation.
    Circuit circ(2, 0);
    circ.swap(0, 1).h(0).swap(0, 1);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_FALSE(pass.changed());
    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(2));
}

TEST(SwapCancel, InterveningMeasureOnPair_DoesNotCancel) {
    // A measurement on one of the swapped qubits blocks cancellation.
    Circuit circ(2, 2);
    circ.swap(0, 1).measure(1, 1).swap(0, 1);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(2));
}

TEST(SwapCancel, DifferentPair_DoesNotCancel) {
    // SWAP(0,1) ; SWAP(1,2) — different pairs, no cancellation.
    Circuit circ(3, 0);
    circ.swap(0, 1).swap(1, 2);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(2));
}

// ── Boundary / no-op ──────────────────────────────────────────────────────────

TEST(SwapCancel, NoSwaps_Unchanged) {
    Circuit circ(2, 0);
    circ.h(0).cx(0, 1);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_FALSE(pass.changed());
    EXPECT_EQ(out.num_ops(), static_cast<std::size_t>(2));
}

TEST(SwapCancel, SingleSwap_Preserved) {
    Circuit circ(2, 0);
    circ.swap(0, 1);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_FALSE(pass.changed());
    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(1));
}

TEST(SwapCancel, BarrierOnPair_DoesNotCancel) {
    // An all-qubit barrier between the SWAPs is a reorder boundary -> blocks.
    Circuit circ(3, 0);
    circ.swap(0, 1).barrier().swap(0, 1);

    SwapCancelPass pass;
    Circuit out = pass.run(circ);

    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(2));
}
