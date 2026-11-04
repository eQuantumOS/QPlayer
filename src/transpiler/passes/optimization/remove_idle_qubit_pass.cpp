/// @file remove_idle_qubit_pass.cpp
/// @brief RemoveIdleQubitPass (Phase 4).
///
/// Identifies "idle" qubits — those that are never *observed* AND have no
/// entanglement dependency with an observed qubit — and removes all GateOps that
/// act solely on those idle qubits.
///
/// The circuit's num_qubits is NOT changed (qreg declarations stay constant).
/// Only GateOp nodes that touch exclusively idle qubits are removed, so the gain
/// is in gate applications, not in state-space memory (that is 's job).
///
/// Observation set ( — the pass used to treat MeasureOp as the *only*
/// observation, which silently destroyed measure-less circuits whose amplitudes
/// or expectation values the caller reads back afterwards):
/// MeasureOp.qubit  ∪  IfOp body qubits  ∪  Circuit::expectation_targets()
/// A statevector read (get_statevector / expectation_value) happens *after*
/// run(), so that intent cannot be detected statically — expectation_targets()
/// is the only observation declaration the circuit itself carries. Therefore a
/// circuit with no observation declaration at all is left untouched.
///
/// Dependency analysis:
/// Pass 1 — collect the observation set (measure + IfOp body + Pauli targets).
/// Empty set => skip the whole pass (conservative).
/// Pass 2 — expand kept set to a fixed point: every multi-qubit GateOp that
/// intersects the current kept set contributes all of its qubits.
///
/// Conservative ops (always emitted):
/// BarrierOp, IdleOp, MeasureOp — structural; removing them would change
/// observable semantics or upset downstream passes.
/// IfOp — body analysis is complex; passed through unchanged.
///
/// A single-qubit GateOp is removed iff its sole qubit is not in `kept`.
/// A multi-qubit GateOp is removed iff ALL of its qubits are not in `kept`.
/// (If any qubit is kept, the gate is preserved to avoid breaking entanglement.)
///
/// Level 2/3 only — not auto-registered at Level 0/1.
///

#include "transpiler/passes/optimization/remove_idle_qubit_pass.hpp"

#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "log.hpp"

namespace qplayer {

std::string RemoveIdleQubitPass::name() const {
    return "RemoveIdleQubitPass";
}

Circuit RemoveIdleQubitPass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    const QubitIndex nq = circuit.num_qubits();

    // Pass 1: collect the observation set.
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

    // Pre-registered Pauli expectation targets are observations too
    // Qubit order is Qiskit big-endian: pauli[0] acts on the
    // highest qubit, matching Backend::expectation_value(). Non-'I' positions
    // name the observed qubits; a shorter string is right-aligned the same way.
    for (const std::string& pauli : circuit.expectation_targets()) {
        const std::size_t len = pauli.size();
        for (std::size_t k = 0; k < len; ++k) {
            // Only 'I' is identity; the validated alphabet is I/X/Y/Z (any other
            // char is rejected later by expectation_value), so treating unknown
            // chars as observations keeps this pass on the conservative side.
            if (pauli[k] == 'I') continue;
            // len may exceed nq on a malformed target; SemanticValidatorPass
            // owns that diagnosis, so here we only skip out-of-range indices.
            if (len - 1 - k >= static_cast<std::size_t>(nq)) continue;
            kept.insert(static_cast<QubitIndex>(len - 1 - k));
        }
    }

    // No observation declared anywhere: the caller can still read amplitudes or
    // expectation values after run(), and that intent is invisible here, so
    // dropping gates would silently change the result.
    if (kept.empty()) {
        return circuit;
    }

    // Pass 2: fixed-point expansion — any multi-qubit GateOp that touches a kept
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
    std::size_t             dropped = 0;
    std::set<QubitIndex>    dropped_qubits;  // ordered: log text must be deterministic

    for (const Op& op : old_ops) {
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            // Opaque GateOps stay untouched — silent removal would violate the
            // "no silent transformation of opaque gates" rule (-2).
            // Every other optimization pass enforces the same guard.
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
                ++dropped;
                for (QubitIndex q : g.qubits) dropped_qubits.insert(q);
                continue;
            }
        }
        new_ops.push_back(op);
    }

    if (!changed_) {
        return circuit;
    }

    // Removal is invisible in counts (the qubits are unobserved) but it does
    // change what get_statevector() reports — those qubits stay |0>. Announce it
    // so the transformation is never silent.
    std::string qubit_list;
    for (QubitIndex q : dropped_qubits) {
        if (!qubit_list.empty()) qubit_list += ", ";
        qubit_list += std::to_string(q);
    }
    log::warn("RemoveIdleQubitPass: dropped " + std::to_string(dropped) +
              " gate(s) on unobserved qubits {" + qubit_list +
              "}; those qubits remain |0> in get_statevector()");

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
