// feasibility.cpp — SparseState pre-flight feasibility analysis.
// See feasibility.hpp for the contract.

#include "backend/sparse_state/feasibility.hpp"

#include <cmath>     // std::hypot
#include <variant>   // std::get_if

#include "backend/full_state/gate_matrix.hpp"  // Mat2x2, gate_matrix()
#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer::detail {

namespace {

// S is capped at 2^56 so the byte product S × kSparseEntryBytes (168) cannot
// overflow uint64 (2^56 × 168 ≈ 1.2e19 < UINT64_MAX ≈ 1.84e19). Any S this large
// dwarfs every real memory budget, so saturating here loses no decision power:
// such a circuit is infeasible regardless of the exact count.
constexpr int        kSatShift = 56;
constexpr StateIndex kStateSaturation = StateIndex{1} << kSatShift;

// Caps S at min(2^num_qubits, kStateSaturation): a circuit can never hold more
// non-zero states than basis states, and we never track past the saturation
// sentinel.
StateIndex apply_cap(StateIndex s, QubitIndex num_qubits) {
    StateIndex cap = kStateSaturation;
    if (num_qubits < static_cast<QubitIndex>(kSatShift)) {
        const StateIndex n_cap = StateIndex{1} << num_qubits;
        if (n_cap < cap) {
            cap = n_cap;
        }
    }
    return (s > cap) ? cap : s;
}

// Decides whether a gate can increase the non-zero count (a "branch"). Rather
// than enumerate GateType cases, inspect the 2x2 target sub-unitary: an
// off-diagonal coupling splits a basis state into a sibling (S can double),
// while a purely diagonal matrix only rephases (S unchanged). A matrix with zero
// diagonal and non-zero off-diagonal is a pure permutation (e.g. X, RY(pi)),
// which relabels indices without changing S. This auto-handles RX/RY/U2/U3 angle
// special cases (theta=0 -> identity-like diagonal, theta=pi -> permutation) and
// stays correct if new gate types are added. SWAP/CSwap/CCX never reach here
// (they are pure permutations handled by the caller); Reset/measure preserve S.
bool gate_branches(const GateOp& g) {
    switch (g.type) {
        // Permutations and no-ops the gate_matrix() table does not cover, or that
        // are known to preserve S without inspecting a matrix.
        case GateType::I:
        case GateType::X:
        case GateType::Y:      // X/Y are anti-diagonal permutations
        case GateType::CX:
        case GateType::CY:
        case GateType::SWAP:
        case GateType::CCX:
        case GateType::CSwap:
        case GateType::Reset:
            return false;
        // Opaque body is unknown — conservatively assume it can branch.
        case GateType::Opaque:
            return true;
        default:
            break;
    }

    const Mat2x2 U = gate_matrix(g.type, g.params);
    constexpr double kEps = 1e-12;
    const double off01 = std::hypot(U.u01r, U.u01i);
    const double off10 = std::hypot(U.u10r, U.u10i);
    const double diag0 = std::hypot(U.u00r, U.u00i);
    const double diag1 = std::hypot(U.u11r, U.u11i);

    // Pure permutation (e.g. RY(pi), U3(pi,..)): zero diagonal, non-zero
    // off-diagonal -> relabels |0><->|1|, S unchanged.
    if (diag0 < kEps && diag1 < kEps && off01 > kEps && off10 > kEps) {
        return false;
    }
    // Diagonal (Z, S, T, RZ, U1, ...): no off-diagonal coupling -> S unchanged.
    if (off01 < kEps && off10 < kEps) {
        return false;
    }
    // Any remaining off-diagonal coupling can split a basis state -> branch.
    return true;
}

// Applies one gate's effect to the running S upper bound.
void step(StateIndex& s, const GateOp& g, QubitIndex num_qubits) {
    if (gate_branches(g)) {
        s = apply_cap(s * 2, num_qubits);
    }
    // Diagonal / permutation / no-op: S unchanged.
}

}  // namespace

SparseFeasibility analyze_sparse_feasibility(const Circuit& circuit,
                                             std::uint64_t  budget_bytes) {
    const QubitIndex num_qubits = circuit.num_qubits();

    StateIndex s = StateIndex{1};  // ground state |0...0> — one non-zero amplitude
    for (const Op& op : circuit.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            step(s, *g, num_qubits);
        } else if (const auto* iff = std::get_if<IfOp>(&op)) {
            // Conservative: assume the classically-controlled body always runs
            // (upper bound). MeasureOp/BarrierOp/IdleOp preserve S.
            step(s, iff->body, num_qubits);
        }
    }

    // s is capped at 2^56, so the cast and product cannot overflow uint64.
    const std::uint64_t s64        = static_cast<std::uint64_t>(s);
    const std::uint64_t est_bytes  = s64 * kSparseEntryBytes;

    return SparseFeasibility{
        /*estimated_max_states=*/s,
        /*estimated_bytes=*/est_bytes,
        /*budget_bytes=*/budget_bytes,
        /*feasible=*/est_bytes <= budget_bytes,
    };
}

}  // namespace qplayer::detail
