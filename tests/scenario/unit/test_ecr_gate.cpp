// test_ecr_gate.cpp — Unit tests for the ECR (Echoed Cross-Resonance) native
// 2-qubit basis gate. ECR is a genuine 4x4 unitary (not
// control+1Q), IBM Eagle's native entangling gate, created only via
// Circuit::ecr() / basis translation — never through OpenQASM v2.
// Covers: matrix correctness (unitary + Qiskit convention), CPU apply, operand
// order asymmetry, fused == unfused equivalence, JSON round-trip, and the two
// documented rejections (to_qasm, SparseState).
// (Arrange/Act/Assert, EXPECT_NEAR tolerances).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <variant>
#include <vector>

#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/backend/full_state/gate_fusion.hpp"
#include "../../../include/backend/full_state/gate_matrix.hpp"
#include "../../../include/backend/sparse_state/sparse_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/types.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::FullStateCpuBackend;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;
using qplayer::QubitIndex;
using qplayer::SimulatorError;
using qplayer::SparseStateBackend;
using qplayer::StateVector;
using qplayer::detail::apply_fused_gate;
using qplayer::detail::ecr_matrix_data;
using qplayer::detail::fuse_circuit;
using qplayer::detail::FusedOp;

namespace {

// Read ECR matrix element [r][c] from the flat SSOT (row-major, 2 doubles/entry).
std::complex<double> ecr_at(int r, int c) {
    const auto& d = ecr_matrix_data();
    const int idx = 8 * r + 2 * c;
    return {d[static_cast<std::size_t>(idx)], d[static_cast<std::size_t>(idx + 1)]};
}

GateOp make_ecr(QubitIndex q0, QubitIndex q1) {
    GateOp op;
    op.type   = GateType::ECR;
    op.name   = "ecr";
    op.qubits = {q0, q1};
    return op;
}

}  // namespace

// ── ECR matrix: unitary + matches the Qiskit ECRGate convention ────
// ECR = (1/sqrt2)(IX - XY). The specified matrix is Hermitian, hence unitary and
// self-inverse (M*M = I). We assert M*M^dagger = I plus the four canonical entries.
TEST(EcrGate, MatrixData_IsUnitaryAndMatchesConvention) {
    // Arrange: reconstruct the 4x4 from the flat SSOT.
    std::complex<double> M[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) M[r][c] = ecr_at(r, c);

    // Assert: M * M^dagger == I (rows are orthonormal). 1e-12: exact arithmetic.
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            std::complex<double> acc{0, 0};
            for (int k = 0; k < 4; ++k) acc += M[i][k] * std::conj(M[j][k]);
            const double expect = (i == j) ? 1.0 : 0.0;
            EXPECT_NEAR(acc.real(), expect, 1e-12);
            EXPECT_NEAR(acc.imag(), 0.0, 1e-12);
        }
    }

    // Assert: canonical entries M[0][1]=1/sqrt2, M[0][3]=i/sqrt2, M[3][0]=-i/sqrt2.
    const double s = M_SQRT1_2;
    EXPECT_NEAR(ecr_at(0, 1).real(), s, 1e-12);
    EXPECT_NEAR(ecr_at(0, 3).imag(), s, 1e-12);
    EXPECT_NEAR(ecr_at(3, 0).imag(), -s, 1e-12);
}

// ── CPU apply: ECR|00> = (1/sqrt2)(|01> - i|11>) in state-index terms ─────────
// ecr(0,1): operand 0 = qubit 0 (bit 0), operand 1 = qubit 1 (bit 1). Acting on
// |00> yields amp[1] = 1/sqrt2 (qubit0=1) and amp[3] = -i/sqrt2 (qubit0=qubit1=1).
TEST(EcrGate, ApplyOnGround_ProducesExpectedAmplitudes) {
    // Arrange: 2-qubit |00>.
    FullStateCpuBackend backend;
    backend.init_state(2);

    // Act.
    backend.apply_gate(make_ecr(0, 1));
    const StateVector sv = backend.get_statevector();

    // Assert. 1e-12: algebraic result, no sampling.
    const double s = M_SQRT1_2;
    EXPECT_NEAR(sv[0].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].real(), s, 1e-12);
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[3].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[3].imag(), -s, 1e-12);
}

// ── Operand order is significant: ecr(0,1) != ecr(1,0) ─────────────
// Swapping operands maps operand 0 to qubit 1, so |00> -> amp[2] (qubit1=1) and
// amp[3], distinct from the ecr(0,1) result above.
TEST(EcrGate, OperandOrder_IsAsymmetric) {
    // Arrange.
    FullStateCpuBackend backend;
    backend.init_state(2);

    // Act: reversed operands.
    backend.apply_gate(make_ecr(1, 0));
    const StateVector sv = backend.get_statevector();

    // Assert: excitation lands on qubit 1 (amp[2]), not qubit 0 (amp[1]).
    const double s = M_SQRT1_2;
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), s, 1e-12);
    EXPECT_NEAR(sv[2].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[3].imag(), -s, 1e-12);
}

// ── ECR is Hermitian, hence self-inverse: ECR*ECR = I (matrix) ───────
// NOTE: prose says "not self-inverse", but the matrix it specifies is
// Hermitian => self-inverse. This test pins the true behaviour of the matrix.
TEST(EcrGate, AppliedTwice_ReturnsToGroundState) {
    // Arrange.
    FullStateCpuBackend backend;
    backend.init_state(2);

    // Act: ECR twice on the same operands.
    backend.apply_gate(make_ecr(0, 1));
    backend.apply_gate(make_ecr(0, 1));
    const StateVector sv = backend.get_statevector();

    // Assert: back to |00>.
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    for (std::size_t i = 1; i < 4; ++i) {
        EXPECT_NEAR(sv[i].real(), 0.0, 1e-12);
        EXPECT_NEAR(sv[i].imag(), 0.0, 1e-12);
    }
}

// ── Fused == unfused: the fusion 4x4 builder matches the dense-2Q kernel ───────
// Guards against gate_full_matrix() silently identity-falling-back on ECR: a
// mismatch here means the fused GEMV and apply_dense_2q_kernel disagree.
TEST(EcrGate, FusedEqualsUnfused_OnEntangledContext) {
    // Arrange: 3-qubit circuit exercising ECR on non-adjacent, reversed operands
    // surrounded by 1Q gates so fusion actually forms a multi-qubit window.
    Circuit circ(3);
    circ.h(0).x(2).ecr(2, 0).h(1).ecr(0, 1);

    FullStateCpuBackend backA;  // unfused
    FullStateCpuBackend backB;  // fused
    const QubitIndex n = circ.num_qubits();

    // Act: path A (per-gate apply).
    backA.init_state(n);
    for (const Op& op : circ.ops())
        if (std::holds_alternative<GateOp>(op))
            backA.apply_gate(std::get<GateOp>(op));
    const StateVector svA = backA.get_statevector();

    // Act: path B (fuse then apply program).
    backB.init_state(n);
    const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), 4);
    backB.apply_fused_program(prog);
    const StateVector svB = backB.get_statevector();

    // Assert: identical to 1e-12.
    EXPECT_EQ(svA.size(), svB.size());
    for (std::size_t i = 0; i < svA.size(); ++i) {
        EXPECT_NEAR(svA[i].real(), svB[i].real(), 1e-12);
        EXPECT_NEAR(svA[i].imag(), svB[i].imag(), 1e-12);
    }
}

// ── JSON round-trip preserves ECR type, name, and operand order ─────
TEST(EcrGate, JsonRoundTrip_PreservesGate) {
    // Arrange: circuit with a reversed-operand ECR.
    Circuit circ(2);
    circ.ecr(1, 0);

    // Act.
    const std::string json = circ.to_json();
    const Circuit restored = Circuit::from_json(json);

    // Assert: single GateOp, ECR type, operands preserved in order.
    const auto& ops = restored.ops();
    std::size_t ecr_count = 0;
    for (const Op& op : ops) {
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            if (g.type == GateType::ECR) {
                ++ecr_count;
                EXPECT_EQ(g.name, std::string{"ecr"});
                EXPECT_EQ(g.qubits.size(), std::size_t{2});
                EXPECT_EQ(g.qubits[0], QubitIndex{1});
                EXPECT_EQ(g.qubits[1], QubitIndex{0});
            }
        }
    }
    EXPECT_EQ(ecr_count, std::size_t{1});
}

// ── ECR has no QASM v2 form: to_qasm() must throw, not emit invalid text ───────
TEST(EcrGate, ToQasm_Throws) {
    Circuit circ(2);
    circ.ecr(0, 1);
    EXPECT_THROW(circ.to_qasm(), CircuitError);
}

// ── SparseState rejects ECR explicitly (no silent identity fallback) ──────────
TEST(EcrGate, SparseStateApply_Throws) {
    SparseStateBackend backend;
    backend.init_state(2);
    EXPECT_THROW(backend.apply_gate(make_ecr(0, 1)), SimulatorError);
}

// ── Builder rejects same-qubit ECR (distinct-qubit invariant) ─────────────────
TEST(EcrGate, BuilderRejectsSameQubit) {
    Circuit circ(2);
    EXPECT_THROW(circ.ecr(1, 1), CircuitError);
}
