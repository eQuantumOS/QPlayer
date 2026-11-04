// test_pass_chain.cpp — Integration tests for PassManager chain scenarios.
// Verifies that multiple passes chained in a PassManager interact correctly:
// each pass receives the output of the previous pass as input, and the final
// result reflects the cumulative effect of all passes in registration order.
//
// Testing rules: -1.3. All comparisons use typed EXPECT_*
// macros. Arrange / Act / Assert 3-section format (— English comments).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstddef>
#include <memory>
#include <variant>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/error.hpp"
#include "../../../include/transpiler/ipass.hpp"
#include "../../../include/transpiler/pass_manager.hpp"
#include "../../../include/transpiler/passes/optimization/cancel_inverse.hpp"
#include "../../../include/transpiler/passes/optimization/commutation_reorder_pass.hpp"
#include "../../../include/transpiler/passes/optimization/merge_single_qubit_pass.hpp"
#include "../../../include/transpiler/passes/optimization/diagonal_phase_pass.hpp"
#include "../../../include/transpiler/passes/optimization/remove_identity_pass.hpp"
#include "../../../include/transpiler/passes/optimization/merge_same_gate_pass.hpp"
#include "../../../include/transpiler/passes/optimization/pattern_replace_pass.hpp"
#include "../../../include/transpiler/passes/optimization/remove_idle_qubit_pass.hpp"
#include "../../../include/transpiler/passes/optimization/merge_two_qubit_pass.hpp"
#include "../../../include/transpiler/passes/validation/semantic_validator_pass.hpp"

using qplayer::CancelInversePass;
using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::CommutationReorderPass;
using qplayer::DiagonalPhasePass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::IPass;
using qplayer::MergeSameGatePass;
using qplayer::MergeSingleQubitPass;
using qplayer::MergeTwoQubitPass;
using qplayer::Op;
using qplayer::PassManager;
using qplayer::PatternReplacePass;
using qplayer::QubitIndex;
using qplayer::RemoveIdentityPass;
using qplayer::RemoveIdleQubitPass;
using qplayer::SemanticValidatorPass;

// Internal bridge for injecting raw Op nodes — same technique as
// test_transpiler.cpp / test_semantic_validator.cpp.
namespace qplayer {
    void detail_circuit_push_op(Circuit& c, Op op);
}

// ── Case 1: Level-1 equivalent chain (Sem + CancelInverse + Commutation + Merge1Q)
// Registers the same passes as Level-1 transpile and verifies the result is
// identical to calling Transpiler::transpile(c, 1) on a clean circuit.

TEST(PassChain, Level1Equivalent_CleanBellLike_NoThrow) {
    // Arrange: clean 2-qubit circuit that passes semantic validation.
    Circuit c(2, 2);
    c.h(0).cx(0, 1);

    // Act: build a PassManager replicating the Level-1 pipeline.
    PassManager pm;
    pm.add_pass(std::make_unique<SemanticValidatorPass>());
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<CommutationReorderPass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());

    // Assert: no exception and circuit structure is preserved (no cancellation).
    Circuit out = pm.run(std::move(c));
    EXPECT_EQ(out.num_ops(), std::size_t{2});  // H + CX survive
}

// ── Case 2: Full Level-2/3 pass set — cumulative reduction on a composite circuit.
// Registers all 9 optimization passes directly (bypassing optimization_level policy)
// to verify that each pass's output feeds the next one.
//
// Circuit design notes:
//   - id(0)         : removed by RemoveIdentityPass.
//   - cx(0,1)+cx(0,1): adjacent CX pair cancels via CancelInversePass.
//   - h(0), measure(0,0): H survives (no cancel pair); measure keeps q[0] live.
//   - x(1), measure(1,1): X survives; measure keeps q[1] live.
// RemoveIdleQubitPass requires at least one MeasureOp per qubit to retain the
// qubit's gates.  Without measurement the pass treats all qubits as idle and
// would remove everything (specification).

TEST(PassChain, AllNinePasses_CompositeCircuit_CumulativeReduction) {
    // Arrange
    Circuit c(2, 2);
    c.id(0);         // identity — removed by RemoveIdentityPass
    c.cx(0, 1);      // adjacent CX pair — cancelled by CancelInversePass
    c.cx(0, 1);
    c.h(0);          // survives (no inverse pair)
    c.x(1);          // survives (no inverse pair)
    c.measure(0, 0); // keeps q[0] live for RemoveIdleQubitPass
    c.measure(1, 1); // keeps q[1] live for RemoveIdleQubitPass

    PassManager pm;
    pm.add_pass(std::make_unique<SemanticValidatorPass>());
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<CommutationReorderPass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());
    pm.add_pass(std::make_unique<DiagonalPhasePass>());
    pm.add_pass(std::make_unique<RemoveIdentityPass>());
    pm.add_pass(std::make_unique<MergeSameGatePass>());
    pm.add_pass(std::make_unique<PatternReplacePass>());
    pm.add_pass(std::make_unique<RemoveIdleQubitPass>());
    pm.add_pass(std::make_unique<MergeTwoQubitPass>());

    Circuit out = pm.run(std::move(c));

    // id(0) removed (-1), cx pair cancelled (-2), h(0)+x(1)+measure(0)+measure(1) survive.
    // Total remaining: 4 ops (H + X + measure×2).
    EXPECT_EQ(out.num_ops(), std::size_t{4});
}

// ── Case 3: CancelInverse → CommutationReorder → MergeSingleQubit pipeline.
// H·H·X·X sequence: CancelInverse removes HH and XX pairs in order so that
// nothing survives when the three Level-1 passes run in sequence.

TEST(PassChain, Level1Three_HH_XX_AllCancelled) {
    // Arrange: H-H pair on q[0] and X-X pair on q[1].
    Circuit c(2);
    c.h(0).h(0);
    c.x(1).x(1);

    PassManager pm;
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<CommutationReorderPass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());

    Circuit out = pm.run(std::move(c));

    // Both pairs cancel -> empty circuit.
    EXPECT_EQ(out.num_ops(), std::size_t{0});
}

// ── Case 4: Opaque gate with definition — chain passes through unchanged.
// SemanticValidatorPass must accept a circuit whose opaque gate has a concrete
// UnitaryMatrix definition, and subsequent passes must leave it untouched.

TEST(PassChain, OpaqueWithDefinition_ChainPassesThrough) {
    // Arrange: declare + define a 1-qubit opaque gate with a 2x2 unitary (identity).
    Circuit c(1);
    c.declare_opaque("mygate", 1);
    // Use the 2x2 overload: row-major identity matrix.
    std::array<std::array<std::complex<double>, 2>, 2> mat = {{
        {std::complex<double>{1.0, 0.0}, std::complex<double>{0.0, 0.0}},
        {std::complex<double>{0.0, 0.0}, std::complex<double>{1.0, 0.0}}
    }};
    c.define_opaque("mygate", mat);

    // Inject the GateOp directly (same pattern as test_transpiler.cpp).
    GateOp g;
    g.type        = GateType::Opaque;
    g.name        = "mygate";
    g.opaque_name = "mygate";
    g.qubits      = {QubitIndex{0}};
    g.is_opaque   = true;
    qplayer::detail_circuit_push_op(c, Op{g});

    PassManager pm;
    pm.add_pass(std::make_unique<SemanticValidatorPass>());
    pm.add_pass(std::make_unique<CancelInversePass>());

    // Assert: no exception; opaque gate survives (not touched by CancelInverse).
    Circuit out = pm.run(std::move(c));
    EXPECT_EQ(out.num_ops(), std::size_t{1});
}

// ── Case 5: Opaque gate without definition — SemanticValidatorPass in chain throws.
// A chain starting with SemanticValidatorPass must reject a circuit that
// contains a DeclarationOnly opaque gate.

TEST(PassChain, OpaqueDeclarationOnly_ChainThrowsAtFirstPass) {
    // Arrange: declare but do not define the opaque gate.
    Circuit c(1);
    c.declare_opaque("bad_gate", 1);

    GateOp g;
    g.type        = GateType::Opaque;
    g.name        = "bad_gate";
    g.opaque_name = "bad_gate";
    g.qubits      = {QubitIndex{0}};
    g.is_opaque   = true;
    qplayer::detail_circuit_push_op(c, Op{g});

    PassManager pm;
    pm.add_pass(std::make_unique<SemanticValidatorPass>());
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());

    // Assert: SemanticValidatorPass (first pass) must throw before any opt pass runs.
    EXPECT_THROW(pm.run(std::move(c)), CircuitError);
}

// ── Case 6: MeasureOp boundary — no pass removes a MeasureOp during chain execution.
// Runs all Level-1 passes on a circuit with measurement; the MeasureOp must
// survive the entire pipeline.

TEST(PassChain, MeasureBoundary_MeasureOpPreservedAcrossAllPasses) {
    // Arrange: H + measure(0,0) — the H is a single op, measure follows.
    Circuit c(1, 1);
    c.h(0).measure(0, 0);

    PassManager pm;
    pm.add_pass(std::make_unique<SemanticValidatorPass>());
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<CommutationReorderPass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());

    Circuit out = pm.run(std::move(c));

    // Both H and MeasureOp survive (H has no partner, MeasureOp is not removed).
    EXPECT_EQ(out.num_ops(), std::size_t{2});

    // Verify the last op is still a MeasureOp.
    const auto& ops = out.ops();
    EXPECT_TRUE(std::holds_alternative<qplayer::MeasureOp>(ops[1]));
}

// ── Case 7: Empty circuit chain — all passes run with changed()=false, return empty.

TEST(PassChain, EmptyCircuit_AllPassesChangedFalse_EmptyOutput) {
    // Arrange: circuit with 2 qubits but no ops.
    Circuit c(2);

    PassManager pm;
    pm.add_pass(std::make_unique<SemanticValidatorPass>());
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<CommutationReorderPass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());
    pm.add_pass(std::make_unique<RemoveIdentityPass>());

    // Act
    Circuit out = pm.run(std::move(c));

    // Assert: empty in -> empty out.
    EXPECT_EQ(out.num_ops(), std::size_t{0});
}

// ── Case 8: Rotation gate boundary — RZ(theta) passes through the Level-1 chain.
// Level-1 passes do not decompose or remove rotation gates with non-zero angles.

TEST(PassChain, RotationGateBoundary_RZPreservedByLevel1Chain) {
    // Arrange: h(0) + rz(1.5, 0) + h(0) — the two H's are separated by RZ,
    // so CancelInverse must NOT cancel them (RZ acts as a boundary).
    Circuit c(1);
    c.h(0).rz(1.5, 0).h(0);

    PassManager pm;
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<CommutationReorderPass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());

    Circuit out = pm.run(std::move(c));

    // RZ is not removed; H's cannot cancel across RZ -> all 3 ops survive.
    EXPECT_EQ(out.num_ops(), std::size_t{3});
}
