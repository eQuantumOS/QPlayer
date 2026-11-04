/// @file remove_identity_pass.cpp
/// @brief RemoveIdentityPass (Phase 4).
///
/// Filters out GateType::I (identity) GateOps from the circuit. The pass is a
/// simple linear scan — no windowing or accumulation required. Opaque GateOps
/// (is_opaque == true) are preserved even when their type field happens to be
/// GateType::I, because opaque semantics are user-defined.
///
/// Level 2/3 only — not auto-registered at Level 0/1.
///

#include "transpiler/passes/optimization/remove_identity_pass.hpp"

#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

std::string RemoveIdentityPass::name() const {
    return "RemoveIdentityPass";
}

// One linear scan: copy every op that is NOT a plain identity GateOp into a
// new vector, then reconstruct the circuit via detail_make_circuit_from_payload.
Circuit RemoveIdentityPass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    std::vector<Op> new_ops;
    new_ops.reserve(old_ops.size());

    for (const Op& op : old_ops) {
        // Only GateOps with type == I and is_opaque == false are removed.
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            if (!g.is_opaque && g.type == GateType::I) {
                changed_ = true;
                continue;  // drop this identity gate
            }
        }
        new_ops.push_back(op);
    }

    if (!changed_) {
        return circuit;  // nothing removed — return unchanged
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool RemoveIdentityPass::changed() const {
    return changed_;
}

}  // namespace qplayer
