// pattern_replace_pass.cpp — PatternReplacePass (Phase 4).
// Applies a fixed table of single-qubit gate-sequence patterns to the circuit.
// Patterns are matched on consecutive same-qubit eligible 1Q GateOps using a
// per-qubit sliding window of up to 4 ops. Global phase is ignored throughout.
//
// Pattern table (same-qubit, adjacent — no intervening gates on that qubit):
//   4-gate:  T·T·T·T → Z
//   3-gate:  H·X·H   → Z
//            H·Z·H   → X
//   2-gate:  T·T     → S
//            S·S     → Z
//            Tdg·Tdg → Sdg
//            Sdg·Sdg → Z
//            H·H     → I  (identity — op removed)
//            X·X     → I  (identity — op removed)
//
// When the window reaches length 4, 3-length and 4-length patterns are tried
// first. When a match is found the consumed ops are removed and the window is
// reset (conservative: earlier pending ops are re-emitted as-is). A match also
// sets changed_ = true.
//
// Level 2/3 only — not auto-registered at Level 0/1.
//

#include "transpiler/passes/optimization/pattern_replace_pass.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

// Returns true when op is a merge-window boundary.
bool is_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

// Returns true when the GateOp is eligible for pattern matching:
// non-opaque, 1-qubit, non-rotation.
bool is_eligible(const GateOp& g) {
    if (g.is_opaque) return false;
    if (g.qubits.size() != 1) return false;
    switch (g.type) {
        case GateType::RX: case GateType::RY: case GateType::RZ:
        case GateType::U1: case GateType::U2: case GateType::U3:
            return false;
        default:
            return true;
    }
}

// Canonical gate-name string (used when writing back the merged op).
const char* gate_name_str(GateType t) {
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

// Result of a pattern match.
struct PatternMatch {
    std::size_t          pattern_len;   // number of ops consumed
    GateType             result_type;   // replacement gate type
    bool                 is_identity;   // true when result is I (ops removed)
};

// Try to match a 4-gate pattern starting at window positions [0..3].
std::optional<PatternMatch> try_match_4(GateType a, GateType b, GateType c, GateType d) {
    // T·T·T·T → Z
    if (a == GateType::T && b == GateType::T &&
        c == GateType::T && d == GateType::T)
        return PatternMatch{4, GateType::Z, false};
    return std::nullopt;
}

// Try to match a 3-gate pattern.
std::optional<PatternMatch> try_match_3(GateType a, GateType b, GateType c) {
    // H·X·H → Z
    if (a == GateType::H && b == GateType::X && c == GateType::H)
        return PatternMatch{3, GateType::Z, false};
    // H·Z·H → X
    if (a == GateType::H && b == GateType::Z && c == GateType::H)
        return PatternMatch{3, GateType::X, false};
    return std::nullopt;
}

// Try to match a 2-gate pattern.
std::optional<PatternMatch> try_match_2(GateType a, GateType b) {
    // T·T → S
    if (a == GateType::T   && b == GateType::T)   return PatternMatch{2, GateType::S,   false};
    // S·S → Z
    if (a == GateType::S   && b == GateType::S)   return PatternMatch{2, GateType::Z,   false};
    // Tdg·Tdg → Sdg
    if (a == GateType::Tdg && b == GateType::Tdg) return PatternMatch{2, GateType::Sdg, false};
    // Sdg·Sdg → Z
    if (a == GateType::Sdg && b == GateType::Sdg) return PatternMatch{2, GateType::Z,   false};
    // H·H → I (identity)
    if (a == GateType::H   && b == GateType::H)   return PatternMatch{2, GateType::I,   true};
    // X·X → I (identity)
    if (a == GateType::X   && b == GateType::X)   return PatternMatch{2, GateType::I,   true};
    return std::nullopt;
}

// Per-qubit sliding window: holds up to 4 consecutive eligible op positions.
struct Window {
    std::vector<std::size_t> indices;  // positions in the original ops vector
    std::vector<GateType>    types;

    void push(std::size_t i, GateType t) { indices.push_back(i); types.push_back(t); }
    void reset() { indices.clear(); types.clear(); }
    std::size_t size() const { return types.size(); }
};

}  // anonymous namespace

std::string PatternReplacePass::name() const {
    return "PatternReplacePass";
}

// Algorithm:
// 1. For each qubit maintain a Window of consecutive eligible-1Q op indices.
// 2. After each push, eagerly try to match patterns from longest to shortest:
//    4-gate, then 3-gate, then 2-gate.
//    On a match: record a Replacement (surviving + drop list + result gate),
//    consume the matched ops from the window, reset the rest.
// 3. On boundary or multi-qubit gate: flush the window (no match = no-op).
// 4. Apply all replacements in a single reconstruction pass.
Circuit PatternReplacePass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    const QubitIndex nq = circuit.num_qubits();

    std::vector<Window> wins(static_cast<std::size_t>(nq));

    struct Replacement {
        std::size_t              surviving_idx;
        std::vector<std::size_t> drop_indices;  // may be empty for identity case
        GateType                 result_type;
        bool                     is_identity;
    };
    std::vector<Replacement> reps;

    // Attempt pattern matches on the back of window w; record any match.
    auto try_match_window = [&](QubitIndex q) {
        Window& w = wins[q];
        std::size_t n = w.size();

        // Try 4-gate match.
        if (n >= 4) {
            auto m = try_match_4(w.types[n-4], w.types[n-3], w.types[n-2], w.types[n-1]);
            if (m.has_value()) {
                std::size_t base = n - 4;
                Replacement r;
                r.surviving_idx = w.indices[base];
                for (std::size_t k = base + 1; k < n; ++k)
                    r.drop_indices.push_back(w.indices[k]);
                r.result_type  = m->result_type;
                r.is_identity  = m->is_identity;
                reps.push_back(r);
                // Consume matched ops from window tail.
                w.indices.resize(base);
                w.types.resize(base);
                return;
            }
        }

        // Try 3-gate match.
        if (n >= 3) {
            auto m = try_match_3(w.types[n-3], w.types[n-2], w.types[n-1]);
            if (m.has_value()) {
                std::size_t base = n - 3;
                Replacement r;
                r.surviving_idx = w.indices[base];
                for (std::size_t k = base + 1; k < n; ++k)
                    r.drop_indices.push_back(w.indices[k]);
                r.result_type  = m->result_type;
                r.is_identity  = m->is_identity;
                reps.push_back(r);
                w.indices.resize(base);
                w.types.resize(base);
                return;
            }
        }

        // Try 2-gate match.
        if (n >= 2) {
            auto m = try_match_2(w.types[n-2], w.types[n-1]);
            if (m.has_value()) {
                std::size_t base = n - 2;
                Replacement r;
                r.surviving_idx = w.indices[base];
                r.drop_indices.push_back(w.indices[n-1]);
                r.result_type  = m->result_type;
                r.is_identity  = m->is_identity;
                reps.push_back(r);
                w.indices.resize(base);
                w.types.resize(base);
                return;
            }
        }
    };

    // flush_window: try to match any pending ops before discarding the window.
    // This handles the case where a boundary or end-of-circuit terminates a
    // deferred window (e.g. T·T held waiting for a 4th T that never arrives).
    auto flush_window = [&](QubitIndex q) {
        if (!wins[q].indices.empty()) {
            try_match_window(q);
        }
        wins[q].reset();
    };

    // Single forward pass to collect replacements.
    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        const Op& op = old_ops[i];

        if (is_boundary(op)) {
            for (QubitIndex q = 0; q < nq; ++q) flush_window(q);
            continue;
        }

        if (!std::holds_alternative<GateOp>(op)) continue;

        const GateOp& g = std::get<GateOp>(op);

        if (g.qubits.size() > 1) {
            for (QubitIndex q : g.qubits) {
                if (q < nq) flush_window(q);
            }
            continue;
        }

        if (g.qubits.empty()) continue;
        QubitIndex q = g.qubits[0];
        if (q >= nq) continue;

        if (!is_eligible(g)) {
            flush_window(q);
            continue;
        }

        wins[q].push(i, g.type);
        // Eagerly try matching after each push, but only commit to a shorter
        // pattern when the window cannot grow to accommodate a longer one.
        // Strategy: always try the longest available pattern first (4 > 3 > 2).
        // A 2-gate match is applied immediately when the window has exactly 2
        // entries OR when no 4-gate / 3-gate pattern can ever extend it (i.e.
        // the current top-2 types do not appear as a prefix of any 4/3-gate
        // pattern). For simplicity — and to ensure T·T·T·T fires before T·T —
        // we delay 2-gate matches until the window size reaches 4 or a boundary
        // is hit. 3-gate matches are applied at window size >= 3 only when the
        // current top-3 does not extend to a known 4-gate prefix.
        if (wins[q].size() >= 4) {
            try_match_window(q);
        } else if (wins[q].size() == 3) {
            // Try 4-gate prefix check: if top-3 cannot lead to T·T·T·T, apply 3.
            // The only 4-gate pattern is T·T·T·T. If the last 3 are not T·T·T
            // then no 4-gate match is possible starting within this window, so
            // it is safe to try 3-gate / 2-gate patterns now.
            auto n = wins[q].size();
            bool all_t = (wins[q].types[n-3] == GateType::T &&
                          wins[q].types[n-2] == GateType::T &&
                          wins[q].types[n-1] == GateType::T);
            if (!all_t) {
                try_match_window(q);
            }
            // If all 3 are T, wait for the 4th to arrive.
        } else if (wins[q].size() == 2) {
            // At size 2: only apply 2-gate pattern if it cannot grow into a
            // useful 3-gate or 4-gate match. Check whether the current pair is
            // a 3-gate prefix: H·X, H·Z, or a 4-gate prefix: T·T (might extend).
            auto n = wins[q].size();
            GateType a = wins[q].types[n-2];
            GateType b = wins[q].types[n-1];
            bool is_3gate_prefix = (a == GateType::H &&
                                    (b == GateType::X || b == GateType::Z));
            bool is_4gate_prefix = (a == GateType::T && b == GateType::T);
            if (!is_3gate_prefix && !is_4gate_prefix) {
                try_match_window(q);
            }
            // Otherwise hold off and let more gates arrive.
        }
    }

    // Flush any windows still open at end-of-circuit (no trailing boundary).
    for (QubitIndex q = 0; q < nq; ++q) flush_window(q);

    if (reps.empty()) {
        return circuit;
    }

    changed_ = true;

    // Build drop set and update map (surviving_idx → replacement index).
    std::unordered_map<std::size_t, bool>        drop_set;
    std::unordered_map<std::size_t, std::size_t> upd_map;

    for (std::size_t ri = 0; ri < reps.size(); ++ri) {
        for (std::size_t d : reps[ri].drop_indices) drop_set[d] = true;
        if (reps[ri].is_identity) {
            drop_set[reps[ri].surviving_idx] = true;
        } else {
            upd_map[reps[ri].surviving_idx] = ri;
        }
    }

    std::vector<Op> new_ops;
    new_ops.reserve(old_ops.size());

    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        if (drop_set.count(i)) continue;

        if (upd_map.count(i)) {
            const Replacement& r = reps[upd_map[i]];
            GateOp merged = std::get<GateOp>(old_ops[i]);
            merged.type   = r.result_type;
            merged.name   = gate_name_str(r.result_type);
            merged.params.clear();
            new_ops.push_back(Op{merged});
        } else {
            new_ops.push_back(old_ops[i]);
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

bool PatternReplacePass::changed() const {
    return changed_;
}

}  // namespace qplayer
