/// @file merge_single_qubit_pass.cpp
/// @brief MergeSingleQubitPass (Phase 4).
///
/// Merges consecutive same-qubit 1Q gate sequences into a single gate using a
/// fixed pattern table. Only non-rotation Clifford+T gates are merged; rotation
/// gates (RX/RY/RZ/U1/U2/U3) act as boundaries (full matrix composition with
/// arbitrary angles is a Phase 4+ enhancement).
///
/// Algorithm: for each qubit, maintain a pending-gate window. When the window
/// width reaches a recognisable pattern, replace it with the merged gate and mark
/// changed_=true. Unrecognised windows are flushed as-is on boundary or rotation.
///
/// Because different qubits are independent, the pass tracks a per-qubit window
/// of GateType values and the corresponding op-list indices. On a flush or a
/// match the indices are replaced with the merged gate.
///
/// Pattern table (global phase ignored per and):
/// 2-gate:  S,S -> Z   Sdg,Sdg -> Z   T,T -> S   Tdg,Tdg -> Sdg
/// 3-gate:  H,X,H -> Z   H,Z,H -> X
///

#include "transpiler/passes/optimization/merge_single_qubit_pass.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

// Returns true when the GateOp is a 1-qubit gate eligible for pattern merging
// (i.e. not opaque, not rotation, not multi-qubit).
bool is_eligible_1q(const GateOp& g) {
    if (g.is_opaque) return false;
    if (g.qubits.size() != 1) return false;
    // Rotation gate types are boundaries for this pass.
    switch (g.type) {
        case GateType::RX: case GateType::RY: case GateType::RZ:
        case GateType::U1: case GateType::U2: case GateType::U3:
            return false;
        default:
            return true;
    }
}

// Returns true when op is a boundary for the merge window.
bool is_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

// Encode a 2-gate sequence as a 64-bit key for fast table lookup.
constexpr std::uint64_t key2(GateType a, GateType b) {
    return (static_cast<std::uint64_t>(a) << 32) |
            static_cast<std::uint64_t>(b);
}

// Encode a 3-gate sequence.
constexpr std::uint64_t key3(GateType a, GateType b, GateType c) {
    return (static_cast<std::uint64_t>(a) << 42) |
           (static_cast<std::uint64_t>(b) << 21) |
            static_cast<std::uint64_t>(c);
}

// Try to match a 2- or 3-gate window on a single qubit to a known pattern.
// Returns the replacement GateType if matched, nullopt otherwise.
std::optional<GateType> match_pattern_2(GateType a, GateType b) {
    // S  S    -> Z
    if (a == GateType::S   && b == GateType::S)   return GateType::Z;
    // Sdg Sdg -> Z
    if (a == GateType::Sdg && b == GateType::Sdg) return GateType::Z;
    // T  T    -> S
    if (a == GateType::T   && b == GateType::T)   return GateType::S;
    // Tdg Tdg -> Sdg
    if (a == GateType::Tdg && b == GateType::Tdg) return GateType::Sdg;
    return std::nullopt;
}

std::optional<GateType> match_pattern_3(GateType a, GateType b, GateType c) {
    // H X H -> Z
    if (a == GateType::H && b == GateType::X && c == GateType::H) return GateType::Z;
    // H Z H -> X
    if (a == GateType::H && b == GateType::Z && c == GateType::H) return GateType::X;
    return std::nullopt;
}

// Canonical single-qubit gate name strings for GateOp.name field.
const char* gate_name(GateType t) {
    switch (t) {
        case GateType::I:   return "id";
        case GateType::X:   return "x";
        case GateType::Y:   return "y";
        case GateType::Z:   return "z";
        case GateType::H:   return "h";
        case GateType::S:   return "s";
        case GateType::Sdg: return "sdg";
        case GateType::T:   return "t";
        case GateType::Tdg: return "tdg";
        default:            return "unknown";
    }
}

// Per-qubit window bookkeeping: holds a sliding list of consecutive eligible-1Q
// op indices in the main ops vector.
struct QubitWindow {
    std::vector<std::size_t> indices;  // positions in ops[] of pending 1Q gates
    std::vector<GateType>    types;    // gate types at those positions

    void reset() {
        indices.clear();
        types.clear();
    }
};

}  // anonymous namespace

std::string MergeSingleQubitPass::name() const {
    return "MergeSingleQubitPass";
}

Circuit MergeSingleQubitPass::run(Circuit circuit) {
    changed_ = false;

    std::vector<Op> ops(circuit.ops().begin(), circuit.ops().end());
    const QubitIndex nq = circuit.num_qubits();

    // Per-qubit windows.
    std::vector<QubitWindow> windows(static_cast<std::size_t>(nq));

    // A set of op indices to mark as "replaced" — replaced indices will be
    // turned into GateType::I (identity) and filtered out later, except for
    // the surviving merged-gate index which gets updated in-place.
    // We accumulate (merge_into_index, new_type, qubit) replacements.
    struct Replacement {
        std::size_t surviving_idx;     // index in ops[] that holds the merged gate
        std::vector<std::size_t> drop; // indices to remove
        GateType merged_type;
        QubitIndex qubit;
    };
    std::vector<Replacement> replacements;

    // Flush the per-qubit window: attempt to match known patterns; on match
    // record a replacement, then reset the window.
    auto try_flush_window = [&](QubitIndex q) {
        QubitWindow& w = windows[q];
        if (w.types.size() >= 3) {
            // Try 3-gate pattern on the last 3 entries.
            auto n = w.types.size();
            auto m = match_pattern_3(w.types[n-3], w.types[n-2], w.types[n-1]);
            if (m.has_value()) {
                Replacement r;
                r.surviving_idx = w.indices[n-3];
                r.drop = {w.indices[n-2], w.indices[n-1]};
                r.merged_type   = *m;
                r.qubit         = q;
                replacements.push_back(r);
                // Consume only the last 3 ops from the window.
                for (int k = 0; k < 3; ++k) {
                    w.indices.pop_back();
                    w.types.pop_back();
                }
                w.reset();  // conservatively flush rest — may revisit
                return;
            }
        }
        if (w.types.size() >= 2) {
            auto n = w.types.size();
            auto m = match_pattern_2(w.types[n-2], w.types[n-1]);
            if (m.has_value()) {
                Replacement r;
                r.surviving_idx = w.indices[n-2];
                r.drop = {w.indices[n-1]};
                r.merged_type   = *m;
                r.qubit         = q;
                replacements.push_back(r);
                // Consume the last 2 ops.
                w.indices.pop_back();
                w.indices.pop_back();
                w.types.pop_back();
                w.types.pop_back();
                // Remaining earlier ops in the window are flushed as-is.
                w.reset();
                return;
            }
        }
        // No match — just flush the window as-is (no change).
        w.reset();
    };

    // First pass: identify windows and collect replacements.
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];

        if (is_boundary(op)) {
            // Flush all qubit windows on any boundary.
            for (QubitIndex q = 0; q < nq; ++q) {
                if (!windows[q].indices.empty()) try_flush_window(q);
            }
            continue;
        }

        if (!std::holds_alternative<GateOp>(op)) continue;

        const GateOp& g = std::get<GateOp>(op);

        // Multi-qubit gate: flush windows for all its qubits.
        if (g.qubits.size() > 1) {
            for (QubitIndex q : g.qubits) {
                if (q < nq && !windows[q].indices.empty()) try_flush_window(q);
            }
            continue;
        }

        if (g.qubits.empty()) continue;
        QubitIndex q = g.qubits[0];
        if (q >= nq) continue;

        if (!is_eligible_1q(g)) {
            // Rotation or opaque: flush this qubit's window.
            if (!windows[q].indices.empty()) try_flush_window(q);
            continue;
        }

        // Accumulate in the window.
        windows[q].indices.push_back(i);
        windows[q].types.push_back(g.type);

        // Eagerly try to apply patterns after each addition.
        // This handles the case where we accumulate H,X,H without waiting
        // for a boundary.
        if (windows[q].types.size() >= 3) {
            auto n = windows[q].types.size();
            auto m = match_pattern_3(windows[q].types[n-3],
                                     windows[q].types[n-2],
                                     windows[q].types[n-1]);
            if (m.has_value()) {
                Replacement r;
                r.surviving_idx = windows[q].indices[n-3];
                r.drop = {windows[q].indices[n-2], windows[q].indices[n-1]};
                r.merged_type   = *m;
                r.qubit         = q;
                replacements.push_back(r);
                // Remove the last 3 from window (consumed by pattern).
                for (int k = 0; k < 3; ++k) {
                    windows[q].indices.pop_back();
                    windows[q].types.pop_back();
                }
            }
        } else if (windows[q].types.size() >= 2) {
            auto n = windows[q].types.size();
            auto m = match_pattern_2(windows[q].types[n-2], windows[q].types[n-1]);
            if (m.has_value()) {
                Replacement r;
                r.surviving_idx = windows[q].indices[n-2];
                r.drop = {windows[q].indices[n-1]};
                r.merged_type   = *m;
                r.qubit         = q;
                replacements.push_back(r);
                for (int k = 0; k < 2; ++k) {
                    windows[q].indices.pop_back();
                    windows[q].types.pop_back();
                }
            }
        }
    }

    // Flush remaining windows at end of circuit.
    for (QubitIndex q = 0; q < nq; ++q) {
        if (!windows[q].indices.empty()) try_flush_window(q);
    }

    if (replacements.empty()) {
        // No merges found — return circuit as-is (changed_ stays false).
        return circuit;
    }

    changed_ = true;

    // Build a set of indices to drop and a map from surviving index to new type.
    std::unordered_map<std::size_t, GateType> update_type;
    std::unordered_map<std::size_t, bool>     drop_set;
    for (const auto& r : replacements) {
        update_type[r.surviving_idx] = r.merged_type;
        for (std::size_t d : r.drop) drop_set[d] = true;
    }

    // Apply mutations.
    std::vector<Op> new_ops;
    new_ops.reserve(ops.size());
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (drop_set.count(i)) continue;  // dropped operand of merge
        if (update_type.count(i)) {
            // Replace the surviving op with the merged gate.
            GateOp merged = std::get<GateOp>(ops[i]);
            merged.type   = update_type[i];
            merged.name   = gate_name(update_type[i]);
            merged.params.clear();
            new_ops.push_back(Op{merged});
        } else {
            new_ops.push_back(ops[i]);
        }
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool MergeSingleQubitPass::changed() const {
    return changed_;
}

}  // namespace qplayer
