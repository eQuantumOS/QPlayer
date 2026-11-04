// semantic_validator_pass.hpp — IR-level opaque gate validator pass
// (⓪).
// Runs as the first pass in PassManager regardless of optimization_level
// Distinct from the parser-level SemanticValidator
// which validates QASM syntax; this pass validates the Circuit IR after
// the QASM parse-and-build stage is complete.
//
// Invariant enforced: every GateOp with is_opaque==true must have a concrete
// OpaqueDefinition body (UnitaryMatrix or KrausChannel) registered on the
// Circuit — DeclarationOnly stubs are rejected here to prevent silent identity
// or mis-dispatch in the backend.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class SemanticValidatorPass : public IPass {
public:
    SemanticValidatorPass() = default;

    std::string name() const override;

    // Traverses Circuit::ops() and validates every GateOp with is_opaque==true.
    // Throws CircuitError if an opaque gate has no definition or only a
    // DeclarationOnly body. Returns the Circuit unchanged on success (validation
    // pass — no structural modifications). changed() always returns false.
    Circuit run(Circuit circuit) override;

    // Always false: this pass is validation-only and never modifies the circuit.
    bool changed() const override;
};

}  // namespace qplayer
