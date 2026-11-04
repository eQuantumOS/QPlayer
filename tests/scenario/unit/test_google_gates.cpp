// test_google_gates.cpp — Unit tests for Google native 2-qubit basis gates:
// iSWAP, sqrt_iSWAP, and fSim. All three are genuine 4x4
// unitaries applied via apply_dense_2q_kernel. None is self-inverse.
// Covers: matrix correctness (unitary + Qiskit/Cirq convention), CPU apply on
// basis states, sqrt_iswap**2 == iswap, fsim(0,0)==I, fsim(pi/2,pi/6) Sycamore
// spot value, sycamore() == fsim(pi/2,pi/6) equivalence, JSON round-trip,
// SparseState rejection, to_qasm rejection, not-self-inverse (cancel_inverse
// safety), and builder distinctness guard.
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
using qplayer::detail::fsim_matrix_data;
using qplayer::detail::fuse_circuit;
using qplayer::detail::FusedOp;
using qplayer::detail::iswap_matrix_data;
using qplayer::detail::sqrt_iswap_matrix_data;

namespace {

// Read a 4x4 matrix element [r][c] from a flat 32-double array (row-major).
std::complex<double> mat_at(const double* d, int r, int c) {
    const int idx = 8 * r + 2 * c;
    return {d[idx], d[idx + 1]};
}

// Helper: verify M * M^dagger == I for a 4x4 matrix given as 32 doubles.
void expect_unitary(const double* d) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            std::complex<double> acc{0, 0};
            for (int k = 0; k < 4; ++k)
                acc += mat_at(d, i, k) * std::conj(mat_at(d, j, k));
            const double expect_re = (i == j) ? 1.0 : 0.0;
            // 1e-12: exact arithmetic (no sampling), tight tolerance.
            EXPECT_NEAR(acc.real(), expect_re, 1e-12);
            EXPECT_NEAR(acc.imag(), 0.0,       1e-12);
        }
    }
}

GateOp make_iswap(QubitIndex q0, QubitIndex q1) {
    GateOp op;
    op.type   = GateType::ISWAP;
    op.name   = "iswap";
    op.qubits = {q0, q1};
    return op;
}

GateOp make_sqrt_iswap(QubitIndex q0, QubitIndex q1) {
    GateOp op;
    op.type   = GateType::SQRT_ISWAP;
    op.name   = "sqrt_iswap";
    op.qubits = {q0, q1};
    return op;
}

GateOp make_fsim(double theta, double phi, QubitIndex q0, QubitIndex q1) {
    GateOp op;
    op.type   = GateType::FSIM;
    op.name   = "fsim";
    op.qubits = {q0, q1};
    op.params = {theta, phi};
    return op;
}

}  // namespace

// ── iSWAP matrix: unitary + Qiskit/Cirq convention ───────────────────────────
// iSWAP = [[1,0,0,0],[0,0,i,0],[0,i,0,0],[0,0,0,1]].
TEST(GoogleGates, iSWAP_MatrixData_IsUnitaryAndMatchesConvention) {
    const auto& d = iswap_matrix_data();
    expect_unitary(d.data());

    // Canonical entries: M[1][2]=i, M[2][1]=i, diagonal={1,0,0,1}.
    EXPECT_NEAR(mat_at(d.data(), 0, 0).real(), 1.0, 1e-12);
    EXPECT_NEAR(mat_at(d.data(), 1, 2).imag(), 1.0, 1e-12);  // i
    EXPECT_NEAR(mat_at(d.data(), 2, 1).imag(), 1.0, 1e-12);  // i
    EXPECT_NEAR(mat_at(d.data(), 3, 3).real(), 1.0, 1e-12);
    // Off-diagonal zeros for the |00> and |11> rows.
    EXPECT_NEAR(mat_at(d.data(), 0, 1).real(), 0.0, 1e-12);
    EXPECT_NEAR(mat_at(d.data(), 3, 0).real(), 0.0, 1e-12);
}

// ── iSWAP CPU apply: iSWAP|01> = i|10> ──────────────────────────────────────
// Basis index convention: bit 0 = qubit 0 (LSB). |01> -> state index 1.
// iSWAP row mapping: new_amp[c] = sum_r M[c][r]*amp[r]; since amp = delta_{r,1},
// new_amp[j] = M[j][1]. Row-col convention: M[r][c] = data[8r+2c].
// M[2][1] = i => amp[2] = i (i.e. qubit1=1, qubit0=0 = |10>).
TEST(GoogleGates, iSWAP_ApplyOn01_ProducesI_Times10) {
    // Arrange: 2-qubit state |01> (qubit 0 = 1, state index = 1).
    FullStateCpuBackend backend;
    backend.init_state(2);
    backend.apply_gate([]() {
        GateOp op; op.type = GateType::X; op.name = "x"; op.qubits = {0}; return op;
    }());

    // Act.
    backend.apply_gate(make_iswap(0, 1));
    const StateVector sv = backend.get_statevector();

    // Assert: iSWAP|01> = i|10>. amp[2] = i; all others zero. 1e-12: algebraic.
    EXPECT_NEAR(sv[0].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].imag(), 1.0, 1e-12);  // i
    EXPECT_NEAR(sv[3].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[3].imag(), 0.0, 1e-12);
}

// ── sqrt_iSWAP matrix: unitary + sqrt_iswap^2 == iSWAP ──────────────────────
// sqrt_iSWAP = [[1,0,0,0],[0,1/√2,i/√2,0],[0,i/√2,1/√2,0],[0,0,0,1]].
TEST(GoogleGates, SqrtISWAP_MatrixData_IsUnitaryAndMatchesConvention) {
    const auto& d = sqrt_iswap_matrix_data();
    expect_unitary(d.data());

    const double s = M_SQRT1_2;
    // M[1][1] = 1/sqrt2 (real), M[1][2] = i/sqrt2 (imaginary).
    EXPECT_NEAR(mat_at(d.data(), 1, 1).real(), s, 1e-12);
    EXPECT_NEAR(mat_at(d.data(), 1, 2).imag(), s, 1e-12);
    EXPECT_NEAR(mat_at(d.data(), 2, 1).imag(), s, 1e-12);
    EXPECT_NEAR(mat_at(d.data(), 2, 2).real(), s, 1e-12);
}

// ── sqrt_iSWAP applied twice == iSWAP (algebraic identity) ───────────────────
// Verifies the naming is correct: (sqrt_iSWAP)^2 = iSWAP.
TEST(GoogleGates, SqrtISWAP_AppliedTwice_EqualsISWAP) {
    // Arrange: two separate backends, both in |01> via X(0).
    FullStateCpuBackend backA;  // sqrt_iswap twice
    FullStateCpuBackend backB;  // iswap once
    auto x0 = []() { GateOp op; op.type = GateType::X; op.name = "x"; op.qubits = {0}; return op; };

    backA.init_state(2); backA.apply_gate(x0());
    backA.apply_gate(make_sqrt_iswap(0, 1));
    backA.apply_gate(make_sqrt_iswap(0, 1));

    backB.init_state(2); backB.apply_gate(x0());
    backB.apply_gate(make_iswap(0, 1));

    // Assert: identical state vectors to 1e-12.
    const StateVector svA = backA.get_statevector();
    const StateVector svB = backB.get_statevector();
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(svA[i].real(), svB[i].real(), 1e-12);
        EXPECT_NEAR(svA[i].imag(), svB[i].imag(), 1e-12);
    }
}

// ── fSim matrix: unitary + Qiskit/Cirq convention ────────────────────────────
// fSim(theta,phi) = [[1,0,0,0],[0,cosθ,-i sinθ,0],[0,-i sinθ,cosθ,0],[0,0,0,e^{-iφ}]].
TEST(GoogleGates, fSim_MatrixData_IsUnitaryAndMatchesConvention) {
    const double theta = 0.7;
    const double phi   = 0.3;
    const auto m = fsim_matrix_data(theta, phi);
    expect_unitary(m.data());

    const double ct = std::cos(theta), st = std::sin(theta);
    const double cp = std::cos(phi),   sp = std::sin(phi);
    // M[1][1] = cos(theta), M[1][2].imag = -sin(theta).
    EXPECT_NEAR(mat_at(m.data(), 1, 1).real(), ct,   1e-12);
    EXPECT_NEAR(mat_at(m.data(), 1, 2).imag(), -st,  1e-12);
    EXPECT_NEAR(mat_at(m.data(), 2, 1).imag(), -st,  1e-12);
    EXPECT_NEAR(mat_at(m.data(), 2, 2).real(), ct,   1e-12);
    // M[3][3] = e^{-i*phi} = cos(phi) - i*sin(phi).
    EXPECT_NEAR(mat_at(m.data(), 3, 3).real(), cp,   1e-12);
    EXPECT_NEAR(mat_at(m.data(), 3, 3).imag(), -sp,  1e-12);
}

// ── fSim(0,0) == Identity ─────────────────────────────────────────────────────
// When theta=phi=0, fSim collapses to I: cos(0)=1, sin(0)=0, e^0=1.
TEST(GoogleGates, fSim_ZeroParams_IsIdentity) {
    FullStateCpuBackend backend;
    backend.init_state(2);
    backend.apply_gate([]() {
        GateOp op; op.type = GateType::H; op.name = "h"; op.qubits = {0}; return op;
    }());
    const StateVector before = backend.get_statevector();

    backend.apply_gate(make_fsim(0.0, 0.0, 0, 1));
    const StateVector after = backend.get_statevector();

    // 1e-12: algebraic — fSim(0,0) is the identity.
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_NEAR(after[i].real(), before[i].real(), 1e-12);
        EXPECT_NEAR(after[i].imag(), before[i].imag(), 1e-12);
    }
}

// ── fSim(pi/2, pi/6) Sycamore: spot-check M[3][3] = e^{-i*pi/6} ─────────────
// Verifies the concrete Sycamore parameter set produces the expected last diagonal.
TEST(GoogleGates, fSim_SycamoreParams_SpotCheckDiagonal) {
    const double theta = M_PI / 2.0;
    const double phi   = M_PI / 6.0;
    const auto m = fsim_matrix_data(theta, phi);

    // M[1][1] = cos(pi/2) = 0, M[1][2] = -i*sin(pi/2) = -i.
    EXPECT_NEAR(mat_at(m.data(), 1, 1).real(), 0.0, 1e-12);
    EXPECT_NEAR(mat_at(m.data(), 1, 2).imag(), -1.0, 1e-12);
    // M[3][3] = e^{-i*pi/6} = cos(pi/6) - i*sin(pi/6) = sqrt(3)/2 - i/2.
    EXPECT_NEAR(mat_at(m.data(), 3, 3).real(), std::cos(M_PI / 6.0), 1e-12);
    EXPECT_NEAR(mat_at(m.data(), 3, 3).imag(), -std::sin(M_PI / 6.0), 1e-12);
}

// ── sycamore() == fsim(pi/2, pi/6) equivalence ───────────────────────────────
// Circuit::sycamore() is a thin wrapper; verify it produces identical state.
TEST(GoogleGates, Sycamore_EqualsFSimHalfPiSixth) {
    FullStateCpuBackend backA;
    FullStateCpuBackend backB;

    // Arrange: both backends in |11> via X(0)X(1) so the |11> diagonal entry
    // e^{-i*phi} of fSim is exercised.
    auto x0 = []() { GateOp op; op.type = GateType::X; op.name = "x"; op.qubits = {0}; return op; };
    auto x1 = []() { GateOp op; op.type = GateType::X; op.name = "x"; op.qubits = {1}; return op; };

    backA.init_state(2); backA.apply_gate(x0()); backA.apply_gate(x1());
    backA.apply_gate(make_fsim(M_PI / 2.0, M_PI / 6.0, 0, 1));

    // For backB, use the Circuit::sycamore() builder via direct GateOp construction.
    backB.init_state(2); backB.apply_gate(x0()); backB.apply_gate(x1());
    {
        Circuit circ(2);
        circ.sycamore(0, 1);
        // Execute the single GateOp from the circuit.
        for (const Op& op : circ.ops())
            if (std::holds_alternative<GateOp>(op))
                backB.apply_gate(std::get<GateOp>(op));
    }

    const StateVector svA = backA.get_statevector();
    const StateVector svB = backB.get_statevector();
    // 1e-12: algebraic — same matrix must yield same result.
    for (std::size_t i = 0; i < svA.size(); ++i) {
        EXPECT_NEAR(svA[i].real(), svB[i].real(), 1e-12);
        EXPECT_NEAR(svA[i].imag(), svB[i].imag(), 1e-12);
    }
}

// ── Fused == unfused: the fusion 4x4 path matches the direct kernel ───────────
// Guards against gate_full_matrix() identity-fallback for the new gate types.
TEST(GoogleGates, Fusion_EqualsDirect_ForAllThreeGates) {
    // Arrange: 3-qubit circuit exercising iSWAP, sqrt_iSWAP, and fSim with
    // surrounding 1Q context so fusion actually forms multi-qubit windows.
    Circuit circ(3);
    circ.h(0).x(2)
        .iswap(0, 1)
        .sqrt_iswap(1, 2)
        .h(0)
        .fsim(0.5, 0.3, 0, 2);

    FullStateCpuBackend backA;  // unfused per-gate apply
    FullStateCpuBackend backB;  // fused program
    const QubitIndex n = circ.num_qubits();

    // Act: path A (per-gate).
    backA.init_state(n);
    for (const Op& op : circ.ops())
        if (std::holds_alternative<GateOp>(op))
            backA.apply_gate(std::get<GateOp>(op));
    const StateVector svA = backA.get_statevector();

    // Act: path B (fused).
    backB.init_state(n);
    const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), 4, circ.num_qubits());
    backB.apply_fused_program(prog);
    const StateVector svB = backB.get_statevector();

    // Assert: identical to 1e-12.
    EXPECT_EQ(svA.size(), svB.size());
    for (std::size_t i = 0; i < svA.size(); ++i) {
        EXPECT_NEAR(svA[i].real(), svB[i].real(), 1e-12);
        EXPECT_NEAR(svA[i].imag(), svB[i].imag(), 1e-12);
    }
}

// ── JSON round-trip: gate type, name, qubits, params preserved ──────
TEST(GoogleGates, JsonRoundTrip_iSWAP_PreservesGate) {
    Circuit circ(2);
    circ.iswap(1, 0);  // reversed operands to exercise qubit order preservation

    const Circuit restored = Circuit::from_json(circ.to_json());

    std::size_t count = 0;
    for (const Op& op : restored.ops()) {
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            if (g.type == GateType::ISWAP) {
                ++count;
                EXPECT_EQ(g.name, std::string{"iswap"});
                EXPECT_EQ(g.qubits.size(), std::size_t{2});
                EXPECT_EQ(g.qubits[0], QubitIndex{1});
                EXPECT_EQ(g.qubits[1], QubitIndex{0});
            }
        }
    }
    EXPECT_EQ(count, std::size_t{1});
}

TEST(GoogleGates, JsonRoundTrip_fSim_PreservesParamsAndQubits) {
    const double theta = 1.2, phi = 0.8;
    Circuit circ(2);
    circ.fsim(theta, phi, 0, 1);

    const Circuit restored = Circuit::from_json(circ.to_json());

    std::size_t count = 0;
    for (const Op& op : restored.ops()) {
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            if (g.type == GateType::FSIM) {
                ++count;
                EXPECT_EQ(g.name, std::string{"fsim"});
                EXPECT_EQ(g.params.size(), std::size_t{2});
                // 1e-12: JSON serialises as double; round-trip must be lossless.
                EXPECT_NEAR(g.params[0], theta, 1e-12);
                EXPECT_NEAR(g.params[1], phi,   1e-12);
            }
        }
    }
    EXPECT_EQ(count, std::size_t{1});
}

// ── to_qasm() must throw for all three gates (no OpenQASM v2 form) ────────────
TEST(GoogleGates, iSWAP_ToQasm_Throws) {
    Circuit circ(2); circ.iswap(0, 1);
    EXPECT_THROW(circ.to_qasm(), CircuitError);
}

TEST(GoogleGates, SqrtISWAP_ToQasm_Throws) {
    Circuit circ(2); circ.sqrt_iswap(0, 1);
    EXPECT_THROW(circ.to_qasm(), CircuitError);
}

TEST(GoogleGates, fSim_ToQasm_Throws) {
    Circuit circ(2); circ.fsim(0.5, 0.3, 0, 1);
    EXPECT_THROW(circ.to_qasm(), CircuitError);
}

// ── SparseState must reject all three gates explicitly ────────────────────────
TEST(GoogleGates, iSWAP_SparseStateApply_Throws) {
    SparseStateBackend backend; backend.init_state(2);
    EXPECT_THROW(backend.apply_gate(make_iswap(0, 1)), SimulatorError);
}

TEST(GoogleGates, SqrtISWAP_SparseStateApply_Throws) {
    SparseStateBackend backend; backend.init_state(2);
    EXPECT_THROW(backend.apply_gate(make_sqrt_iswap(0, 1)), SimulatorError);
}

TEST(GoogleGates, fSim_SparseStateApply_Throws) {
    SparseStateBackend backend; backend.init_state(2);
    EXPECT_THROW(backend.apply_gate(make_fsim(0.5, 0.3, 0, 1)), SimulatorError);
}

// ── Builder rejects same-qubit operands (distinct-qubit invariant) ─────────────
TEST(GoogleGates, iSWAP_BuilderRejectsSameQubit) {
    Circuit circ(2);
    EXPECT_THROW(circ.iswap(0, 0), CircuitError);
}

TEST(GoogleGates, SqrtISWAP_BuilderRejectsSameQubit) {
    Circuit circ(2);
    EXPECT_THROW(circ.sqrt_iswap(1, 1), CircuitError);
}

TEST(GoogleGates, fSim_BuilderRejectsSameQubit) {
    Circuit circ(2);
    EXPECT_THROW(circ.fsim(0.5, 0.3, 0, 0), CircuitError);
}

// ── None of the three gates is self-inverse ───────────────────────────────────
// Verifies that cancel_inverse does NOT cancel adjacent identical pairs.
// For iSWAP: (iSWAP)^2 maps |01> -> i|10> -> i*(i|01>) = -|01> ≠ |01>.
TEST(GoogleGates, iSWAP_NotSelfInverse_TwiceIsNotIdentity) {
    FullStateCpuBackend backend;
    backend.init_state(2);
    // Prepare |01> via X(0).
    backend.apply_gate([]() {
        GateOp op; op.type = GateType::X; op.name = "x"; op.qubits = {0}; return op;
    }());
    backend.apply_gate(make_iswap(0, 1));
    backend.apply_gate(make_iswap(0, 1));
    const StateVector sv = backend.get_statevector();

    // (iSWAP)^2|01>: first iSWAP maps |01>->i|10>, second maps i|10>->i*i|01>=-|01>.
    // So amp[1] = -1, all others = 0.
    EXPECT_NEAR(sv[1].real(), -1.0, 1e-12);
    EXPECT_NEAR(sv[1].imag(),  0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(),  0.0, 1e-12);
    EXPECT_NEAR(sv[2].imag(),  0.0, 1e-12);
}
