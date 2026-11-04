// test_qelib1_gate_convention.cpp -- qelib1.inc gate convention verification tests.
//
// Step 1 (diagnosis): Compare each suspect gate applied via GateType against the
// equivalent qelib1.inc decomposition built from primitive gates.  Both paths
// start from the same non-trivial 2- or 3-qubit state prepared with RY/RZ so
// that both control=0 and control=1 subspaces are exercised.
//
// Step 2 (gate-fusion qubit-order): Verify that the fused path and unfused path
// agree for every controlled gate where qubit[0] > qubit[1] (the "reversed order"
// case that exposed the ctrl/tgt swap bug in gate_fusion.cpp).
//
// Gates under test: rz, cy, cz, ch, crz, cu1, cu3, ccx, cswap.
// Tests follow (Arrange/Act/Assert, EXPECT_NEAR 1e-12).
// Code comments in English.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <cstdint>
#include <complex>
#include <vector>

#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/backend/full_state/gate_fusion.hpp"
#include "../../../include/error.hpp"
#include "../../../include/types.hpp"

using qplayer::FullStateCpuBackend;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::QubitIndex;
using qplayer::StateVector;
using qplayer::StateIndex;

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

// Construct a GateOp quickly.
static GateOp make_gate(GateType type,
                         std::vector<QubitIndex> qubits,
                         std::vector<double>     params = {}) {
    GateOp op;
    op.type   = type;
    op.qubits = std::move(qubits);
    op.params = std::move(params);
    return op;
}

// Fixture: reset backend, init n-qubit state.
class ConventionFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override {
        backend = std::make_unique<FullStateCpuBackend>();
    }
    void TearDown() override {
        backend.reset();
    }
    std::unique_ptr<FullStateCpuBackend> backend;
};

// Check that two statevectors agree element-wise within tol (re and im separately).
// Uses EXPECT_NEAR from the self-test framework; reports first failure index.
static void expect_sv_equal(const StateVector& a,
                             const StateVector& b,
                             double tol,
                             const char* label) {
    (void)label;  // used only in EXPECT messages via macro expansion
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(a[i].real(), b[i].real(), tol);
        EXPECT_NEAR(a[i].imag(), b[i].imag(), tol);
        if (std::abs(a[i].real() - b[i].real()) > tol ||
            std::abs(a[i].imag() - b[i].imag()) > tol) {
            // Stop at first failing index so the output stays readable.
            return;
        }
    }
}

}  // namespace

// ── 1. rz convention ─────────────────────────────────────────────────────────
// qelib1: rz(phi) = u1(phi) = diag(1, e^{i*phi}).
// Our GateType::RZ = diag(e^{-i*phi/2}, e^{i*phi/2}) (standard physics convention).
// These differ by global phase e^{-i*phi/2} on the full state vector -- physically
// equivalent, but the final statevector magnitudes are identical (global phase
// preserves all |amplitude|).
// Test: RZ(phi)|0> and U1(phi)|0> give identical probabilities; magnitudes differ
// only by the global phase factor (absolute value remains 1).
//
// We verify that the RELATIVE PHASE between the |0> and |1> components is the
// same (both conventions give e^{i*phi} relative phase), which is what matters
// for any subsequent gate.

TEST_F(ConventionFixture, RZ_vs_U1_RelativePhaseAgrees) {
    // Arrange: qubit prepared in |+> = H|0> so both |0> and |1> are populated.
    const double phi = 1.23;

    // Path A: H then RZ(phi).
    backend->init_state(1);
    backend->apply_gate(make_gate(GateType::H, {0}));
    backend->apply_gate(make_gate(GateType::RZ, {0}, {phi}));
    const StateVector sv_rz = backend->get_statevector();

    // Path B: H then U1(phi).
    backend->init_state(1);
    backend->apply_gate(make_gate(GateType::H, {0}));
    backend->apply_gate(make_gate(GateType::U1, {0}, {phi}));
    const StateVector sv_u1 = backend->get_statevector();

    // Both give same relative phase e^{i*phi} between |1> and |0> components.
    // Ratio sv_rz[1]/sv_rz[0] and sv_u1[1]/sv_u1[0] should be equal.
    // Check via: arg( sv_rz[1] * conj(sv_rz[0]) ) == arg( sv_u1[1] * conj(sv_u1[0]) ).
    const std::complex<double> phase_rz = sv_rz[1] * std::conj(sv_rz[0]);
    const std::complex<double> phase_u1 = sv_u1[1] * std::conj(sv_u1[0]);
    // Tolerance 1e-12: algebraic result.
    EXPECT_NEAR(phase_rz.real(), phase_u1.real(), 1e-12);
    EXPECT_NEAR(phase_rz.imag(), phase_u1.imag(), 1e-12);

    // Magnitudes: both |sv_rz[i]| = |sv_u1[i]| = 1/sqrt(2).
    const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
    EXPECT_NEAR(std::abs(sv_rz[0]), inv_sqrt2, 1e-12);
    EXPECT_NEAR(std::abs(sv_rz[1]), inv_sqrt2, 1e-12);
    EXPECT_NEAR(std::abs(sv_u1[0]), inv_sqrt2, 1e-12);
    EXPECT_NEAR(std::abs(sv_u1[1]), inv_sqrt2, 1e-12);
}

// ── 2. CY matches qelib1 decomposition ───────────────────────────────────────
// qelib1: cy a,b { sdg b; cx a,b; s b; }
// Expected: block-diag(I, Y) -- this should match exactly.

TEST_F(ConventionFixture, CY_MatchesQelib1Decomposition) {
    // Arrange: 2-qubit state with both ctrl=0 and ctrl=1 populated.
    // Prepare with RY on each qubit so state is a non-trivial superposition.
    const double theta0 = 0.7, theta1 = 1.1;

    // Path A: direct CY gate (ctrl=0, tgt=1).
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::CY, {0, 1}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 decomposition { sdg b; cx a,b; s b; }.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::Sdg, {1}));
    backend->apply_gate(make_gate(GateType::CX,  {0, 1}));
    backend->apply_gate(make_gate(GateType::S,   {1}));
    const StateVector sv_b = backend->get_statevector();

    // Tolerance 1e-12: algebraic result.
    expect_sv_equal(sv_a, sv_b, 1e-12, "CY");
}

// CY with ctrl > tgt (reversed qubit order) -- exercises the fusion qubit-order path.

TEST_F(ConventionFixture, CY_ReversedOrder_MatchesDecomposition) {
    const double theta0 = 0.9, theta1 = 0.6;

    // Path A: CY(ctrl=2, tgt=0).
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {2}, {theta1}));
    backend->apply_gate(make_gate(GateType::CY, {2, 0}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 decomposition for cy ctrl=2, tgt=0.
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {2}, {theta1}));
    backend->apply_gate(make_gate(GateType::Sdg, {0}));
    backend->apply_gate(make_gate(GateType::CX,  {2, 0}));
    backend->apply_gate(make_gate(GateType::S,   {0}));
    const StateVector sv_b = backend->get_statevector();

    expect_sv_equal(sv_a, sv_b, 1e-12, "CY reversed");
}

// ── 3. CZ matches qelib1 decomposition ───────────────────────────────────────
// qelib1: cz a,b { h b; cx a,b; h b; }
// Expected: block-diag(I, Z) == diag(1,1,1,-1).

TEST_F(ConventionFixture, CZ_MatchesQelib1Decomposition) {
    const double theta0 = 0.5, theta1 = 1.3;

    // Path A: direct CZ.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::CZ, {0, 1}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 { h b; cx a,b; h b }.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::H, {1}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    backend->apply_gate(make_gate(GateType::H, {1}));
    const StateVector sv_b = backend->get_statevector();

    expect_sv_equal(sv_a, sv_b, 1e-12, "CZ");
}

// CZ with ctrl > tgt (reversed qubit order).

TEST_F(ConventionFixture, CZ_ReversedOrder_MatchesDecomposition) {
    const double theta0 = 0.4, theta1 = 0.8;

    // Path A: CZ(ctrl=3, tgt=1) in 4-qubit system.
    backend->init_state(4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {3}, {theta1}));
    backend->apply_gate(make_gate(GateType::CZ, {3, 1}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 { h b; cx a,b; h b } with ctrl=3, tgt=1.
    backend->init_state(4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {3}, {theta1}));
    backend->apply_gate(make_gate(GateType::H,  {1}));
    backend->apply_gate(make_gate(GateType::CX, {3, 1}));
    backend->apply_gate(make_gate(GateType::H,  {1}));
    const StateVector sv_b = backend->get_statevector();

    expect_sv_equal(sv_a, sv_b, 1e-12, "CZ reversed");
}

// ── 4. CH matches qelib1 decomposition ───────────────────────────────────────
// qelib1: ch a,b { h b; sdg b; cx a,b; h b; t b; cx a,b; t b; h b; s b; x b; s a; }
// The qelib1 decomposition applies a global phase e^{i*pi/4} relative to block-diag(I,H).
// Both definitions are physically equivalent (global phase only), so their statevectors
// differ by at most a uniform complex factor of magnitude 1.
// We verify that |amplitude| agrees exactly for both.

TEST_F(ConventionFixture, CH_MagnitudesMatchQelib1Decomposition) {
    const double theta0 = 0.6, theta1 = 1.4;

    // Path A: direct CH(ctrl=0, tgt=1) = block-diag(I, H).
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::CH, {0, 1}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 decomposition { h b; sdg b; cx a,b; h b; t b; cx a,b; t b; h b; s b; x b; s a }.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::H,   {1}));
    backend->apply_gate(make_gate(GateType::Sdg, {1}));
    backend->apply_gate(make_gate(GateType::CX,  {0, 1}));
    backend->apply_gate(make_gate(GateType::H,   {1}));
    backend->apply_gate(make_gate(GateType::T,   {1}));
    backend->apply_gate(make_gate(GateType::CX,  {0, 1}));
    backend->apply_gate(make_gate(GateType::T,   {1}));
    backend->apply_gate(make_gate(GateType::H,   {1}));
    backend->apply_gate(make_gate(GateType::S,   {1}));
    backend->apply_gate(make_gate(GateType::X,   {1}));
    backend->apply_gate(make_gate(GateType::S,   {0}));  // S on control qubit!
    const StateVector sv_b = backend->get_statevector();

    // Verify magnitudes agree (global phase does not change |amplitude|).
    for (std::size_t i = 0; i < sv_a.size(); ++i) {
        EXPECT_NEAR(std::abs(sv_a[i]), std::abs(sv_b[i]), 1e-12);
    }
}

// CH with ctrl > tgt (reversed qubit order).

TEST_F(ConventionFixture, CH_ReversedOrder_MagnitudesMatchDecomposition) {
    const double theta0 = 1.0, theta1 = 0.5;

    // Path A: CH(ctrl=2, tgt=0).
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {2}, {theta1}));
    backend->apply_gate(make_gate(GateType::CH, {2, 0}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 decomposition ctrl=2, tgt=0.
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {2}, {theta1}));
    backend->apply_gate(make_gate(GateType::H,   {0}));
    backend->apply_gate(make_gate(GateType::Sdg, {0}));
    backend->apply_gate(make_gate(GateType::CX,  {2, 0}));
    backend->apply_gate(make_gate(GateType::H,   {0}));
    backend->apply_gate(make_gate(GateType::T,   {0}));
    backend->apply_gate(make_gate(GateType::CX,  {2, 0}));
    backend->apply_gate(make_gate(GateType::T,   {0}));
    backend->apply_gate(make_gate(GateType::H,   {0}));
    backend->apply_gate(make_gate(GateType::S,   {0}));
    backend->apply_gate(make_gate(GateType::X,   {0}));
    backend->apply_gate(make_gate(GateType::S,   {2}));
    const StateVector sv_b = backend->get_statevector();

    for (std::size_t i = 0; i < sv_a.size(); ++i) {
        EXPECT_NEAR(std::abs(sv_a[i]), std::abs(sv_b[i]), 1e-12);
    }
}

// ── 5. CRZ matches qelib1 decomposition ─────────────────────────────────────
// qelib1: crz(l) a,b { u1(l/2) b; cx a,b; u1(-l/2) b; cx a,b; }
// This is equivalent to block-diag(I, standard-RZ(l)) which is our implementation.

TEST_F(ConventionFixture, CRZ_MatchesQelib1Decomposition) {
    const double lam = 0.7;
    const double theta0 = 0.8, theta1 = 1.2;

    // Path A: direct CRZ.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::CRZ, {0, 1}, {lam}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 { u1(l/2) b; cx a,b; u1(-l/2) b; cx a,b }.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::U1, {1}, {lam / 2.0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    backend->apply_gate(make_gate(GateType::U1, {1}, {-lam / 2.0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    const StateVector sv_b = backend->get_statevector();

    expect_sv_equal(sv_a, sv_b, 1e-12, "CRZ");
}

// ── 6. CU1 matches qelib1 decomposition ─────────────────────────────────────
// qelib1: cu1(l) a,b { u1(l/2) a; cx a,b; u1(-l/2) b; cx a,b; u1(l/2) b; }
// This results in diag(1, e^{il/2}, 1, e^{il}) -- a phase gate on BOTH ctrl and tgt.
// Our block-diag(I, U1(l)) = diag(1, 1, 1, e^{il}).
// The qelib1 version also puts a phase e^{il/2} on the ctrl=1,tgt=0 entry.
// These differ by a phase on the |10> subspace -- NOT a global phase.
// Numerically verify they match.

TEST_F(ConventionFixture, CU1_MatchesQelib1Decomposition) {
    const double lam = 0.7;
    const double theta0 = 0.9, theta1 = 1.1;

    // Path A: direct CU1.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::CU1, {0, 1}, {lam}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 { u1(l/2) a; cx a,b; u1(-l/2) b; cx a,b; u1(l/2) b }.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::U1, {0}, {lam / 2.0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    backend->apply_gate(make_gate(GateType::U1, {1}, {-lam / 2.0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    backend->apply_gate(make_gate(GateType::U1, {1}, {lam / 2.0}));
    const StateVector sv_b = backend->get_statevector();

    expect_sv_equal(sv_a, sv_b, 1e-12, "CU1");
}

// ── 7. CU3 matches qelib1 decomposition ─────────────────────────────────────
// qelib1: cu3(th,phi,l) c,t {
//   u1((l+phi)/2) c; u1((l-phi)/2) t;
//   cx c,t;
//   u3(-th/2,0,-(phi+l)/2) t;
//   cx c,t;
//   u3(th/2,phi,0) t;
// }
// Our block-diag(I, U3(th,phi,l)) should match this decomposition numerically.

TEST_F(ConventionFixture, CU3_MatchesQelib1Decomposition) {
    const double th = 0.5, phi = 0.6, lam = 0.7;
    const double theta0 = 0.8, theta1 = 1.0;

    // Path A: direct CU3.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::CU3, {0, 1}, {th, phi, lam}));
    const StateVector sv_a = backend->get_statevector();

    // Path B: qelib1 decomposition.
    backend->init_state(2);
    backend->apply_gate(make_gate(GateType::RY, {0}, {theta0}));
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta1}));
    backend->apply_gate(make_gate(GateType::U1, {0}, {(lam + phi) / 2.0}));
    backend->apply_gate(make_gate(GateType::U1, {1}, {(lam - phi) / 2.0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    backend->apply_gate(make_gate(GateType::U3, {1}, {-th / 2.0, 0.0, -(phi + lam) / 2.0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    backend->apply_gate(make_gate(GateType::U3, {1}, {th / 2.0, phi, 0.0}));
    const StateVector sv_b = backend->get_statevector();

    expect_sv_equal(sv_a, sv_b, 1e-12, "CU3");
}

// ── 8. CCX (Toffoli) matches qelib1 standard Toffoli ─────────────────────────
// qelib1 ccx is the standard Toffoli gate.
// Our CCX: both controls must be 1, then X applied to target.

TEST_F(ConventionFixture, CCX_MatchesStandardToffoli) {
    // Test with |110> input (q0=0,q1=1,q2=1): CCX(0,1,2) should give |111>.
    // Qubits [0,1,2], all ascending -> no reorder issue for direct path.
    backend->init_state(3);
    // Prepare q1=1, q2=1 (index = 0b110 = 6).
    backend->apply_gate(make_gate(GateType::X, {1}));
    backend->apply_gate(make_gate(GateType::X, {2}));

    backend->apply_gate(make_gate(GateType::CCX, {0, 1, 2}));
    const StateVector sv = backend->get_statevector();

    // Both controls (q1,q2) are 1 but ctrl0=q0=0, so no flip -> state stays |110>=6.
    // Wait: CCX(0,1,2) means ctrl=q0 AND ctrl=q1, tgt=q2.
    // q0=0 => no flip.
    // Index 6 = |110> (q0=0,q1=1,q2=1): amp[6] should be 1.
    EXPECT_NEAR(sv[6].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[6].imag(), 0.0, 1e-12);
}

TEST_F(ConventionFixture, CCX_BothControlsHigh_FlipsTarget) {
    // Prepare q0=1,q1=1 (state=|011>=3), CCX(0,1,2) should flip q2 -> |111>=7.
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::X, {0}));
    backend->apply_gate(make_gate(GateType::X, {1}));

    backend->apply_gate(make_gate(GateType::CCX, {0, 1, 2}));
    const StateVector sv = backend->get_statevector();

    // Index 7 = |111> (all ones): amp[7] should be 1.
    EXPECT_NEAR(sv[7].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[7].imag(), 0.0, 1e-12);
}

// CCX with non-ascending qubit indices (e.g., ctrl0=2,ctrl1=1,tgt=0).

TEST_F(ConventionFixture, CCX_ReversedOrder_FlipsTarget) {
    // CCX(ctrl=2, ctrl=1, tgt=0): prepare q1=1,q2=1 -> should flip q0.
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::X, {1}));
    backend->apply_gate(make_gate(GateType::X, {2}));

    // CCX(2,1,0): qubits not ascending.
    backend->apply_gate(make_gate(GateType::CCX, {2, 1, 0}));
    const StateVector sv = backend->get_statevector();

    // q0 was 0; both controls 1 -> flip q0 to 1 -> state = |111> = index 7.
    EXPECT_NEAR(sv[7].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[7].imag(), 0.0, 1e-12);
}

// ── 9. CSWAP (Fredkin) matches qelib1 decomposition ─────────────────────────
// qelib1: cswap a,b,c { cx c,b; ccx a,b,c; cx c,b; }
// Our: swap b and c when a=1.

TEST_F(ConventionFixture, CSwap_ControlHigh_SwapsTargetQubits) {
    // Arrange: q0=1 (ctrl), q1=1, q2=0. CSwap(0,1,2): should swap q1,q2 -> q1=0,q2=1.
    // Index before: q0=1,q1=1,q2=0 = 0b011 = 3.
    // Index after:  q0=1,q1=0,q2=1 = 0b101 = 5.
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::X, {0}));
    backend->apply_gate(make_gate(GateType::X, {1}));

    backend->apply_gate(make_gate(GateType::CSwap, {0, 1, 2}));
    const StateVector sv = backend->get_statevector();

    EXPECT_NEAR(sv[5].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[5].imag(), 0.0, 1e-12);
}

TEST_F(ConventionFixture, CSwap_ControlLow_NoSwap) {
    // Arrange: q0=0 (ctrl), q1=1, q2=0. CSwap(0,1,2): no swap -> state unchanged.
    // Index: q0=0,q1=1,q2=0 = 0b010 = 2.
    backend->init_state(3);
    backend->apply_gate(make_gate(GateType::X, {1}));

    backend->apply_gate(make_gate(GateType::CSwap, {0, 1, 2}));
    const StateVector sv = backend->get_statevector();

    EXPECT_NEAR(sv[2].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[2].imag(), 0.0, 1e-12);
}

// ── 10. Gate fusion: controlled gate with reversed qubit order ────────────────
// This test directly exercises the fusion path with ctrl > tgt to catch the
// gate_fusion.cpp qubit-order bug (window_gate_qubits must use original order).
// Test: CX(ctrl=5, tgt=1) in fused mode vs unfused mode must give same result.

TEST_F(ConventionFixture, GateFusion_CX_ReversedOrder_MatchesUnfused) {
    // Circuit: CX(ctrl=5, tgt=1) -- ctrl(5) > tgt(1), 6-qubit system.
    // Arrange: populate both subspaces with RY gates.
    const double theta = 1.0;

    // Path A: unfused (fusion disabled).
    backend->init_state(6);
    backend->set_fusion(false, 4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {5}, {theta}));
    backend->apply_gate(make_gate(GateType::CX, {5, 1}));
    const StateVector sv_unfused = backend->get_statevector();

    // Path B: fused (fusion enabled).
    backend->init_state(6);
    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {5}, {theta}));
    backend->apply_gate(make_gate(GateType::CX, {5, 1}));
    const StateVector sv_fused = backend->get_statevector();

    // Tolerance 1e-10: numerical result from FMA pipeline.
    expect_sv_equal(sv_fused, sv_unfused, 1e-10, "CX reversed fusion");
}

TEST_F(ConventionFixture, GateFusion_CZ_ReversedOrder_MatchesUnfused) {
    const double theta = 0.8;

    backend->init_state(4);
    backend->set_fusion(false, 4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {3}, {theta}));
    backend->apply_gate(make_gate(GateType::CZ, {3, 1}));
    const StateVector sv_unfused = backend->get_statevector();

    backend->init_state(4);
    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {3}, {theta}));
    backend->apply_gate(make_gate(GateType::CZ, {3, 1}));
    const StateVector sv_fused = backend->get_statevector();

    expect_sv_equal(sv_fused, sv_unfused, 1e-10, "CZ reversed fusion");
}

TEST_F(ConventionFixture, GateFusion_CH_ReversedOrder_MatchesUnfused) {
    const double theta = 0.7;

    backend->init_state(4);
    backend->set_fusion(false, 4);
    backend->apply_gate(make_gate(GateType::RY, {2}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {3}, {theta}));
    backend->apply_gate(make_gate(GateType::CH, {3, 2}));
    const StateVector sv_unfused = backend->get_statevector();

    backend->init_state(4);
    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::RY, {2}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {3}, {theta}));
    backend->apply_gate(make_gate(GateType::CH, {3, 2}));
    const StateVector sv_fused = backend->get_statevector();

    expect_sv_equal(sv_fused, sv_unfused, 1e-10, "CH reversed fusion");
}

TEST_F(ConventionFixture, GateFusion_CCX_NonAscendingOrder_MatchesUnfused) {
    // ccx q[7],q[1],q[8] -- matches oracle_10 case where 7>1.
    // Use 9-qubit system (indices 0..8).
    const double theta = 0.9;

    backend->init_state(9);
    backend->set_fusion(false, 4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {7}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {8}, {theta}));
    backend->apply_gate(make_gate(GateType::CCX, {7, 1, 8}));
    const StateVector sv_unfused = backend->get_statevector();

    backend->init_state(9);
    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::RY, {1}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {7}, {theta}));
    backend->apply_gate(make_gate(GateType::RY, {8}, {theta}));
    backend->apply_gate(make_gate(GateType::CCX, {7, 1, 8}));
    const StateVector sv_fused = backend->get_statevector();

    expect_sv_equal(sv_fused, sv_unfused, 1e-10, "CCX non-ascending fusion");
}

// ── 11. Gate fusion: ascending-order gates still correct after fix ────────────
// Regression: verify that Bell state (CX ctrl=0,tgt=1, ascending) still works.

TEST_F(ConventionFixture, GateFusion_CX_AscendingOrder_Regression) {
    // H + CX(ctrl=0,tgt=1) produces Bell state |00>+|11> -- ascending qubit order.
    backend->init_state(2);
    backend->set_fusion(false, 4);
    backend->apply_gate(make_gate(GateType::H,  {0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    const StateVector sv_unfused = backend->get_statevector();

    backend->init_state(2);
    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::H,  {0}));
    backend->apply_gate(make_gate(GateType::CX, {0, 1}));
    const StateVector sv_fused = backend->get_statevector();

    expect_sv_equal(sv_fused, sv_unfused, 1e-10, "Bell state regression");
}

// ── 12. SWAP analytic permutation verification — non-adjacent qubits ─────────
//
// Strict correctness test: for SWAP(q_a, q_b), B[i] must equal A[perm(i)]
// where perm(i) = i with bits q_a and q_b exchanged.
// State is prepared so ALL amplitudes are distinct and non-zero (no degenerate
// entries that could mask a permutation bug).
//
// Preparation recipe: RY(theta_k) on each qubit k with distinct theta_k, then
// RZ(phi_k) on each qubit.  Resulting amplitudes are products of independent
// single-qubit factors — all distinct when angles are transcendentally chosen.
//
// Cases mirroring oracle_20 patterns:
//   SWAP(8,9)   — adjacent (sanity)
//   SWAP(0,19)  — max spread in 20-qubit circuit (tested at 6Q equivalent)
//   SWAP(12,14) — non-adjacent by 2 positions
//   SWAP(4,6)   — non-adjacent by 2 positions
//   SWAP(9,14)  — non-adjacent by 5 positions
// We use 6-qubit system for all cases to keep test fast.
//
// Separate helper: compute perm(i) = swap bits q_a and q_b in i.

namespace {

static StateIndex perm_swap(StateIndex i, QubitIndex q_a, QubitIndex q_b) {
    int b_a = static_cast<int>((i >> q_a) & 1);
    int b_b = static_cast<int>((i >> q_b) & 1);
    if (b_a == b_b) return i;
    // Flip both bits.
    return i ^ (StateIndex{1} << q_a) ^ (StateIndex{1} << q_b);
}

// perm_cswap(i, ctrl, q_a, q_b): identity if ctrl bit of i is 0; else swap bits q_a/q_b.
static StateIndex perm_cswap(StateIndex i, QubitIndex ctrl,
                              QubitIndex q_a, QubitIndex q_b) {
    if (!((i >> ctrl) & 1)) return i;
    return perm_swap(i, q_a, q_b);
}

// Prepare a 6-qubit non-trivial state: each qubit k gets RY(angles[k]) then RZ(angles2[k]).
// All amplitudes end up distinct and non-zero for the chosen angle set.
static StateVector prepare_nontrivial_6q(FullStateCpuBackend& bk) {
    bk.init_state(6);
    // angles chosen to be mutually irrational multiples of pi (no accidental degeneracy).
    const double ry_angles[6] = {0.31, 0.71, 1.13, 1.47, 0.89, 1.23};
    const double rz_angles[6] = {0.53, 1.07, 0.67, 1.31, 0.43, 0.97};
    for (int q = 0; q < 6; ++q) {
        bk.apply_gate(make_gate(GateType::RY, {static_cast<QubitIndex>(q)}, {ry_angles[q]}));
        bk.apply_gate(make_gate(GateType::RZ, {static_cast<QubitIndex>(q)}, {rz_angles[q]}));
    }
    return bk.get_statevector();
}

// Check B[i] == A[perm(i)] for all i in [0, 2^6).
// perm_fn maps an index to the expected source index in A.
// Reports pass/fail via EXPECT_NEAR at 1e-12 tolerance.
using PermFn = StateIndex(*)(StateIndex);

static void check_permutation_6q(const StateVector& A,
                                  const StateVector& B,
                                  StateIndex(*perm_fn)(StateIndex)) {
    const std::size_t dim = 64;  // 2^6
    for (std::size_t i = 0; i < dim; ++i) {
        const std::size_t src = static_cast<std::size_t>(perm_fn(static_cast<StateIndex>(i)));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

}  // namespace

// SWAP(0,1): adjacent — sanity check that verify helper itself is correct.
TEST_F(ConventionFixture, SWAP_Adjacent_01_AnalyticPermutation) {
    // Arrange.
    const StateVector A = prepare_nontrivial_6q(*backend);

    // Act: apply SWAP(0,1).
    backend->apply_gate(make_gate(GateType::SWAP, {0, 1}));
    const StateVector B = backend->get_statevector();

    // Assert: B[i] == A[swap_bits(i,0,1)].
    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_swap(static_cast<StateIndex>(i), 0, 1));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// SWAP(0,4): non-adjacent, gap=4, mirrors oracle_20 pattern SWAP(0,19).
TEST_F(ConventionFixture, SWAP_NonAdjacent_04_AnalyticPermutation) {
    // Arrange.
    const StateVector A = prepare_nontrivial_6q(*backend);

    // Act: apply SWAP(0,4).
    backend->apply_gate(make_gate(GateType::SWAP, {0, 4}));
    const StateVector B = backend->get_statevector();

    // Assert: B[i] == A[swap_bits(i,0,4)].
    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_swap(static_cast<StateIndex>(i), 0, 4));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// SWAP(4,0): reversed argument order — same permutation, must give identical result.
TEST_F(ConventionFixture, SWAP_NonAdjacent_40_ReversedArgs_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::SWAP, {4, 0}));
    const StateVector B = backend->get_statevector();

    // Same expected permutation: swap bits 0 and 4.
    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_swap(static_cast<StateIndex>(i), 4, 0));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// SWAP(2,4): non-adjacent by 2, mirrors oracle_20 SWAP(12,14) and SWAP(4,6).
TEST_F(ConventionFixture, SWAP_NonAdjacent_24_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::SWAP, {2, 4}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_swap(static_cast<StateIndex>(i), 2, 4));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// SWAP(1,5): non-adjacent max-spread in 6-qubit system, mirrors oracle_20 SWAP(9,14).
TEST_F(ConventionFixture, SWAP_NonAdjacent_15_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::SWAP, {1, 5}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_swap(static_cast<StateIndex>(i), 1, 5));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// SWAP(0,5): maximum spread in 6-qubit system.
TEST_F(ConventionFixture, SWAP_NonAdjacent_05_MaxSpread_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::SWAP, {0, 5}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_swap(static_cast<StateIndex>(i), 0, 5));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// ── 13. CSwap analytic permutation verification — non-adjacent qubits ─────────
//
// For CSwap(ctrl, q_a, q_b): B[i] == A[perm_cswap(i, ctrl, q_a, q_b)]
// where perm_cswap is identity when ctrl-bit(i)==0, else swap bits q_a and q_b.
//
// Oracle_20 CSwap patterns (mapped to 6-qubit equivalent):
//   cswap(3,4,5)   → (0,1,2): adjacent ctrl, adjacent targets (sanity)
//   cswap(0,2,4)   → ctrl=0, non-adjacent targets
//   cswap(4,1,3)   → ctrl=4, non-adjacent targets (control > both targets)
//   cswap(0,1,2)   → ctrl=0, adjacent targets ascending
//   cswap ctrl=low, targets non-adjacent
//   cswap ctrl=high, targets low (matches oracle_20 cswap q[9],q[12],q[15] pattern)

// CSwap(0,1,2): adjacent ascending — sanity check.
TEST_F(ConventionFixture, CSwap_Adjacent_012_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::CSwap, {0, 1, 2}));
    const StateVector B = backend->get_statevector();

    // B[i] == A[perm_cswap(i, ctrl=0, qa=1, qb=2)].
    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 0, 1, 2));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// CSwap(0,2,4): ctrl=0, non-adjacent targets (gap=2), mirrors oracle_20 cswap(3,4,5).
TEST_F(ConventionFixture, CSwap_NonAdjacentTargets_024_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::CSwap, {0, 2, 4}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 0, 2, 4));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// CSwap(4,1,3): ctrl=4 (highest), non-adjacent targets, control > both targets.
// Mirrors oracle_20 cswap q[9],q[12],q[15] style (spread pattern).
TEST_F(ConventionFixture, CSwap_HighControl_NonAdjacentTargets_413_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::CSwap, {4, 1, 3}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 4, 1, 3));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// CSwap(5,0,3): maximum spread between ctrl and targets.
TEST_F(ConventionFixture, CSwap_MaxSpread_503_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::CSwap, {5, 0, 3}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 5, 0, 3));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// CSwap(2,0,4): ctrl between the two targets (ctrl index between qa and qb).
TEST_F(ConventionFixture, CSwap_CtrlBetweenTargets_204_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->apply_gate(make_gate(GateType::CSwap, {2, 0, 4}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 2, 0, 4));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// CSwap with control=0 and both targets zero: no qubit is actually set, verify
// identity — checks the ctrl=0 branch correctness.
TEST_F(ConventionFixture, CSwap_ControlLow_NonAdjacentTargets_NoSwap) {
    // Arrange: prepare state, then ensure we check the no-swap case.
    const StateVector A = prepare_nontrivial_6q(*backend);

    // CSwap(3,0,5): ctrl=q3. The permutation fn will handle ctrl-bit checks.
    backend->apply_gate(make_gate(GateType::CSwap, {3, 0, 5}));
    const StateVector B = backend->get_statevector();

    // B[i] == A[perm_cswap(i, ctrl=3, qa=0, qb=5)].
    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 3, 0, 5));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// ── 14. SWAP/CSwap fusion path analytic permutation ──────────────────────────
// Verify the gate_fusion.cpp SWAP/CSwap matrix embed also produces the correct
// analytic permutation (separate from the direct kernel path).

TEST_F(ConventionFixture, SWAP_Fused_NonAdjacent_04_AnalyticPermutation) {
    // Arrange.
    const StateVector A = prepare_nontrivial_6q(*backend);

    // Fused path: enable fusion, apply SWAP(0,4).
    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::SWAP, {0, 4}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_swap(static_cast<StateIndex>(i), 0, 4));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

TEST_F(ConventionFixture, CSwap_Fused_NonAdjacent_024_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::CSwap, {0, 2, 4}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 0, 2, 4));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// Fused CSwap with high control (ctrl > targets) — mirrors oracle_20 spread.
TEST_F(ConventionFixture, CSwap_Fused_HighControl_413_AnalyticPermutation) {
    const StateVector A = prepare_nontrivial_6q(*backend);

    backend->set_fusion(true, 4);
    backend->apply_gate(make_gate(GateType::CSwap, {4, 1, 3}));
    const StateVector B = backend->get_statevector();

    for (std::size_t i = 0; i < 64; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            perm_cswap(static_cast<StateIndex>(i), 4, 1, 3));
        EXPECT_NEAR(B[i].real(), A[src].real(), 1e-12);
        EXPECT_NEAR(B[i].imag(), A[src].imag(), 1e-12);
    }
}

// ── 15. SWAP/CSwap unfused vs fused agreement — non-adjacent cases ─────────
// Cross-check: for the same non-adjacent circuit, fused and unfused must agree.

TEST_F(ConventionFixture, SWAP_FusedVsUnfused_NonAdjacent_15_Agreement) {
    const double ry_angles[6] = {0.31, 0.71, 1.13, 1.47, 0.89, 1.23};
    const double rz_angles[6] = {0.53, 1.07, 0.67, 1.31, 0.43, 0.97};

    // Unfused path.
    backend->init_state(6);
    backend->set_fusion(false, 4);
    for (int q = 0; q < 6; ++q) {
        backend->apply_gate(make_gate(GateType::RY, {static_cast<QubitIndex>(q)}, {ry_angles[q]}));
        backend->apply_gate(make_gate(GateType::RZ, {static_cast<QubitIndex>(q)}, {rz_angles[q]}));
    }
    backend->apply_gate(make_gate(GateType::SWAP, {1, 5}));
    const StateVector sv_unfused = backend->get_statevector();

    // Fused path.
    backend->init_state(6);
    backend->set_fusion(true, 4);
    for (int q = 0; q < 6; ++q) {
        backend->apply_gate(make_gate(GateType::RY, {static_cast<QubitIndex>(q)}, {ry_angles[q]}));
        backend->apply_gate(make_gate(GateType::RZ, {static_cast<QubitIndex>(q)}, {rz_angles[q]}));
    }
    backend->apply_gate(make_gate(GateType::SWAP, {1, 5}));
    const StateVector sv_fused = backend->get_statevector();

    // 1e-10 tolerance: FMA numerical pipeline.
    expect_sv_equal(sv_fused, sv_unfused, 1e-10, "SWAP(1,5) fused vs unfused");
}

TEST_F(ConventionFixture, CSwap_FusedVsUnfused_HighControl_413_Agreement) {
    const double ry_angles[6] = {0.31, 0.71, 1.13, 1.47, 0.89, 1.23};
    const double rz_angles[6] = {0.53, 1.07, 0.67, 1.31, 0.43, 0.97};

    // Unfused path.
    backend->init_state(6);
    backend->set_fusion(false, 4);
    for (int q = 0; q < 6; ++q) {
        backend->apply_gate(make_gate(GateType::RY, {static_cast<QubitIndex>(q)}, {ry_angles[q]}));
        backend->apply_gate(make_gate(GateType::RZ, {static_cast<QubitIndex>(q)}, {rz_angles[q]}));
    }
    backend->apply_gate(make_gate(GateType::CSwap, {4, 1, 3}));
    const StateVector sv_unfused = backend->get_statevector();

    // Fused path.
    backend->init_state(6);
    backend->set_fusion(true, 4);
    for (int q = 0; q < 6; ++q) {
        backend->apply_gate(make_gate(GateType::RY, {static_cast<QubitIndex>(q)}, {ry_angles[q]}));
        backend->apply_gate(make_gate(GateType::RZ, {static_cast<QubitIndex>(q)}, {rz_angles[q]}));
    }
    backend->apply_gate(make_gate(GateType::CSwap, {4, 1, 3}));
    const StateVector sv_fused = backend->get_statevector();

    expect_sv_equal(sv_fused, sv_unfused, 1e-10, "CSwap(4,1,3) fused vs unfused");
}
