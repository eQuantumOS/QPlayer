/// @file contraction_order.cpp
/// @brief Contraction order planning and execution.
///
/// Planning works on index-dimension bookkeeping only — no amplitudes are
/// touched — so a plan can be costed before anything is allocated. Execution
/// then replays the plan with real tensors.

#include "backend/tensor_network/contraction_order.hpp"

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "backend/full_state/gate_fusion.hpp"
#include "error.hpp"

#include <algorithm>
#include <limits>
#include <cmath>
#include <numeric>
#include <string>

namespace qplayer {
namespace detail {

namespace {

using Cx = std::complex<double>;

constexpr const char* kBackend = "TensorNetwork";

[[noreturn]] void reject(const std::string& what) {
    throw SimulatorError{kBackend, what};
}

// Index identity during planning: kind plus id. Two legs contract iff they match.
struct LegKey {
    bool          is_bond = true;
    std::uint32_t id      = 0;
    bool operator==(const LegKey& o) const noexcept { return is_bond == o.is_bond && id == o.id; }
};

LegKey key_of(const TensorIndex& idx) {
    return LegKey{idx.kind == TensorIndex::Kind::Bond, idx.id};
}

// A node during planning: its legs and their dims, in log2 for cost arithmetic.
struct PlanNode {
    std::vector<LegKey>        legs;
    std::vector<double>        log2_dims;
    bool                       live = true;

    double log2_size() const {
        return std::accumulate(log2_dims.begin(), log2_dims.end(), 0.0);
    }
};

double log2_shared(const PlanNode& a, const PlanNode& b) {
    double shared = 0.0;
    for (std::size_t i = 0; i < a.legs.size(); ++i) {
        for (const LegKey& lb : b.legs) {
            if (a.legs[i] == lb) { shared += a.log2_dims[i]; break; }
        }
    }
    return shared;
}

// Cost of contracting a and b: |free(a)| * |free(b)| * |shared| multiplies,
// which equals |a| * |b| / |shared|.
double log2_cost(const PlanNode& a, const PlanNode& b) {
    return a.log2_size() + b.log2_size() - log2_shared(a, b);
}

PlanNode merge_plan_nodes(const PlanNode& a, const PlanNode& b) {
    PlanNode out;
    for (std::size_t i = 0; i < a.legs.size(); ++i) {
        const bool shared = std::any_of(b.legs.begin(), b.legs.end(),
                                        [&](const LegKey& k) { return k == a.legs[i]; });
        if (!shared) { out.legs.push_back(a.legs[i]); out.log2_dims.push_back(a.log2_dims[i]); }
    }
    for (std::size_t i = 0; i < b.legs.size(); ++i) {
        const bool shared = std::any_of(a.legs.begin(), a.legs.end(),
                                        [&](const LegKey& k) { return k == b.legs[i]; });
        if (!shared) { out.legs.push_back(b.legs[i]); out.log2_dims.push_back(b.log2_dims[i]); }
    }
    return out;
}

std::vector<PlanNode> plan_nodes_of(const TensorGraph& graph) {
    std::vector<PlanNode> nodes;
    nodes.reserve(graph.node_count());
    for (const TensorNode& n : graph.nodes()) {
        PlanNode p;
        for (const TensorIndex& idx : n.indices) {
            p.legs.push_back(key_of(idx));
            p.log2_dims.push_back(std::log2(static_cast<double>(std::max(idx.dim, 1u))));
        }
        nodes.push_back(std::move(p));
    }
    return nodes;
}

// Accumulates log2(2^x + 2^y) without leaving log space.
double log2_add(double x, double y) {
    if (x < y) std::swap(x, y);
    if (y == -std::numeric_limits<double>::infinity()) return x;
    return x + std::log2(1.0 + std::exp2(y - x));
}

void record(ContractionPlan& plan, std::uint32_t a, std::uint32_t b, double cost, double size) {
    plan.steps.push_back(ContractionStep{a, b});
    plan.log2_flops = plan.steps.size() == 1 ? cost : log2_add(plan.log2_flops, cost);
    plan.log2_max_intermediate = std::max(plan.log2_max_intermediate, size);
}

// Greedy: contract the currently cheapest connected pair. Disconnected pieces
// are joined last (their contraction is an outer product, always the priciest).
ContractionPlan plan_greedy(std::vector<PlanNode> nodes) {
    ContractionPlan plan;
    for (;;) {
        double best_cost = 0.0;
        std::uint32_t best_a = 0, best_b = 0;
        bool found = false, found_connected = false;

        for (std::uint32_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i].live) continue;
            for (std::uint32_t j = i + 1; j < nodes.size(); ++j) {
                if (!nodes[j].live) continue;
                const bool connected = log2_shared(nodes[i], nodes[j]) > 0.0;
                if (found_connected && !connected) continue;
                const double cost = log2_cost(nodes[i], nodes[j]);
                if (!found || (connected && !found_connected) || cost < best_cost) {
                    best_cost = cost; best_a = i; best_b = j;
                    found = true; found_connected = found_connected || connected;
                }
            }
        }
        if (!found) break;

        PlanNode merged = merge_plan_nodes(nodes[best_a], nodes[best_b]);
        record(plan, best_a, best_b, best_cost, merged.log2_size());
        nodes[best_a] = std::move(merged);
        nodes[best_b].live = false;
    }
    return plan;
}

// Recursive bisection: split the live set in two along a small bond cut, plan
// each side, then join. The split is a linear sweep over the current node order,
// which is the right heuristic for the chain-like networks a circuit produces —
// a full min-cut search would cost more than it saves here ( excludes
// cotengra-class search).
void plan_bisect(std::vector<PlanNode>& nodes, std::vector<std::uint32_t> members,
                 ContractionPlan& plan, std::uint32_t* root) {
    if (members.size() == 1) { *root = members[0]; return; }
    if (members.size() == 2) {
        const double cost = log2_cost(nodes[members[0]], nodes[members[1]]);
        PlanNode merged = merge_plan_nodes(nodes[members[0]], nodes[members[1]]);
        record(plan, members[0], members[1], cost, merged.log2_size());
        nodes[members[0]] = std::move(merged);
        nodes[members[1]].live = false;
        *root = members[0];
        return;
    }

    // Pick the sweep position whose cut carries the least weight.
    //
    // Costed incrementally rather than re-summed per position. Recomputing every
    // left x right pair at each split is O(m^3) per level, which measured at
    // ~O(n^3.8) overall — 23.8 s for 408 nodes, i.e. hours for a real 50-qubit
    // circuit. Precomputing the pair weights once and sliding the boundary makes
    // it O(m^2), since moving from `split` to `split+1` only moves one member
    // across: cut(s+1) = cut(s) - sum_{i<s} W[i][s] + sum_{j>s} W[s][j].
    const std::size_t m = members.size();
    std::vector<double> weight(m * m, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = i + 1; j < m; ++j) {
            const double w = log2_shared(nodes[members[i]], nodes[members[j]]);
            weight[i * m + j] = w;
            weight[j * m + i] = w;
        }
    }

    double cut = 0.0;
    for (std::size_t j = 1; j < m; ++j) cut += weight[0 * m + j];

    std::size_t best_split = 1;
    double best_cut = cut;
    for (std::size_t split = 1; split + 1 < m; ++split) {
        for (std::size_t i = 0; i < split; ++i)   cut -= weight[i * m + split];
        for (std::size_t j = split + 1; j < m; ++j) cut += weight[split * m + j];
        if (cut < best_cut) { best_cut = cut; best_split = split + 1; }
    }

    std::vector<std::uint32_t> left(members.begin(), members.begin() + static_cast<long>(best_split));
    std::vector<std::uint32_t> right(members.begin() + static_cast<long>(best_split), members.end());

    std::uint32_t left_root = 0, right_root = 0;
    plan_bisect(nodes, left, plan, &left_root);
    plan_bisect(nodes, right, plan, &right_root);

    const double cost = log2_cost(nodes[left_root], nodes[right_root]);
    PlanNode merged = merge_plan_nodes(nodes[left_root], nodes[right_root]);
    record(plan, left_root, right_root, cost, merged.log2_size());
    nodes[left_root] = std::move(merged);
    nodes[right_root].live = false;
    *root = left_root;
}

}  // namespace

ContractionPlan ContractionOrderOptimizer::plan(const TensorGraph& graph,
                                                ContractionStrategy strategy) {
    if (graph.node_count() == 0) reject("cannot plan a contraction of an empty network");

    std::vector<PlanNode> nodes = plan_nodes_of(graph);
    if (strategy == ContractionStrategy::Greedy) return plan_greedy(std::move(nodes));

    ContractionPlan plan;
    std::vector<std::uint32_t> members(nodes.size());
    std::iota(members.begin(), members.end(), 0u);
    std::uint32_t root = 0;
    plan_bisect(nodes, members, plan, &root);
    return plan;
}

TensorNode contract_pair(const TensorNode& a, const TensorNode& b) {
    // Partition a's legs into shared / free, and mirror the pairing on b so the
    // shared axes are walked in the same order on both sides.
    std::vector<std::size_t> a_shared, a_free, b_shared, b_free;
    for (std::size_t i = 0; i < a.indices.size(); ++i) {
        bool matched = false;
        for (std::size_t j = 0; j < b.indices.size(); ++j) {
            if (key_of(a.indices[i]) == key_of(b.indices[j])) {
                if (a.indices[i].dim != b.indices[j].dim) {
                    reject("shared index dimension mismatch during contraction");
                }
                a_shared.push_back(i);
                b_shared.push_back(j);
                matched = true;
                break;
            }
        }
        if (!matched) a_free.push_back(i);
    }
    for (std::size_t j = 0; j < b.indices.size(); ++j) {
        if (std::find(b_shared.begin(), b_shared.end(), j) == b_shared.end()) b_free.push_back(j);
    }

    const auto extent = [](const TensorNode& n, const std::vector<std::size_t>& axes) {
        std::size_t e = 1;
        for (std::size_t ax : axes) e *= n.indices[ax].dim;
        return e;
    };
    const std::size_t free_a = extent(a, a_free);
    const std::size_t free_b = extent(b, b_free);
    const std::size_t shared = extent(a, a_shared);

    // Strides let the loops read the row-major buffers without materialising a
    // permuted copy — the copy would double the memory traffic of every step.
    const auto strides = [](const TensorNode& n) {
        std::vector<std::size_t> s(n.indices.size(), 1);
        for (std::size_t i = n.indices.size(); i-- > 1;) s[i - 1] = s[i] * n.indices[i].dim;
        return s;
    };
    const std::vector<std::size_t> sa = strides(a);
    const std::vector<std::size_t> sb = strides(b);

    const auto offset = [](const std::vector<std::size_t>& stride,
                           const TensorNode& n,
                           const std::vector<std::size_t>& axes,
                           std::size_t flat) {
        std::size_t off = 0;
        for (std::size_t k = axes.size(); k-- > 0;) {
            const std::size_t dim = n.indices[axes[k]].dim;
            off += (flat % dim) * stride[axes[k]];
            flat /= dim;
        }
        return off;
    };

    TensorNode out;
    out.device = a.device;
    for (std::size_t ax : a_free) out.indices.push_back(a.indices[ax]);
    for (std::size_t ax : b_free) out.indices.push_back(b.indices[ax]);
    out.data.assign(free_a * free_b, Cx{0, 0});

    for (std::size_t fa = 0; fa < free_a; ++fa) {
        const std::size_t base_a = offset(sa, a, a_free, fa);
        for (std::size_t fb = 0; fb < free_b; ++fb) {
            const std::size_t base_b = offset(sb, b, b_free, fb);
            Cx acc{0, 0};
            for (std::size_t s = 0; s < shared; ++s) {
                acc += a.data[base_a + offset(sa, a, a_shared, s)] *
                       b.data[base_b + offset(sb, b, b_shared, s)];
            }
            out.data[fa * free_b + fb] = acc;
        }
    }
    return out;
}

TensorNode contract_network(const TensorGraph& graph, const ContractionPlan& plan) {
    if (graph.node_count() == 0) reject("cannot contract an empty network");

    std::vector<TensorNode> live = graph.nodes();
    std::vector<bool> alive(live.size(), true);

    for (const ContractionStep& step : plan.steps) {
        if (step.node_a >= live.size() || step.node_b >= live.size() ||
            !alive[step.node_a] || !alive[step.node_b]) {
            reject("contraction plan references a node that is not live");
        }
        live[step.node_a] = contract_pair(live[step.node_a], live[step.node_b]);
        alive[step.node_b] = false;
    }

    std::size_t remaining = 0;
    std::size_t root = 0;
    for (std::size_t i = 0; i < alive.size(); ++i) {
        if (alive[i]) { ++remaining; root = i; }
    }
    if (remaining != 1) {
        reject("contraction plan left " + std::to_string(remaining) + " tensors instead of 1");
    }
    return live[root];
}

TensorGraph build_circuit_network(const Circuit& circuit) {
    const QubitIndex n = circuit.num_qubits();
    if (n == 0) reject("cannot build a network for a 0-qubit circuit");

    TensorGraph graph;
    // Bond ids are allocated per wire segment: `wire[q]` names the leg currently
    // dangling on qubit q, and every gate consumes it and produces a new one.
    std::vector<std::uint32_t> wire(n, 0);
    std::uint32_t next_bond = 0;

    for (QubitIndex q = 0; q < n; ++q) {
        TensorNode ket;
        wire[q] = next_bond++;
        ket.indices = {TensorIndex{TensorIndex::Kind::Bond, 2, wire[q]}};
        ket.data = {Cx{1, 0}, Cx{0, 0}};  // |0>
        graph.add_node(std::move(ket));
    }

    for (const Op& op : circuit.ops()) {
        const auto* gate = std::get_if<GateOp>(&op);
        if (gate == nullptr) continue;  // measurement / barrier: not part of the unitary network
        if (gate->is_opaque || gate->type == GateType::Opaque) {
            reject("opaque gates cannot be expressed in the circuit network yet");
        }
        if (gate->qubits.empty()) continue;

        const std::vector<Cx> matrix = gate_full_matrix(*gate);
        const std::size_t k = gate->qubits.size();

        TensorNode node;
        node.data = matrix;
        // Row axes are the outputs (new wires), column axes the inputs (old wires).
        // gate_full_matrix packs local bit b of an index as operand b, and this
        // layout is row-major over [out_0 ... out_{k-1}][in_0 ... in_{k-1}] once
        // each 2^k axis is split into k binary axes — which is exactly the bit
        // order the matrix already has.
        std::vector<std::uint32_t> out_bonds(k, 0);
        for (std::size_t b = k; b-- > 0;) out_bonds[b] = next_bond++;
        for (std::size_t b = 0; b < k; ++b) {
            node.indices.push_back(TensorIndex{TensorIndex::Kind::Bond, 2, out_bonds[k - 1 - b]});
        }
        for (std::size_t b = 0; b < k; ++b) {
            node.indices.push_back(TensorIndex{TensorIndex::Kind::Bond, 2, wire[gate->qubits[k - 1 - b]]});
        }
        graph.add_node(std::move(node));
        for (std::size_t b = 0; b < k; ++b) wire[gate->qubits[b]] = out_bonds[b];
    }

    // Mark the dangling end of each wire as that qubit's physical leg. Without
    // this the contracted result carries anonymous bond ids and there is no way
    // to say which axis is which qubit.
    for (QubitIndex q = 0; q < n; ++q) {
        for (std::uint32_t id = 0; id < graph.node_count(); ++id) {
            for (TensorIndex& idx : graph.node(id).indices) {
                if (idx.kind == TensorIndex::Kind::Bond && idx.id == wire[q]) {
                    idx.kind = TensorIndex::Kind::Physical;
                    idx.id   = q;
                }
            }
        }
    }
    return graph;
}

std::vector<Cx> contract_to_statevector(const Circuit& circuit, ContractionStrategy strategy) {
    const QubitIndex n = circuit.num_qubits();
    if (n > MAX_DENSE_EXPORT_QUBITS) {
        reject("contract_to_statevector exceeds MAX_DENSE_EXPORT_QUBITS (" +
               std::to_string(MAX_DENSE_EXPORT_QUBITS) + ")");
    }

    const TensorGraph graph = build_circuit_network(circuit);
    const TensorNode result = contract_network(graph, ContractionOrderOptimizer::plan(graph, strategy));

    if (result.indices.size() != n) {
        reject("contracted network left " + std::to_string(result.indices.size()) +
               " open legs for " + std::to_string(n) + " qubits");
    }

    // The contraction decides the axis order, so map each axis back to its qubit
    // rather than assuming ascending order.
    std::vector<std::uint32_t> qubit_of_axis(result.indices.size(), 0);
    for (std::size_t ax = 0; ax < result.indices.size(); ++ax) {
        if (result.indices[ax].kind != TensorIndex::Kind::Physical) {
            reject("contracted network has a leftover bond leg");
        }
        qubit_of_axis[ax] = result.indices[ax].id;
    }

    std::vector<Cx> dense(std::size_t{1} << n, Cx{0, 0});
    for (std::size_t flat = 0; flat < result.data.size(); ++flat) {
        std::size_t rest = flat;
        std::size_t basis = 0;
        for (std::size_t ax = result.indices.size(); ax-- > 0;) {
            if (rest & 1u) basis |= std::size_t{1} << qubit_of_axis[ax];
            rest >>= 1;
        }
        dense[basis] = result.data[flat];
    }
    return dense;
}

}  // namespace detail
}  // namespace qplayer
