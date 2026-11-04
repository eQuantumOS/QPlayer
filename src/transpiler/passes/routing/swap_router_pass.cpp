// swap_router_pass.cpp — SwapRouterPass implementation.
// BFS shortest-path greedy routing: for each two-qubit gate whose
// operands are not coupling-adjacent, find the shortest physical path between
// them and insert SWAPs along it, updating the running layout. All state ops
// are rewritten into physical space; measures stay logical.

#include "transpiler/passes/routing/swap_router_pass.hpp"

#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "circuit/metadata.hpp"
#include "error.hpp"

namespace qplayer {

namespace {

// Adjacency set (both directions) for O(log n) coupling lookups.
using AdjSet = std::set<std::pair<QubitIndex, QubitIndex>>;

AdjSet make_adj_set(const CouplingMap& cm) {
    AdjSet s;
    for (const auto& [a, b] : cm) {
        s.emplace(a, b);
        s.emplace(b, a);
    }
    return s;
}

bool adjacent(const AdjSet& adj, QubitIndex a, QubitIndex b) {
    return adj.find({a, b}) != adj.end();
}

// BFS shortest physical path src->dst over the coupling graph. Returns the
// node sequence including both endpoints, or an empty vector when dst is
// unreachable (disconnected components).
std::vector<QubitIndex> bfs_path(const std::vector<std::vector<QubitIndex>>& graph,
                                 QubitIndex src, QubitIndex dst) {
    std::unordered_map<QubitIndex, QubitIndex> parent;
    std::queue<QubitIndex> frontier;
    frontier.push(src);
    parent[src] = src;  // self-parent marks the root / visited

    while (!frontier.empty()) {
        const QubitIndex cur = frontier.front();
        frontier.pop();
        if (cur == dst) break;
        for (QubitIndex nb : graph[cur]) {
            if (parent.find(nb) == parent.end()) {
                parent[nb] = cur;
                frontier.push(nb);
            }
        }
    }

    if (parent.find(dst) == parent.end()) return {};  // unreachable

    std::vector<QubitIndex> path;
    for (QubitIndex node = dst;; node = parent[node]) {
        path.push_back(node);
        if (node == src) break;
    }
    // Reconstructed dst->src; reverse to src->dst.
    return {path.rbegin(), path.rend()};
}

// Exchange the logical occupants of physical qubits pi and pj. Empty physical
// slots (no logical qubit) are handled — routing may pass through ancillas.
void apply_swap(QubitLayout& layout, QubitIndex pi, QubitIndex pj) {
    auto it = layout.physical_to_logical.find(pi);
    auto jt = layout.physical_to_logical.find(pj);
    const bool hi = it != layout.physical_to_logical.end();
    const bool hj = jt != layout.physical_to_logical.end();
    const QubitIndex li = hi ? it->second : 0;
    const QubitIndex lj = hj ? jt->second : 0;

    if (hi && hj) {
        layout.logical_to_physical[li] = pj;
        layout.logical_to_physical[lj] = pi;
        layout.physical_to_logical[pi] = lj;
        layout.physical_to_logical[pj] = li;
    } else if (hi) {
        layout.logical_to_physical[li] = pj;
        layout.physical_to_logical.erase(pi);
        layout.physical_to_logical[pj] = li;
    } else if (hj) {
        layout.logical_to_physical[lj] = pi;
        layout.physical_to_logical.erase(pj);
        layout.physical_to_logical[pi] = lj;
    }
    // both empty -> physical no-op
}

QubitIndex phys_of(const QubitLayout& layout, QubitIndex logical) {
    return layout.logical_to_physical.at(logical);
}

// Build a SWAP GateOp on two physical qubits (atomic SWAP IR).
GateOp make_swap(QubitIndex pa, QubitIndex pb) {
    GateOp g;
    g.type = GateType::SWAP;
    g.name = "swap";
    g.qubits = {pa, pb};
    return g;
}

}  // namespace

SwapRouterPass::SwapRouterPass(const DeviceModel& device)
    : device_(&device) {}

std::string SwapRouterPass::name() const {
    return "SwapRouterPass";
}

Circuit SwapRouterPass::run(Circuit circuit) {
    changed_ = false;

    // Full connectivity (no coupling map) -> nothing to route.
    if (!device_->has_coupling_map()) {
        return circuit;
    }

    const QubitIndex   dev_n = device_->num_qubits();
    const CouplingMap&  cm   = device_->coupling_map();
    const AdjSet        adj  = make_adj_set(cm);

    std::vector<std::vector<QubitIndex>> graph(dev_n);
    for (const auto& [a, b] : cm) {
        graph[a].push_back(b);
        graph[b].push_back(a);
    }

    // Running layout: start from InitialLayoutPass output, else identity.
    QubitLayout layout = circuit.metadata().layout.has_value()
                             ? *circuit.metadata().layout
                             : make_identity_layout(circuit.num_qubits());

    std::vector<Op> new_ops;
    new_ops.reserve(circuit.ops().size());
    bool modified = false;

    for (const Op& op : circuit.ops()) {
        if (std::holds_alternative<GateOp>(op)) {
            GateOp g = std::get<GateOp>(op);

            if (g.qubits.size() == 2) {
                // Two-qubit gate: ensure operands map to an adjacent pair.
                const QubitIndex la = g.qubits[0];
                const QubitIndex lb = g.qubits[1];
                QubitIndex pa = phys_of(layout, la);
                QubitIndex pb = phys_of(layout, lb);

                if (!adjacent(adj, pa, pb)) {
                    const std::vector<QubitIndex> path = bfs_path(graph, pa, pb);
                    if (path.size() < 2) {
                        throw CircuitError{
                            "SwapRouterPass: no coupling path between physical "
                            "qubits " + std::to_string(pa) + " and " +
                            std::to_string(pb) + " (disconnected graph)"};
                    }
                    // SWAP along path[:-1] to slide logical `la` next to `lb`.
                    for (std::size_t i = 0; i + 2 < path.size(); ++i) {
                        new_ops.push_back(make_swap(path[i], path[i + 1]));
                        apply_swap(layout, path[i], path[i + 1]);
                    }
                    pa = phys_of(layout, la);
                    pb = phys_of(layout, lb);
                    modified = true;
                }
                g.qubits = {pa, pb};
                if (g.qubits[0] != la || g.qubits[1] != lb) modified = true;
            } else {
                // 1Q / Reset / 3Q: remap operands to physical. 3Q coupling
                // routing is out of scope (no requirement); operands are mapped
                // so the circuit stays in a single physical space.
                for (QubitIndex& q : g.qubits) {
                    const QubitIndex p = phys_of(layout, q);
                    if (p != q) modified = true;
                    q = p;
                }
            }
            new_ops.push_back(std::move(g));

        } else if (std::holds_alternative<MeasureOp>(op)) {
            // Remap the measured qubit to its current physical home so the
            // outcome of logical qubit q lands in its classical bit — results
            // come out logical-indexed automatically. Using the
            // running layout (not just the final one) keeps mid-circuit
            // measurement correct under later SWAPs.
            MeasureOp m = std::get<MeasureOp>(op);
            const QubitIndex p = phys_of(layout, m.qubit);
            if (p != m.qubit) modified = true;
            m.qubit = p;
            new_ops.push_back(std::move(m));

        } else if (std::holds_alternative<BarrierOp>(op)) {
            BarrierOp b = std::get<BarrierOp>(op);
            for (QubitIndex& q : b.qubits) {  // empty == all -> left empty
                const QubitIndex p = phys_of(layout, q);
                if (p != q) modified = true;
                q = p;
            }
            new_ops.push_back(std::move(b));

        } else if (std::holds_alternative<IfOp>(op)) {
            IfOp f = std::get<IfOp>(op);
            for (QubitIndex& q : f.body.qubits) {
                const QubitIndex p = phys_of(layout, q);
                if (p != q) modified = true;
                q = p;
            }
            new_ops.push_back(std::move(f));

        } else {  // IdleOp — not expected before scheduling, remap defensively
            IdleOp d = std::get<IdleOp>(op);
            const QubitIndex p = phys_of(layout, d.qubit);
            if (p != d.qubit) modified = true;
            d.qubit = p;
            new_ops.push_back(std::move(d));
        }
    }

    changed_ = modified;
    if (!modified) {
        return circuit;  // identity layout + all gates adjacent — unchanged
    }

    CircuitMetadata meta = circuit.metadata();
    meta.layout = std::move(layout);
    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        std::move(meta));
}

bool SwapRouterPass::changed() const {
    return changed_;
}

}  // namespace qplayer
