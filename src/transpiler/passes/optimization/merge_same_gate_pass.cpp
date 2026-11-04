/// @file merge_same_gate_pass.cpp
/// @brief MergeSameGatePass (Phase 4).
///
/// Merges consecutive same-kind 1Q GateOps on the same qubit:
///
/// Self-inverse (result = identity, op removed by RemoveIdentityPass later):
/// X·X → I,  Y·Y → I,  Z·Z → I,  H·H → I
///
/// Fixed-phase table:
/// S·S   → Z
/// Sdg·Sdg → Z
/// T·T   → S
/// Tdg·Tdg → Sdg
///
/// Rotation accumulation:
/// RZ(θ1)·RZ(θ2) → RZ(θ1+θ2)
/// U1(λ1)·U1(λ2) → U1(λ1+λ2)
///
/// Mixed-kind pairs (H·X, S·T, …) are left unchanged by this pass.
/// Boundary ops (Measure, Barrier, If, Idle) and multi-qubit GateOps terminate
/// the per-qubit merge window. Opaque GateOps pass through unchanged.
///
/// Algorithm: per-qubit pending slot stores the most-recently-seen eligible 1Q
/// gate (index + type + params). When the next eligible 1Q gate arrives on the
/// same qubit and is of the same type, merge and record a replacement; otherwise
/// flush the pending slot and start a new one.
///
/// Level 2/3 only — not auto-registered at Level 0/1.
///

#include "transpiler/passes/optimization/merge_same_gate_pass.hpp"

#include <cmath>
#include <optional>
#include <unordered_map>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

constexpr double kTol = 1e-10;  // angle equality tolerance

// Returns true when the op is a merge-window boundary.
bool is_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

// Returns true when the 1Q GateOp is eligible for same-kind merging.
// Excludes: opaque, multi-qubit, U2/U3 (non-trivial multi-param rotation).
bool is_eligible(const GateOp& g) {
    if (g.is_opaque) return false;
    if (g.qubits.size() != 1) return false;
    // U2 and U3 have multiple parameters and mixed-angle merging is out of scope.
    if (g.type == GateType::U2 || g.type == GateType::U3) return false;
    return true;
}

// Given two consecutive identical-type eligible-1Q gates, returns the merged
// result. `MergedGate::is_identity` means the pair collapses to identity.
struct MergedGate {
    GateType           type;
    std::vector<double> params;
    const char*        name_str;
    bool               is_identity = false;
};

std::optional<MergedGate> try_merge_same(GateType t,
                                          const std::vector<double>& p1,
                                          const std::vector<double>& p2) {
    switch (t) {
        // Self-inverse: pair → identity.
        case GateType::X:
        case GateType::Y:
        case GateType::Z:
        case GateType::H:
            return MergedGate{GateType::I, {}, "id", true};

        // Fixed-phase table.
        case GateType::S:
            return MergedGate{GateType::Z,   {}, "z",  false};
        case GateType::Sdg:
            return MergedGate{GateType::Z,   {}, "z",  false};
        case GateType::T:
            return MergedGate{GateType::S,   {}, "s",  false};
        case GateType::Tdg:
            return MergedGate{GateType::Sdg, {}, "sdg", false};

        // SX*SX = X, SXdg*SXdg = X.
        case GateType::SX:
        case GateType::SXdg:
            return MergedGate{GateType::X,   {}, "x",  false};

        // Rotation accumulation: sum the single angle parameter.
        case GateType::RZ:
        case GateType::U1:
            if (p1.size() == 1 && p2.size() == 1) {
                double sum = p1[0] + p2[0];
                const char* nm = (t == GateType::RZ) ? "rz" : "u1";
                return MergedGate{t, {sum}, nm, false};
            }
            return std::nullopt;

        // RX and RY: accumulate angle as well (same kind only).
        case GateType::RX:
            if (p1.size() == 1 && p2.size() == 1)
                return MergedGate{GateType::RX, {p1[0] + p2[0]}, "rx", false};
            return std::nullopt;
        case GateType::RY:
            if (p1.size() == 1 && p2.size() == 1)
                return MergedGate{GateType::RY, {p1[0] + p2[0]}, "ry", false};
            return std::nullopt;

        default:
            return std::nullopt;
    }
}

// Per-qubit pending slot: tracks the last eligible 1Q gate seen.
struct Pending {
    std::size_t         idx   = 0;
    GateType            type  = GateType::I;
    std::vector<double> params;
    bool                valid = false;

    void set(std::size_t i, GateType t, const std::vector<double>& p) {
        idx = i; type = t; params = p; valid = true;
    }
    void clear() { valid = false; }
};

}  // anonymous namespace

std::string MergeSameGatePass::name() const {
    return "MergeSameGatePass";
}

// Algorithm:
// 1. Scan ops; maintain per-qubit Pending slot.
// 2. When a new eligible gate arrives on qubit q:
//    a. If pending[q] is valid AND same type → try_merge_same().
//       On success: record replacement (surviving = pending.idx, drop = current i).
//       Update pending[q] with the merged result (for chaining: T·T·T·T).
//    b. Otherwise: flush old pending[q] (no-op since it didn't merge), set new pending.
// 3. On boundary or multi-qubit gate: clear pending slots for affected qubits.
// 4. Reconstruct the circuit using the drop/update sets.
//
// Note on chaining: after T·T → S, the surviving slot holds S; a subsequent T
// will not match (S≠T) so it correctly starts a new pending slot. This is safe
// because chaining same-kind triples (T·T·T = S·T, no further same-kind merge)
// naturally terminates.
Circuit MergeSameGatePass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    const QubitIndex nq = circuit.num_qubits();

    std::vector<Pending> pend(static_cast<std::size_t>(nq));

    struct Replacement {
        std::size_t         surviving_idx;
        std::size_t         drop_idx;
        GateType            merged_type;
        std::vector<double> merged_params;
        const char*         merged_name;
        bool                is_identity;
    };
    std::vector<Replacement> reps;

    // Flush pending[q]: no-op (pending slot is simply discarded if not merged).
    auto clear_pending = [&](QubitIndex q) {
        pend[q].clear();
    };

    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        const Op& op = old_ops[i];

        if (is_boundary(op)) {
            for (QubitIndex q = 0; q < nq; ++q) clear_pending(q);
            continue;
        }

        if (!std::holds_alternative<GateOp>(op)) continue;

        const GateOp& g = std::get<GateOp>(op);

        // Multi-qubit gate: clear pending for all its qubits.
        if (g.qubits.size() > 1) {
            for (QubitIndex q : g.qubits) {
                if (q < nq) clear_pending(q);
            }
            continue;
        }

        if (g.qubits.empty()) continue;
        QubitIndex q = g.qubits[0];
        if (q >= nq) continue;

        if (!is_eligible(g)) {
            clear_pending(q);
            continue;
        }

        if (pend[q].valid && pend[q].type == g.type) {
            // Same kind: attempt merge.
            auto m = try_merge_same(g.type, pend[q].params, g.params);
            if (m.has_value()) {
                Replacement r;
                r.surviving_idx  = pend[q].idx;
                r.drop_idx       = i;
                r.merged_type    = m->type;
                r.merged_params  = m->params;
                r.merged_name    = m->name_str;
                r.is_identity    = m->is_identity;
                reps.push_back(r);

                // Update the surviving slot to reflect the merged gate so
                // subsequent same-kind gates chain correctly.
                if (m->is_identity) {
                    clear_pending(q);
                } else {
                    pend[q].set(r.surviving_idx, m->type, m->params);
                }
                continue;
            }
        }

        // Different kind or no pending: start a new pending slot.
        clear_pending(q);
        pend[q].set(i, g.type, g.params);
    }

    if (reps.empty()) {
        return circuit;
    }

    changed_ = true;

    // Build drop set and update map.
    std::unordered_map<std::size_t, bool>        drop_set;
    std::unordered_map<std::size_t, std::size_t> rep_idx;  // surviving_idx → reps index

    for (std::size_t ri = 0; ri < reps.size(); ++ri) {
        drop_set[reps[ri].drop_idx] = true;
        if (reps[ri].is_identity) {
            drop_set[reps[ri].surviving_idx] = true;
        } else {
            rep_idx[reps[ri].surviving_idx] = ri;
        }
    }

    std::vector<Op> new_ops;
    new_ops.reserve(old_ops.size());

    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        if (drop_set.count(i)) continue;

        if (rep_idx.count(i)) {
            const Replacement& r = reps[rep_idx[i]];
            GateOp merged = std::get<GateOp>(old_ops[i]);
            merged.type   = r.merged_type;
            merged.name   = r.merged_name;
            merged.params = r.merged_params;
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

bool MergeSameGatePass::changed() const {
    return changed_;
}

}  // namespace qplayer
