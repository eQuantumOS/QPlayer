/// @file topology.hpp
/// @brief Circuit interaction-topology analysis for MPS vs general TN selection.
/// @ingroup backend
///
/// An MPS is a chain, so it represents states cheaply exactly when the circuit's
/// two-qubit interactions also form a chain. This analyser extracts the coupling
/// graph a circuit induces, decides whether that graph is chain-like, and — when
/// it is — reports the order the sites should be laid out in. A star or a grid
/// needs a general tensor network instead.

#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace qplayer {

class Circuit;

namespace detail {

/// Which tensor-network representation a circuit's interaction graph calls for.
/// @ingroup backend
enum class TnTopology {
    /// Every connected component is a path — an MPS can lay the sites out so all
    /// two-qubit gates act on neighbours.
    Linear,
    /// A branch or a cycle exists (star, heavy-hex, grid), so no site ordering
    /// makes every interaction adjacent; a general TN is the right structure.
    NonLinear,
};

/// How a NonLinear circuit's qubits are placed on the chain.
/// @ingroup backend
enum class NonLinearLayout {
    /// Qubit number = site. The default: the sign of any re-layout's effect flips
    /// with the noise and the cutoff, and this keeps a user-chosen numbering.
    Identity,
    /// Weighted minimum linear arrangement, adopted only when it walks
    /// strictly less than the identity. Opt-in via QPLAYER_TN_LAYOUT=minla.
    MinLA,
};

/// The interaction graph a circuit induces, plus the layout conclusions drawn from it.
/// @ingroup backend
struct TopologyAnalysis {
    /// Classification driving the representation choice.
    TnTopology topology = TnTopology::Linear;

    /// Qubit placed on each site: chain_order[i] sits on site i.
    ///
    /// Linear: the path order, which makes every 2Q gate adjacent. NonLinear: the
    /// identity by default; under NonLinearLayout::MinLA a minimum-linear-
    /// arrangement order when it walks less than the identity (0950).
    std::vector<QubitIndex> chain_order;

    /// Sum over 2Q interactions of weight x site distance, under chain_order and
    /// under the identity. A gate at distance k costs 2k-1 adjacent two-site
    /// steps, so these rank layouts by SVD count.
    std::uint64_t layout_cost = 0;
    std::uint64_t identity_layout_cost = 0;

    /// Distinct qubit pairs touched by two-qubit gates — the graph's edge count.
    std::size_t coupling_edges = 0;

    /// Highest interaction degree; >= 3 is what makes a graph non-chain-like.
    std::size_t max_degree = 0;

    /// Connected components counting isolated qubits — a circuit may split into
    /// several independent chains, which an MPS still handles in one layout.
    std::size_t components = 0;

    /// True when chain_order is 0, 1, 2, ... — i.e. the circuit is already laid out well.
    bool is_identity_order() const noexcept;
};

/// Extracts and classifies the coupling graph of a circuit.
///
/// Isolated qubits (no two-qubit gate touches them) never break linearity: they
/// can sit anywhere in the chain, so they are appended rather than treated as a
/// branch. That also makes a gate-free or purely single-qubit circuit Linear,
/// which is the honest answer — such a circuit needs no bond at all.
/// @ingroup backend
class CircuitTopologyAnalyzer {
public:
    /// Analyses the interactions the circuit's two-qubit gates induce.
    /// Gates on three or more qubits contribute every pair among their operands,
    /// which is the conservative reading — such a gate needs all of them close.
    /// Each gate adds one unit of weight to its pairs, so the NonLinear layout
    /// favours the pairs that interact most.
    static TopologyAnalysis analyze(const Circuit& circuit,
                                    NonLinearLayout layout = NonLinearLayout::Identity);

    /// Analyses an explicit coupling list, for the F-CC case where a device
    /// coupling map is authoritative rather than the gate pattern.
    /// A pair listed k times weighs k in the NonLinear layout.
    static TopologyAnalysis analyze(const std::vector<std::pair<QubitIndex, QubitIndex>>& coupling,
                                    QubitIndex num_qubits,
                                    NonLinearLayout layout = NonLinearLayout::Identity);
};

}  // namespace detail
}  // namespace qplayer
