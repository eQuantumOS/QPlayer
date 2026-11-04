/// @file topology.cpp
/// @brief Coupling-graph extraction and MPS/TN classification.
///
/// The decision is a graph question: an MPS lays sites on a line, so it can make
/// every two-qubit gate act on neighbours exactly when each connected component
/// of the interaction graph is a path. A vertex of degree 3 (a star centre) or a
/// cycle defeats every possible ordering, and that is when a general tensor
/// network is the right representation.
///
/// A NonLinear circuit still runs on the MPS, and there the site order decides
/// how far apart the operands of each gate sit. A minimum linear arrangement
/// is available but NOT the default: on the same d=5
/// surface code it turned simple damping noise from 45 s lossy into 2 s
/// lossless, and the paper-1 willow workload from 2 s into more than 900 s.
/// The sign of the effect is not predictable from the walk cost.

#include "backend/tensor_network/topology.hpp"

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <numeric>
#include <set>

namespace qplayer {
namespace detail {

namespace {

// Undirected adjacency with duplicate edges collapsed: an MPS cares about which
// qubits interact, not how often.
struct Adjacency {
    std::vector<std::set<QubitIndex>> neighbours;
    std::size_t edges = 0;
};

Adjacency build_adjacency(const std::vector<std::pair<QubitIndex, QubitIndex>>& coupling,
                          QubitIndex num_qubits) {
    Adjacency adj;
    adj.neighbours.resize(num_qubits);
    for (const auto& [a, b] : coupling) {
        if (a == b || a >= num_qubits || b >= num_qubits) continue;  // self/out-of-range: no edge
        const bool inserted = adj.neighbours[a].insert(b).second;
        adj.neighbours[b].insert(a);
        if (inserted) ++adj.edges;
    }
    return adj;
}

// Walks one connected component and appends it to `order`. Returns false when
// the component is not a path (a branch or a cycle), which settles the whole
// classification — one bad component is enough.
bool walk_component(const Adjacency& adj, QubitIndex start,
                    std::vector<bool>& seen, std::vector<QubitIndex>& order) {
    // Collect the component first so the walk can start from an endpoint. A path
    // walked from the middle would look like two branches.
    std::vector<QubitIndex> members;
    std::vector<QubitIndex> stack{start};
    seen[start] = true;
    while (!stack.empty()) {
        const QubitIndex q = stack.back();
        stack.pop_back();
        members.push_back(q);
        for (QubitIndex nb : adj.neighbours[q]) {
            if (!seen[nb]) { seen[nb] = true; stack.push_back(nb); }
        }
    }

    std::size_t component_edges = 0;
    QubitIndex endpoint = members.front();
    bool endpoint_found = false;
    for (QubitIndex q : members) {
        const std::size_t degree = adj.neighbours[q].size();
        if (degree > 2) return false;              // a branch: no ordering fixes it
        component_edges += degree;
        if (degree <= 1 && !endpoint_found) { endpoint = q; endpoint_found = true; }
    }
    component_edges /= 2;

    // A path on k vertices has k-1 edges; k edges means a cycle, which also has
    // no linear layout even though every degree is 2.
    if (component_edges != members.size() - 1) return false;

    // Walk endpoint -> ... -> endpoint, which is unique for a path.
    std::vector<bool> placed(adj.neighbours.size(), false);
    QubitIndex current = endpoint;
    for (std::size_t i = 0; i < members.size(); ++i) {
        order.push_back(current);
        placed[current] = true;
        QubitIndex next = current;
        bool advanced = false;
        for (QubitIndex nb : adj.neighbours[current]) {
            if (!placed[nb]) { next = nb; advanced = true; break; }
        }
        if (!advanced) break;
        current = next;
    }
    return true;
}

// Interaction weights for the layout: a pair that two gates touch walks twice,
// so unlike Adjacency the duplicates are counted, not collapsed.
using WeightedNeighbours = std::vector<std::vector<std::pair<QubitIndex, std::uint64_t>>>;

WeightedNeighbours build_weighted(const std::vector<std::pair<QubitIndex, QubitIndex>>& coupling,
                                  QubitIndex num_qubits) {
    std::map<std::pair<QubitIndex, QubitIndex>, std::uint64_t> weight;
    for (auto [a, b] : coupling) {
        if (a == b || a >= num_qubits || b >= num_qubits) continue;
        if (a > b) std::swap(a, b);
        ++weight[{a, b}];
    }
    WeightedNeighbours nbr(num_qubits);
    for (const auto& [edge, w] : weight) {
        nbr[edge.first].emplace_back(edge.second, w);
        nbr[edge.second].emplace_back(edge.first, w);
    }
    return nbr;
}

std::uint64_t arrangement_cost(const WeightedNeighbours& nbr, const std::vector<QubitIndex>& pos) {
    std::uint64_t cost = 0;
    for (QubitIndex q = 0; q < nbr.size(); ++q) {
        for (const auto& [o, w] : nbr[q]) {
            if (o > q) cost += w * static_cast<std::uint64_t>(pos[q] > pos[o] ? pos[q] - pos[o]
                                                                              : pos[o] - pos[q]);
        }
    }
    return cost;
}

std::vector<QubitIndex> positions_of(const std::vector<QubitIndex>& order) {
    std::vector<QubitIndex> pos(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) pos[order[i]] = static_cast<QubitIndex>(i);
    return pos;
}

// Breadth-first order from the far end of each component (Cuthill-McKee), which
// already keeps neighbours close; the swap passes below then polish it. Ties go
// to the lower degree, then the lower index, so the order is deterministic.
std::vector<QubitIndex> cuthill_mckee(const WeightedNeighbours& nbr) {
    const QubitIndex n = static_cast<QubitIndex>(nbr.size());
    auto bfs = [&](QubitIndex start, std::vector<bool>& seen) {
        std::vector<QubitIndex> out;
        std::deque<QubitIndex> queue{start};
        seen[start] = true;
        while (!queue.empty()) {
            const QubitIndex q = queue.front();
            queue.pop_front();
            out.push_back(q);
            std::vector<QubitIndex> next;
            for (const auto& [o, w] : nbr[q]) {
                if (!seen[o]) { seen[o] = true; next.push_back(o); }
            }
            std::sort(next.begin(), next.end(), [&](QubitIndex x, QubitIndex y) {
                return nbr[x].size() != nbr[y].size() ? nbr[x].size() < nbr[y].size() : x < y;
            });
            queue.insert(queue.end(), next.begin(), next.end());
        }
        return out;
    };

    std::vector<bool> placed(n, false);
    std::vector<QubitIndex> order;
    order.reserve(n);
    for (;;) {
        QubitIndex start = n;
        for (QubitIndex q = 0; q < n; ++q) {
            if (!placed[q] && (start == n || nbr[q].size() < nbr[start].size())) start = q;
        }
        if (start == n) break;
        // One George-Liu step: restart from the last vertex reached, an end of the
        // component's longest shortest path, so the order runs along it.
        std::vector<bool> probe = placed;
        const std::vector<QubitIndex> first = bfs(start, probe);
        const std::vector<QubitIndex> component = bfs(first.back(), placed);
        order.insert(order.end(), component.begin(), component.end());
    }
    return order;
}

// Pairwise site exchanges, kept only when they strictly lower the cost. Strict
// matters: on a symmetric graph (all-to-all, as in a QFT) every exchange ties,
// and accepting ties would shuffle the order for no gain.
void improve_by_exchange(const WeightedNeighbours& nbr, std::vector<QubitIndex>& order) {
    constexpr int kMaxPasses = 64;   // measured surface code d<=7 settles in < 10
    std::vector<QubitIndex> pos = positions_of(order);
    const std::size_t n = order.size();
    auto local = [&](QubitIndex q, QubitIndex p) {
        std::uint64_t c = 0;
        for (const auto& [o, w] : nbr[q]) c += w * static_cast<std::uint64_t>(p > pos[o] ? p - pos[o] : pos[o] - p);
        return c;
    };
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        bool improved = false;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                const QubitIndex a = order[i], b = order[j];
                const QubitIndex pi = static_cast<QubitIndex>(i), pj = static_cast<QubitIndex>(j);
                const std::uint64_t before = local(a, pi) + local(b, pj);
                pos[a] = pj;
                pos[b] = pi;
                const std::uint64_t after = local(a, pj) + local(b, pi);
                if (after < before) {
                    std::swap(order[i], order[j]);
                    improved = true;
                } else {
                    pos[a] = pi;
                    pos[b] = pj;
                }
            }
        }
        if (!improved) break;
    }
}

// Sets chain_order and both costs for a NonLinear circuit. The identity stays
// unless a candidate walks strictly less: a layout change reorders the native
// sampler's draws, so it is not worth making for nothing.
void choose_nonlinear_layout(const std::vector<std::pair<QubitIndex, QubitIndex>>& coupling,
                             QubitIndex num_qubits, TopologyAnalysis& out) {
    const WeightedNeighbours nbr = build_weighted(coupling, num_qubits);
    std::vector<QubitIndex> identity(num_qubits);
    std::iota(identity.begin(), identity.end(), QubitIndex{0});
    out.identity_layout_cost = arrangement_cost(nbr, identity);
    out.chain_order          = identity;
    out.layout_cost          = out.identity_layout_cost;

    for (std::vector<QubitIndex> candidate : {identity, cuthill_mckee(nbr)}) {
        improve_by_exchange(nbr, candidate);
        const std::uint64_t cost = arrangement_cost(nbr, positions_of(candidate));
        if (cost < out.layout_cost) {
            out.layout_cost = cost;
            out.chain_order = std::move(candidate);
        }
    }
}

}  // namespace

bool TopologyAnalysis::is_identity_order() const noexcept {
    for (std::size_t i = 0; i < chain_order.size(); ++i) {
        if (chain_order[i] != static_cast<QubitIndex>(i)) return false;
    }
    return true;
}

TopologyAnalysis CircuitTopologyAnalyzer::analyze(
        const std::vector<std::pair<QubitIndex, QubitIndex>>& coupling,
        QubitIndex num_qubits, NonLinearLayout layout) {
    TopologyAnalysis out;
    out.chain_order.resize(num_qubits);
    std::iota(out.chain_order.begin(), out.chain_order.end(), QubitIndex{0});
    if (num_qubits == 0) return out;

    const Adjacency adj = build_adjacency(coupling, num_qubits);
    out.coupling_edges = adj.edges;
    for (const auto& nbs : adj.neighbours) {
        out.max_degree = std::max(out.max_degree, nbs.size());
    }

    std::vector<bool> seen(num_qubits, false);
    std::vector<QubitIndex> order;
    order.reserve(num_qubits);
    bool linear = true;
    for (QubitIndex q = 0; q < num_qubits; ++q) {
        if (seen[q]) continue;
        ++out.components;
        if (!walk_component(adj, q, seen, order)) { linear = false; break; }
    }

    if (!linear || order.size() != num_qubits) {
        // The partial walk is discarded -- it must not leak a half-built
        // permutation -- and the layout is chosen from scratch.
        out.topology = TnTopology::NonLinear;
        if (layout == NonLinearLayout::MinLA) {
            choose_nonlinear_layout(coupling, num_qubits, out);
        } else {
            const WeightedNeighbours nbr = build_weighted(coupling, num_qubits);
            out.identity_layout_cost = arrangement_cost(nbr, out.chain_order);
            out.layout_cost          = out.identity_layout_cost;
        }
        return out;
    }
    out.topology    = TnTopology::Linear;
    out.chain_order = std::move(order);
    const WeightedNeighbours nbr = build_weighted(coupling, num_qubits);
    std::vector<QubitIndex> identity(num_qubits);
    std::iota(identity.begin(), identity.end(), QubitIndex{0});
    out.identity_layout_cost = arrangement_cost(nbr, identity);
    out.layout_cost          = arrangement_cost(nbr, positions_of(out.chain_order));
    return out;
}

TopologyAnalysis CircuitTopologyAnalyzer::analyze(const Circuit& circuit,
                                                  NonLinearLayout layout) {
    std::vector<std::pair<QubitIndex, QubitIndex>> coupling;
    for (const Op& op : circuit.ops()) {
        const auto* gate = std::get_if<GateOp>(&op);
        if (gate == nullptr || gate->qubits.size() < 2) continue;
        // All pairs, so a 3-qubit gate is read as "these must all be close" —
        // the conservative reading, and it makes such a circuit NonLinear, which
        // is correct: no chain makes three mutual neighbours adjacent.
        for (std::size_t i = 0; i < gate->qubits.size(); ++i) {
            for (std::size_t j = i + 1; j < gate->qubits.size(); ++j) {
                coupling.emplace_back(gate->qubits[i], gate->qubits[j]);
            }
        }
    }
    return analyze(coupling, circuit.num_qubits(), layout);
}

}  // namespace detail
}  // namespace qplayer
