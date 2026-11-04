// remove_idle_qubit_pass.cpp — RemoveIdleQubitPass (Phase 4).
// Identifies "idle" qubits — those that are never measured AND have no
// entanglement dependency with a measured qubit — and removes all GateOps that
// act solely on those idle qubits.
//
// The circuit's num_qubits is NOT changed (qreg declarations stay constant).
// Only GateOp nodes that touch exclusively idle qubits are removed.
//
// Dependency analysis:
//   Pass 1 — collect measure_qubits from all MeasureOps (direct targets).
//   Pass 2 — expand kept set: for every multi-qubit GateOp that intersects
//             the current kept set, add all its qubits to kept.
//   (Single round of expansion — 1-step closure. Deeper transitive closure
// is a Phase 4+ enhancement; tracked as simplification in notes.)
//
// Conservative ops (always emitted):
//   BarrierOp, IdleOp, MeasureOp — structural; removing them would change
//   observable semantics or upset downstream passes.
//   IfOp — body analysis is complex; passed through unchanged.
//
// A single-qubit GateOp is removed iff its sole qubit is not in `kept`.
// A multi-qubit GateOp is removed iff ALL of its qubits are not in `kept`.
// (If any qubit is kept, the gate is preserved to avoid breaking entanglement.)
//
// Level 2/3 only — not auto-registered at Level 0/1.
//

#include "transpiler/passes/optimization/remove_idle_qubit_pass.hpp"

#include <unordered_set>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

std::string RemoveIdleQubitPass::name() const {
    return "RemoveIdleQubitPass";
}

Circuit RemoveIdleQubitPass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    const QubitIndex nq = circuit.num_qubits();

    // Pass 1: collect directly-measured qubits.
    std::unordered_set<QubitIndex> kept;

    for (const Op& op : old_ops) {
        if (std::holds_alternative<MeasureOp>(op)) {
            kept.insert(std::get<MeasureOp>(op).qubit);
        }
        // IfOp body may implicitly "use" a qubit but analysis is deferred;
        // treat IfOp body qubits as kept to be conservative.
        if (std::holds_alternative<IfOp>(op)) {
            const GateOp& body = std::get<IfOp>(op).body;
            for (QubitIndex q : body.qubits) kept.insert(q);
        }
    }

    // Pass 2: 1-step expansion — any multi-qubit GateOp that touches a kept
    // qubit brings all its qubits into kept.
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const Op& op : old_ops) {
            if (!std::holds_alternative<GateOp>(op)) continue;
            const GateOp& g = std::get<GateOp>(op);
            if (g.qubits.size() <= 1) continue;  // only multi-qubit gates create dependencies
            // Check if any qubit is already kept.
            bool touches_kept = false;
            for (QubitIndex q : g.qubits) {
                if (kept.count(q)) { touches_kept = true; break; }
            }
            if (touches_kept) {
                for (QubitIndex q : g.qubits) {
                    if (!kept.count(q)) {
                        kept.insert(q);
                        expanded = true;
                    }
                }
            }
        }
    }

    // Pass 3: remove GateOps that touch only idle (non-kept) qubits.
    std::vector<Op> new_ops;
    new_ops.reserve(old_ops.size());

    for (const Op& op : old_ops) {
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            // Opaque GateOps stay untouched — silent removal would violate the
            // "no silent transformation of opaque gates" rule (-2).
            // Every other pass enforces the same guard.
            if (g.is_opaque) {
                new_ops.push_back(op);
                continue;
            }
            // Check whether any qubit is kept.
            bool any_kept = false;
            for (QubitIndex q : g.qubits) {
                if (q >= nq) continue;
                if (kept.count(q)) { any_kept = true; break; }
            }
            if (!any_kept) {
                // All qubits are idle — remove this gate.
                changed_ = true;
                continue;
            }
        }
        new_ops.push_back(op);
    }

    if (!changed_) {
        return circuit;
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool RemoveIdleQubitPass::changed() const {
    return changed_;
}

}  // namespace qplayer
