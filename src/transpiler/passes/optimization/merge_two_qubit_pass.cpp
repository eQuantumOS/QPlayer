// merge_two_qubit_pass.cpp — MergeTwoQubitPass (Phase 4).
// Merges adjacent same-kind 2Q GateOp pairs that act on the identical ordered
// qubit pair. Qubit-pair order is significant — CX(0,1) and CX(1,0) are
// treated as distinct gates and are NOT merged.
//
// Merge rules:
//   Self-inverse (result = identity, op removed):
//     CX(q0,q1)·CX(q0,q1)       → I
//     CY·CY, CZ·CZ, CH·CH       → I  (same qubit pair)
//     SWAP(q0,q1)·SWAP(q0,q1)   → I
//     CCX(c0,c1,t)·CCX(c0,c1,t) → I  (all 3 qubits match in order)
//     CSwap(c,t0,t1)·CSwap(c,t0,t1) → I
//
//   Rotation accumulation:
//     CRZ(θ1,q0,q1)·CRZ(θ2,q0,q1) → CRZ(θ1+θ2, q0, q1)
//     CU1(λ1,q0,q1)·CU1(λ2,q0,q1) → CU1(λ1+λ2, q0, q1)
//
// Window logic: a pending slot is maintained per (gate_type, qubit_vector) key.
// When the next 2Q gate arrives with matching type AND identical qubit vector,
// the pair is merged. Any intervening op that touches a qubit in the pair
// invalidates the pending slot for that pair.
//
// A 1Q GateOp that does NOT touch either qubit in the pair does NOT invalidate
// the pending slot (it commutes past the 2Q gate for merge purposes). This is
// a sound approximation because the 2Q gate and its copy are still logically
// adjacent on both qubits' timelines.
//
// Level 2/3 only — not auto-registered at Level 0/1.
//

#include "transpiler/passes/optimization/merge_two_qubit_pass.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

// Returns true when op is a hard boundary (terminates all pending windows).
bool is_hard_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

// Returns true when the 2Q gate type is self-inverse.
bool is_self_inverse_2q(GateType t) {
    switch (t) {
        case GateType::CX:
        case GateType::CY:
        case GateType::CZ:
        case GateType::CH:
        case GateType::SWAP:
        case GateType::CCX:
        case GateType::CSwap:
            return true;
        default:
            return false;
    }
}

// Returns true when the 2Q gate accumulates a single rotation angle.
bool is_rotation_2q(GateType t) {
    return t == GateType::CRZ || t == GateType::CU1;
}

// Canonical gate-name string for 2Q gates.
const char* gate_name_str_2q(GateType t) {
    switch (t) {
        case GateType::CRZ:  return "crz";
        case GateType::CU1:  return "cu1";
        default:             return "unknown";
    }
}

// A stable key identifying a (gate_type, qubit_vector) pair. Used as the
// map key for pending 2Q gate slots. std::map provides ordering without
// needing a custom hash.
struct TwoQKey {
    GateType                type;
    std::vector<QubitIndex> qubits;  // ordered as in GateOp.qubits

    bool operator<(const TwoQKey& o) const {
        if (type != o.type) return static_cast<int>(type) < static_cast<int>(o.type);
        return qubits < o.qubits;
    }
    bool operator==(const TwoQKey& o) const {
        return type == o.type && qubits == o.qubits;
    }
};

// Pending slot for a 2Q gate — awaiting a matching partner.
struct PendingSlot {
    std::size_t         idx;    // position in old_ops
    std::vector<double> params; // gate parameters (for rotation accumulation)
};

}  // anonymous namespace

std::string MergeTwoQubitPass::name() const {
    return "MergeTwoQubitPass";
}

// Algorithm:
// Maintain a map<TwoQKey, PendingSlot> of currently open 2Q pending slots.
// For each op:
//   - Hard boundary: clear all pending slots.
//   - 1Q GateOp: invalidate any pending slot whose qubit set intersects this gate.
//   - 2Q GateOp (non-opaque, >= 2 qubits):
//       * Build key = (type, qubits).
//       * If a matching pending slot exists → record a Replacement, remove the slot.
//       * Else → open a new pending slot.
//   - All other ops pass through.
// After scan: reconstruct circuit dropping/updating recorded positions.
Circuit MergeTwoQubitPass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();

    // Map from TwoQKey to open PendingSlot.
    std::map<TwoQKey, PendingSlot> pending;

    struct Replacement {
        std::size_t         surviving_idx;
        std::size_t         drop_idx;
        GateType            result_type;
        std::vector<double> result_params;
        const char*         result_name;
        bool                is_identity;
    };
    std::vector<Replacement> reps;

    // Invalidate any pending slot that involves any of the given qubits.
    auto invalidate_for_qubits = [&](const std::vector<QubitIndex>& qs) {
        for (auto it = pending.begin(); it != pending.end(); ) {
            bool conflict = false;
            for (QubitIndex q : it->first.qubits) {
                for (QubitIndex pq : qs) {
                    if (q == pq) { conflict = true; break; }
                }
                if (conflict) break;
            }
            if (conflict) it = pending.erase(it);
            else ++it;
        }
    };

    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        const Op& op = old_ops[i];

        if (is_hard_boundary(op)) {
            pending.clear();
            continue;
        }

        if (!std::holds_alternative<GateOp>(op)) continue;

        const GateOp& g = std::get<GateOp>(op);

        if (g.is_opaque) {
            // Opaque: invalidate affected qubits and pass through.
            invalidate_for_qubits(g.qubits);
            continue;
        }

        if (g.qubits.size() == 1) {
            // 1Q gate: invalidate any pending 2Q slot touching this qubit.
            invalidate_for_qubits(g.qubits);
            continue;
        }

        if (g.qubits.size() < 2) continue;  // 0-qubit gate (unusual) — skip

        // 2Q (or 3Q) gate: check for a pending match.
        TwoQKey key{g.type, g.qubits};
        auto it = pending.find(key);

        if (it != pending.end()) {
            // Matching pair found — merge.
            Replacement r;
            r.surviving_idx = it->second.idx;
            r.drop_idx      = i;

            if (is_self_inverse_2q(g.type)) {
                r.result_type   = GateType::I;
                r.result_params = {};
                r.result_name   = "id";
                r.is_identity   = true;
            } else if (is_rotation_2q(g.type) &&
                       it->second.params.size() == 1 &&
                       g.params.size() == 1) {
                double sum = it->second.params[0] + g.params[0];
                r.result_type   = g.type;
                r.result_params = {sum};
                r.result_name   = gate_name_str_2q(g.type);
                r.is_identity   = false;
            } else {
                // Unknown merge — do not merge (treat as no match).
                // Close the old pending slot and open a new one.
                pending.erase(it);
                pending[key] = PendingSlot{i, g.params};
                continue;
            }

            reps.push_back(r);
            pending.erase(it);
            // After cancellation the surviving slot is consumed; no new pending.
        } else {
            // No matching pending slot: open one.
            pending[key] = PendingSlot{i, g.params};
        }
    }

    if (reps.empty()) {
        return circuit;
    }

    changed_ = true;

    // Build drop set and update map.
    std::unordered_map<std::size_t, bool>        drop_set;
    std::unordered_map<std::size_t, std::size_t> upd_map;  // idx → reps index

    for (std::size_t ri = 0; ri < reps.size(); ++ri) {
        drop_set[reps[ri].drop_idx] = true;
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
            merged.name   = r.result_name;
            merged.params = r.result_params;
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

bool MergeTwoQubitPass::changed() const {
    return changed_;
}

}  // namespace qplayer
