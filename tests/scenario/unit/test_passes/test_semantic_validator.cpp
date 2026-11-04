// test_semantic_validator.cpp — Unit tests for SemanticValidatorPass
// (⓪).
//
// Opaque GateOp nodes are injected via detail_circuit_push_op() — the same
// internal bridge used by CircuitBuilder. This avoids adding a public
// API for a scenario that normal user code cannot construct through the
// builder (the builder chains only qelib1 named gates + declare/define_opaque).

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <array>
#include <complex>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/error.hpp"
#include "../../../../include/transpiler/passes/validation/semantic_validator_pass.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::DeclarationOnly;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;
using qplayer::SemanticValidatorPass;

// Helper: build an opaque GateOp node for the named gate on qubit 0.
// is_opaque must be true and opaque_name must match the declaration name so
// SemanticValidatorPass can look up the OpaqueDefinition.
static GateOp make_opaque_gate(const std::string& gate_name) {
    GateOp g;
    g.type        = GateType::Opaque;
    g.name        = gate_name;
    g.opaque_name = gate_name;
    g.qubits      = {0};
    g.is_opaque   = true;
    return g;
}

// ── Scenario 1: defined opaque gate passes through without throwing ───────────

TEST(SemanticValidatorPass, DefinedOpaque_PassThrough_NoThrow) {
    // Arrange: declare + define a 1-qubit opaque gate, then add it to the circuit.
    Circuit c(1);
    c.declare_opaque("myop", 1);
    // 2x2 identity matrix satisfies the 1-qubit definition requirement.
    std::array<std::array<std::complex<double>, 2>, 2> identity = {{
        {std::complex<double>{1.0, 0.0}, std::complex<double>{0.0, 0.0}},
        {std::complex<double>{0.0, 0.0}, std::complex<double>{1.0, 0.0}}
    }};
    c.define_opaque("myop", identity);
    detail_circuit_push_op(c, Op{make_opaque_gate("myop")});

    // Act + Assert: must not throw.
    SemanticValidatorPass pass;
    EXPECT_NO_THROW(pass.run(c));
}

// ── Scenario 2: declared-only opaque gate is rejected ────────────────────────

TEST(SemanticValidatorPass, DeclarationOnlyOpaque_ThrowsCircuitError) {
    // Arrange: declare without defining — body stays DeclarationOnly.
    Circuit c(1);
    c.declare_opaque("myop", 1);
    detail_circuit_push_op(c, Op{make_opaque_gate("myop")});

    // Act + Assert: SemanticValidatorPass must throw CircuitError.
    SemanticValidatorPass pass;
    EXPECT_THROW(pass.run(c), CircuitError);
}

// ── Scenario 3: opaque gate with no declaration/definition is rejected ────────

TEST(SemanticValidatorPass, UndeclaredOpaque_UsedInCircuit_ThrowsCircuitError) {
    // Arrange: inject an opaque GateOp directly without any declare_opaque call.
    // Simulates a Circuit built by a QASM parser that produced a GateOp but lost
    // its declaration — or a programmatic mutation that bypassed the builder API.
    Circuit c(1);
    detail_circuit_push_op(c, Op{make_opaque_gate("ghost_op")});

    // Act + Assert: must throw because has_opaque_definition("ghost_op") == false.
    SemanticValidatorPass pass;
    EXPECT_THROW(pass.run(c), CircuitError);
}

// ── Scenario 4: circuit with only qelib1 standard gates passes through ────────

TEST(SemanticValidatorPass, NoOpaqueGates_PassThrough_NoThrow) {
    // Arrange: a plain Bell-state circuit — all gates have is_opaque==false.
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act + Assert: no opaque gates → validator is a no-op, no throw.
    SemanticValidatorPass pass;
    EXPECT_NO_THROW(pass.run(c));
}

// ── changed() invariant: validation pass never sets changed to true ───────────

TEST(SemanticValidatorPass, Changed_AlwaysFalse) {
    Circuit c(2, 2);
    c.h(0).cx(0, 1);

    SemanticValidatorPass pass;
    pass.run(c);
    // changed() must be false: this pass is validation-only, never modifies.
    EXPECT_FALSE(pass.changed());
}

// ── name() returns expected string ────────────────────────────────────────────

TEST(SemanticValidatorPass, Name_ReturnsExpectedString) {
    SemanticValidatorPass pass;
    EXPECT_EQ(pass.name(), std::string{"SemanticValidatorPass"});
}
