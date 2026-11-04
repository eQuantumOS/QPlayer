/// @file contraction_order.hpp
/// @brief Contraction order planning and execution for general tensor networks.
/// @ingroup backend
///
/// Contracting a network pairwise is associative in result but not in cost: the
/// order decides how large the intermediate tensors get, and a bad order can
/// blow up exponentially on a network a good order handles easily. This plans an
/// order (greedy or bisection) and executes it. Full search — the cotengra class
/// of optimiser — is out of scope.

#pragma once

#include "backend/tensor_network/tn_backend.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace qplayer {

class Circuit;

namespace detail {

/// Strategy used to order pairwise contractions.
/// @ingroup backend
enum class ContractionStrategy {
    /// Repeatedly contract the pair whose result is cheapest right now. Cheap to
    /// compute and good on chains; it can be short-sighted on dense graphs.
    Greedy,
    /// Recursively split the graph along a small bond cut and contract each side
    /// first. Costlier to plan, but it keeps intermediates small on graphs where
    /// greedy walks into a large one. A bisection heuristic, not exact min-cut.
    MinCut,
};

/// One pairwise contraction: the two live node ids to merge.
/// @ingroup backend
struct ContractionStep {
    std::uint32_t node_a = 0;
    std::uint32_t node_b = 0;
};

/// A full contraction order plus what it is expected to cost.
/// @ingroup backend
struct ContractionPlan {
    /// Steps in execution order; the result of a step reuses node_a's id.
    std::vector<ContractionStep> steps;

    /// log2 of the total multiply count. Logarithmic because a bad order on 50
    /// sites overflows any integer counter long before it overflows memory.
    double log2_flops = 0.0;

    /// log2 of the largest intermediate tensor's element count — the number that
    /// actually decides whether a plan fits in memory.
    double log2_max_intermediate = 0.0;
};

/// Plans a pairwise contraction order over a tensor network.
/// @ingroup backend
class ContractionOrderOptimizer {
public:
    /// Builds an order covering every node exactly once.
    /// throws: SimulatorError — the graph is empty.
    static ContractionPlan plan(const TensorGraph& graph, ContractionStrategy strategy);
};

/// Contracts two nodes over every index they share, returning the merged node.
/// Free indices keep their order: a's first, then b's — which is what makes the
/// executor's bookkeeping predictable.
/// throws: SimulatorError — shared indices disagree on dimension.
/// @ingroup backend
TensorNode contract_pair(const TensorNode& a, const TensorNode& b);

/// Executes a plan and returns the single remaining tensor.
/// throws: SimulatorError — the plan does not match the graph (dead id, or nodes left over).
/// @ingroup backend
TensorNode contract_network(const TensorGraph& graph, const ContractionPlan& plan);

/// Builds the tensor network of a circuit acting on |0...0>: one node per qubit
/// initial state, one per gate, wired along each qubit's timeline.
///
/// The open physical legs are the final qubit indices in ascending order, so a
/// full contraction yields the statevector directly. This is the general-TN
/// counterpart of the MPS path and exists to cross-check it.
/// throws: SimulatorError — the circuit uses an op family the builder cannot express.
/// @ingroup backend
TensorGraph build_circuit_network(const Circuit& circuit);

/// Contracts a circuit's full network into a dense statevector.
///
/// The general-TN counterpart of the MPS path: same circuit, entirely different
/// machinery, so disagreement between the two localises a bug to one of them.
/// O(2^n) in the result alone, hence the export cap.
/// throws: SimulatorError — num_qubits exceeds MAX_DENSE_EXPORT_QUBITS, or the
///   circuit uses an op family the builder cannot express.
/// @ingroup backend
std::vector<std::complex<double>> contract_to_statevector(const Circuit& circuit,
                                                         ContractionStrategy strategy);

}  // namespace detail
}  // namespace qplayer
