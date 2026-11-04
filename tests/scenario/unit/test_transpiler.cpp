// test_transpiler.cpp — Unit tests for Transpiler::transpile() and
// PassManager::make_default().
//
// Coverage:
//   1. Level 0: SemanticValidatorPass only, returns circuit unchanged.
//   2. Level 1 (default): Sem + CancelInverse + CommutationReorder + MergeSingleQubit.
//   3. Level 1 opt_cancel-style circuit (H H): num_ops reduced to 0.
// 4. Level 2: CircuitError (pending).
// 5. Level 3: CircuitError (pending).
//   6. device_json empty: normal path, no exception.
//   7. device_json non-empty: NotImplementedError (Phase 6+ stub).
//   8. Opaque DeclarationOnly circuit -> CircuitError via SemanticValidatorPass.
// 9. PassManager::make_default() returns PM with 3 Level-1 passes.
//  10. PassManager::make_default() PM runs without throwing on clean circuit.

#include "../../framework/assertions.hpp"
#include "../../framework/test_case.hpp"

#include <cstddef>
#include <memory>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/error.hpp"
#include "../../../include/transpiler/pass_manager.hpp"
#include "../../../include/transpiler/transpiler.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::NotImplementedError;
using qplayer::Op;
using qplayer::PassManager;
using qplayer::QubitIndex;
using qplayer::Transpiler;

// Internal bridge for injecting raw Op nodes (same technique as
// test_semantic_validator.cpp). Used only for the opaque-gate test case.
namespace qplayer {
    void detail_circuit_push_op(Circuit& c, Op op);
}

// ── Helper ───────────────────────────────────────────────────────────────────

// Returns a clean 2-qubit circuit with no opaque gates (passes SemanticValidator).
static Circuit make_clean_circuit() {
    Circuit c(2, 2);
    c.h(0).cx(0, 1);
    return c;
}

// ── Level 0 ──────────────────────────────────────────────────────────────────

TEST(Transpiler, Level0_SemanticOnly_ReturnsCircuitUnchanged) {
    // Arrange
    Circuit c(2, 2);
    c.h(0).cx(0, 1);
    const std::size_t original_ops = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 0);

    // Assert: Level 0 has no optimization passes — op count must be unchanged.
    EXPECT_EQ(out.num_ops(), original_ops);
}

// ── Level 1 basic ────────────────────────────────────────────────────────────

TEST(Transpiler, Level1_CleanCircuit_NoThrow) {
    // Arrange
    Circuit c = make_clean_circuit();

    // Act + Assert
    Transpiler t;
    EXPECT_NO_THROW(t.transpile(std::move(c), 1));
}

TEST(Transpiler, Level1_HHPair_CancelledToZeroOps) {
    // Arrange: two consecutive H gates on the same qubit should cancel.
    Circuit c(1);
    c.h(0);
    c.h(0);

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: both H gates cancel -> num_ops == 0.
    // CancelInversePass collapses H H -> identity (removed).
    EXPECT_EQ(out.num_ops(), std::size_t{0});
}

TEST(Transpiler, Level1_SingleH_SurvivesCancellation) {
    // Arrange: single lone H must not be removed.
    Circuit c(1);
    c.h(0);

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: single H has no inverse pair -> survives.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
}

// ── Level 2 / 3 activation ───────────────────────────────────────
// Level 2 = Level 1 + DiagonalPhase + RemoveIdentity + MergeSameGate + PatternReplace.
// Level 3 = Level 2 + RemoveIdleQubit + MergeTwoQubit.

TEST(Transpiler, Level2_RunsWithoutThrow) {
    // Level 2 activates 1Q safe reduction passes.
    Circuit c = make_clean_circuit();
    Transpiler t;
    EXPECT_NO_THROW(t.transpile(std::move(c), 2));
}

TEST(Transpiler, Level3_RunsWithoutThrow) {
    // Level 3 adds 2Q + qubit pruning passes on top of Level 2.
    Circuit c = make_clean_circuit();
    Transpiler t;
    EXPECT_NO_THROW(t.transpile(std::move(c), 3));
}

TEST(Transpiler, Level2_RemovesIdentityGate) {
    // Level 2 includes RemoveIdentityPass — an id() should be stripped.
    Circuit c(1);
    c.id(0);
    c.h(0);
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 2);
    EXPECT_EQ(out.num_ops(), std::size_t{1});
}

TEST(Transpiler, Level2_AccumulatesDiagonalPhase_TTtoS) {
    // Level 2 includes DiagonalPhasePass — t·t accumulates to S (single op).
    Circuit c(1);
    c.t(0);
    c.t(0);
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 2);
    EXPECT_EQ(out.num_ops(), std::size_t{1});
}

TEST(Transpiler, OutOfRangeLevel_Throws_CircuitError) {
    // Level <0 or >3 is out of range.
    Circuit c1 = make_clean_circuit();
    Transpiler t;
    EXPECT_THROW(t.transpile(std::move(c1), -1), CircuitError);
    Circuit c2 = make_clean_circuit();
    EXPECT_THROW(t.transpile(std::move(c2), 4), CircuitError);
}

// ── device_json handling ─────────────────────────────────────────────────────

TEST(Transpiler, EmptyDeviceJson_NormalPath_NoThrow) {
    // Empty device_json -> device-less path -> no exception.
    Circuit c = make_clean_circuit();
    Transpiler t;
    EXPECT_NO_THROW(t.transpile(std::move(c), 1, ""));
}

TEST(Transpiler, NonEmptyDeviceJson_RoutesWithoutThrow) {
    // Non-empty valid device_json -> device-aware pipeline runs. An
    // empty coupling_map means full connectivity, so routing is a no-op and the
    // compile completes without error (② ③).
    Circuit c = make_clean_circuit();
    Transpiler t;
    EXPECT_NO_THROW(t.transpile(std::move(c), 1, "{\"coupling_map\":[]}"));
}

// ── SemanticValidatorPass integration ────────────────────────────────────────

TEST(Transpiler, OpaqueDeclarationOnly_Throws_CircuitError) {
    // Circuit that contains a GateOp with is_opaque=true but only a
    // DeclarationOnly body -> SemanticValidatorPass must reject it.
    // Use detail_circuit_push_op to inject a raw opaque GateOp (same technique
    // as test_semantic_validator.cpp).
    Circuit c(1);
    c.declare_opaque("mygate", 1);
    // DeclarationOnly body remains -- no define_opaque() call.
    GateOp g;
    g.type        = GateType::Opaque;
    g.name        = "mygate";
    g.opaque_name = "mygate";
    g.qubits      = {QubitIndex{0}};
    g.is_opaque   = true;
    qplayer::detail_circuit_push_op(c, Op{g});

    Transpiler t;
    EXPECT_THROW(t.transpile(std::move(c), 0), CircuitError);
}

// ── PassManager::make_default() ─────────────────────────────────────────

TEST(PassManager, MakeDefault_T44_SizeIsThree) {
    // : make_default() must register exactly 3 Level-1 passes:
    //   CancelInverse, CommutationReorder, MergeSingleQubit.
    PassManager pm = PassManager::make_default();
    EXPECT_EQ(pm.size(), std::size_t{3});
}

TEST(PassManager, MakeDefault_T44_RunsWithoutThrow) {
    // The default PM must run a clean circuit without throwing.
    PassManager pm = PassManager::make_default();
    Circuit c = make_clean_circuit();
    EXPECT_NO_THROW(pm.run(std::move(c)));
}

// ── opt_cancel-style full pipeline verification ───────────────────────────────

TEST(Transpiler, OptCancelStyle_FivePairs_PlusOneLoneH_OneOpRemains) {
    // Mirrors the opt_cancel.qasm acceptance scenario in unit-test form.
    // Circuit: lone H q[0] + pairs (X-X, Z-Z, CX-CX, H-H on q[2]).
    // After Level 1 transpile, only the lone H must survive.
    Circuit c(3, 3);
    c.h(0);       // lone -- must survive
    c.x(1);       // pair start: X-X cancels
    c.x(1);       // pair end
    c.z(2);       // pair start: Z-Z cancels
    c.z(2);       // pair end
    c.cx(0, 1);   // pair start: CX-CX cancels
    c.cx(0, 1);   // pair end
    c.h(2);       // pair start: H-H cancels
    c.h(2);       // pair end

    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 4 pairs cancelled, lone H survives -> num_ops == 1.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
}
