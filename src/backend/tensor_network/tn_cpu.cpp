/// @file tn_cpu.cpp
/// @brief TensorGraph storage and MPS initialisation for the TensorNetwork backend.
///
/// Implements the tensor-graph types declared in tn_backend.hpp
/// (11.1): node construction, bond wiring with dimension
/// validation, and the |0...0> MPS chain. Gate application, SVD
/// truncation and contraction ordering arrive
/// with and.

#include "backend/tensor_network/tn_backend.hpp"

#include "error.hpp"

#include <algorithm>
#include <string>

namespace qplayer {
namespace detail {

namespace {

constexpr const char* kBackend = "TensorNetwork";

// Index positions inside an MPS site tensor: [left bond, physical, right bond].
// Sites are uniformly 3-index — boundary bonds stay explicit at dim 1 so no
// routine needs an end-of-chain special case ( rationale carried
// the storage layout).
constexpr std::uint32_t kLeftBond  = 0;
constexpr std::uint32_t kPhysical  = 1;
constexpr std::uint32_t kRightBond = 2;
constexpr std::size_t   kMpsRank   = 3;

[[noreturn]] void reject(const std::string& what) {
    throw SimulatorError{kBackend, what};
}

}  // namespace

std::size_t TensorNode::element_count() const noexcept {
    std::size_t n = 1;
    for (const TensorIndex& idx : indices) n *= static_cast<std::size_t>(idx.dim);
    return n;
}

std::uint32_t TensorGraph::add_node(TensorNode node) {
    // Checked here rather than at contraction time: a length mismatch reads as
    // garbage amplitudes rather than as an error once a kernel walks the buffer.
    if (node.data.size() != node.element_count()) {
        reject("tensor data length " + std::to_string(node.data.size()) +
               " does not match the index dims product " + std::to_string(node.element_count()));
    }
    nodes_.push_back(std::move(node));
    return static_cast<std::uint32_t>(nodes_.size() - 1);
}

void TensorGraph::connect(std::uint32_t node_a, std::uint32_t index_a,
                          std::uint32_t node_b, std::uint32_t index_b) {
    if (node_a >= nodes_.size() || node_b >= nodes_.size()) {
        reject("connect node id out of range");
    }
    if (node_a == node_b) {
        reject("connect requires two distinct nodes (self-contraction is a trace, not a bond)");
    }
    if (index_a >= nodes_[node_a].indices.size() || index_b >= nodes_[node_b].indices.size()) {
        reject("connect index position out of range");
    }

    const TensorIndex& leg_a = nodes_[node_a].indices[index_a];
    const TensorIndex& leg_b = nodes_[node_b].indices[index_b];

    // Physical legs carry the observable qubit and must survive every contraction.
    if (leg_a.kind != TensorIndex::Kind::Bond || leg_b.kind != TensorIndex::Kind::Bond) {
        reject("connect requires bond-kind legs on both sides");
    }
    if (leg_a.dim != leg_b.dim) {
        reject("bond dim mismatch: " + std::to_string(leg_a.dim) + " vs " + std::to_string(leg_b.dim));
    }

    const auto already_bonded = [this](std::uint32_t n, std::uint32_t i) {
        return std::any_of(bonds_.begin(), bonds_.end(), [&](const Bond& b) {
            return (b.node_a == n && b.index_a == i) || (b.node_b == n && b.index_b == i);
        });
    };
    if (already_bonded(node_a, index_a) || already_bonded(node_b, index_b)) {
        reject("leg is already bonded (one leg belongs to at most one bond)");
    }

    bonds_.push_back(Bond{node_a, index_a, node_b, index_b});
}

const TensorNode& TensorGraph::node(std::uint32_t id) const {
    if (id >= nodes_.size()) reject("node id out of range");
    return nodes_[id];
}

TensorNode& TensorGraph::node(std::uint32_t id) {
    if (id >= nodes_.size()) reject("node id out of range");
    return nodes_[id];
}

std::uint32_t TensorGraph::max_bond_dimension() const noexcept {
    std::uint32_t chi = 0;
    for (const TensorNode& n : nodes_) {
        for (const TensorIndex& idx : n.indices) {
            if (idx.kind == TensorIndex::Kind::Bond) chi = std::max(chi, idx.dim);
        }
    }
    return chi;
}

bool TensorGraph::is_mps_chain() const noexcept {
    const std::size_t n = nodes_.size();
    if (n == 0) return false;
    if (bonds_.size() != n - 1) return false;

    for (std::size_t i = 0; i < n; ++i) {
        const TensorNode& site = nodes_[i];
        if (site.indices.size() != kMpsRank) return false;
        if (site.indices[kLeftBond].kind  != TensorIndex::Kind::Bond)     return false;
        if (site.indices[kRightBond].kind != TensorIndex::Kind::Bond)     return false;
        if (site.indices[kPhysical].kind  != TensorIndex::Kind::Physical) return false;
        if (site.indices[kPhysical].dim != 2) return false;
        // Site i must carry physical qubit i: gate dispatch indexes sites by
        // qubit number, so a permuted chain would apply gates to the wrong site.
        if (site.indices[kPhysical].id != static_cast<std::uint32_t>(i)) return false;
    }

    // Open boundary conditions: the dangling ends carry no entanglement.
    if (nodes_.front().indices[kLeftBond].dim  != 1) return false;
    if (nodes_.back().indices[kRightBond].dim != 1) return false;

    // Each bond must join site i's right leg to site i+1's left leg. Bond order
    // is not assumed — only the set of joins matters.
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const bool joined = std::any_of(bonds_.begin(), bonds_.end(), [&](const Bond& b) {
            const bool forward = b.node_a == i && b.index_a == kRightBond &&
                                 b.node_b == i + 1 && b.index_b == kLeftBond;
            const bool reverse = b.node_b == i && b.index_b == kRightBond &&
                                 b.node_a == i + 1 && b.index_a == kLeftBond;
            return forward || reverse;
        });
        if (!joined) return false;
    }
    return true;
}

TensorGraph TensorGraph::mps_zero_state(QubitIndex num_qubits) {
    if (num_qubits == 0) {
        reject("mps_zero_state requires at least one qubit");
    }
    if (num_qubits > kMaxTensorNetworkQubits) {
        reject("num_qubits " + std::to_string(num_qubits) + " exceeds the structural ceiling " +
               std::to_string(kMaxTensorNetworkQubits));
    }

    TensorGraph graph;
    for (QubitIndex q = 0; q < num_qubits; ++q) {
        TensorNode site;
        // Bond ids number the cuts left-to-right: site q sits between cut q and q+1.
        site.indices = {
            TensorIndex{TensorIndex::Kind::Bond,     1, q},
            TensorIndex{TensorIndex::Kind::Physical, 2, q},
            TensorIndex{TensorIndex::Kind::Bond,     1, q + 1},
        };
        // 1 x 2 x 1 tensor holding |0> — the product state needs no entanglement.
        site.data = {std::complex<double>{1.0, 0.0}, std::complex<double>{0.0, 0.0}};
        graph.add_node(std::move(site));
    }
    for (QubitIndex q = 0; q + 1 < num_qubits; ++q) {
        graph.connect(q, kRightBond, q + 1, kLeftBond);
    }
    return graph;
}

}  // namespace detail
}  // namespace qplayer
