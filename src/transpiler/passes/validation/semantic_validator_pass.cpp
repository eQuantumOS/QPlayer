// semantic_validator_pass.cpp — IR-level opaque gate validator pass
// (⓪).
//
// Why this pass exists at the IR level (not only at parse time):
//   The QASM parser's SemanticValidator only sees what was textually present in
//   the .qasm source. A Circuit built programmatically via the C++ / Python API
//   can add opaque GateOp nodes whose definitions are registered later — or
//   never. Running this pass as the unconditional first pass in PassManager
//   catches the "declared but not defined" case before any backend or
// optimisation pass silently treats the gate as identity.

#include "transpiler/passes/validation/semantic_validator_pass.hpp"

#include <string>
#include <variant>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "error.hpp"

namespace qplayer {

std::string SemanticValidatorPass::name() const {
    return "SemanticValidatorPass";
}

Circuit SemanticValidatorPass::run(Circuit circuit) {
    // Iterate all ops; validate only GateOp nodes flagged as opaque.
    // Non-opaque GateOps (qelib1 standard gates, is_opaque==false) pass
    // through without any check — they have no OpaqueDefinition entry.
    for (const auto& op : circuit.ops()) {
        if (!std::holds_alternative<GateOp>(op)) {
            continue;
        }
        const auto& gate = std::get<GateOp>(op);
        if (!gate.is_opaque) {
            continue;  // Standard qelib1 gate — skip.
        }

        const std::string& gate_name = gate.opaque_name;

        // Check 1: the Circuit must have an OpaqueDefinition entry at all.
        if (!circuit.has_opaque_definition(gate_name)) {
            throw CircuitError{
                "opaque gate '" + gate_name +
                "' has no definition in this Circuit."
                " Register via Circuit::define_opaque() or remove from circuit."};
        }

        // Check 2: the body must be a concrete implementation, not a stub.
        // DeclarationOnly means declare_opaque() was called but define_opaque()
        // was never called — the backend cannot execute this gate.
        const auto& def = circuit.opaque_definition(gate_name);
        if (std::holds_alternative<DeclarationOnly>(def.body)) {
            throw CircuitError{
                "opaque gate '" + gate_name +
                "' has only a declaration; provide a definition via"
                " Circuit::define_opaque()"};
        }
    }

    // Validation passed — return the circuit unchanged.
    // changed_ stays false: this pass never modifies the circuit.
    return circuit;
}

bool SemanticValidatorPass::changed() const {
    // Validation-only pass: structural modifications are never performed.
    return false;
}

}  // namespace qplayer
