// test_swap_router.cpp — Phase 8 unit tests for SwapRouterPass
// (BFS greedy). IR-level checks: full-connectivity
// no-op, adjacent gate pass-through, SWAP insertion for non-adjacent 2Q gates,
// the post-routing adjacency invariant, layout consistency, measure ops left
// logical, custom initial layout, and the unroutable (disconnected) reject.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <set>
#include <utility>
#include <variant>
#include <vector>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/error.hpp"
#include "../../../../include/noise/device_model.hpp"
#include "../../../../include/transpiler/passes/layout/initial_layout_pass.hpp"
#include "../../../../include/transpiler/passes/routing/swap_router_pass.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::DeviceModel;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::InitialLayoutPass;
using qplayer::MeasureOp;
using qplayer::Op;
using qplayer::QubitIndex;
using qplayer::QubitLayout;
using qplayer::SwapRouterPass;

namespace {

// Linear chain 0-1-2-3 device with coherence so num_qubits == 4.
DeviceModel linear_4q() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4},
                 {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "coupling_map": [[0, 1], [1, 2], [2, 3]]
    })");
}

// Full connectivity — no coupling_map field.
DeviceModel full_4q() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4},
                 {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}]
    })");
}

// Two disconnected components 0-1 and 2-3.
DeviceModel disconnected_4q() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4},
                 {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "coupling_map": [[0, 1], [2, 3]]
    })");
}

std::set<std::pair<QubitIndex, QubitIndex>> undirected(
    const std::vector<std::pair<QubitIndex, QubitIndex>>& cm) {
    std::set<std::pair<QubitIndex, QubitIndex>> s;
    for (const auto& [a, b] : cm) {
        s.emplace(a, b);
        s.emplace(b, a);
    }
    return s;
}

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

// True when every 2-qubit GateOp (including inserted SWAPs) acts on a
// coupling-adjacent physical pair.
bool all_two_qubit_gates_adjacent(
    const Circuit& c,
    const std::set<std::pair<QubitIndex, QubitIndex>>& adj) {
    for (const Op& op : c.ops()) {
        if (!std::holds_alternative<GateOp>(op)) continue;
        const GateOp& g = std::get<GateOp>(op);
        if (g.qubits.size() != 2) continue;
        if (adj.find({g.qubits[0], g.qubits[1]}) == adj.end()) return false;
    }
    return true;
}

}  // namespace

// ── Full connectivity: no-op ──────────────────────────────────────────────────

TEST(SwapRouter, FullConnectivity_NoOp) {
    // Arrange
    DeviceModel dev = full_4q();
    Circuit circ(4, 0);
    circ.h(0).cx(0, 3);  // non-adjacent on a chain, but full connectivity here

    // Act
    SwapRouterPass pass(dev);
    Circuit out = pass.run(circ);

    // Assert — no SWAPs, nothing changed.
    EXPECT_FALSE(pass.changed());
    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(0));
    EXPECT_EQ(out.num_ops(), circ.num_ops());
}

// ── Already adjacent: pass-through ────────────────────────────────────────────

TEST(SwapRouter, AdjacentGate_NoSwapInserted) {
    DeviceModel dev = linear_4q();
    Circuit circ(4, 0);
    circ.h(0).cx(0, 1).cx(1, 2);  // all adjacent on chain

    SwapRouterPass pass(dev);
    Circuit out = pass.run(circ);

    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(0));
    EXPECT_TRUE(all_two_qubit_gates_adjacent(out, undirected(dev.coupling_map())));
}

// ── Non-adjacent: SWAP insertion + adjacency invariant ────────────────────────

TEST(SwapRouter, NonAdjacentGate_InsertsSwaps) {
    DeviceModel dev = linear_4q();
    Circuit circ(4, 0);
    circ.h(0).cx(0, 3);  // logical 0..3 are distance 3 apart on the chain

    SwapRouterPass pass(dev);
    Circuit out = pass.run(circ);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(count_swaps(out) > 0);
    // BFS path 0-1-2-3 -> SWAP(0,1), SWAP(1,2) move logical 0 adjacent to 3.
    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(2));
    EXPECT_TRUE(all_two_qubit_gates_adjacent(out, undirected(dev.coupling_map())));
}

// ── Layout consistency maintained ─────────────────────────────────────────────

TEST(SwapRouter, FinalLayoutConsistent) {
    DeviceModel dev = linear_4q();
    Circuit circ(4, 0);
    circ.cx(0, 3).cx(1, 3);

    SwapRouterPass pass(dev);
    Circuit out = pass.run(circ);

    EXPECT_TRUE(out.metadata().layout.has_value());
    EXPECT_TRUE(qplayer::is_layout_consistent(*out.metadata().layout));
    EXPECT_TRUE(all_two_qubit_gates_adjacent(out, undirected(dev.coupling_map())));
}

// ── Measure ops remapped to physical home ──────────────────────────

TEST(SwapRouter, MeasureOpsRemappedToPhysical) {
    DeviceModel dev = linear_4q();
    Circuit circ(4, 4);
    circ.h(0).cx(0, 3).measure(0, 0).measure(3, 3);

    SwapRouterPass pass(dev);
    Circuit out = pass.run(circ);

    // The measure qubit is rewritten to logical q's final physical home; the
    // cubit (classical target) is preserved so the outcome is logical-indexed.
    const QubitLayout& layout = *out.metadata().layout;
    std::vector<std::pair<QubitIndex, QubitIndex>> measures;  // (qubit, cubit)
    for (const Op& op : out.ops()) {
        if (std::holds_alternative<MeasureOp>(op)) {
            const MeasureOp& m = std::get<MeasureOp>(op);
            measures.emplace_back(m.qubit, m.cubit);
        }
    }
    EXPECT_EQ(measures.size(), static_cast<std::size_t>(2));
    // qubit == physical home of the original logical qubit; cubit unchanged.
    EXPECT_EQ(measures[0].first, layout.logical_to_physical.at(0));
    EXPECT_EQ(measures[0].second, static_cast<QubitIndex>(0));
    EXPECT_EQ(measures[1].first, layout.logical_to_physical.at(3));
    EXPECT_EQ(measures[1].second, static_cast<QubitIndex>(3));
}

// ── Custom initial layout from InitialLayoutPass ──────────────────────────────

TEST(SwapRouter, CustomInitialLayout_RoutedAgainstIt) {
    DeviceModel dev = linear_4q();
    Circuit circ(4, 0);
    circ.cx(0, 1);  // logical 0,1

    // Reverse layout: logical 0->phys 3, 1->phys 2, 2->phys 1, 3->phys 0.
    // Logical (0,1) now sit on physical (3,2): adjacent on the chain -> no SWAP.
    InitialLayoutPass layout(dev, std::vector<QubitIndex>{3, 2, 1, 0});
    Circuit laid = layout.run(circ);

    SwapRouterPass pass(dev);
    Circuit out = pass.run(laid);

    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(0));
    EXPECT_TRUE(all_two_qubit_gates_adjacent(out, undirected(dev.coupling_map())));
}

// ── Unroutable: disconnected coupling graph ───────────────────────────────────

TEST(SwapRouter, DisconnectedGraph_ThrowsCircuitError) {
    DeviceModel dev = disconnected_4q();  // components {0,1} and {2,3}
    Circuit circ(4, 0);
    circ.cx(0, 2);  // crosses the component boundary -> no physical path

    SwapRouterPass pass(dev);
    EXPECT_THROW(pass.run(circ), CircuitError);
}
