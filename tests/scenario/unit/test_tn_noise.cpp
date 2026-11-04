// test_tn_noise.cpp — Unit tests for Kraus operator application on an MPS chain
// 1.1, 1.3 (Arrange/Act/Assert).
//
// The contract under test is narrow but easy to break: a Kraus operator is not
// unitary, and ||K psi|| is the branch probability the trajectory loop
// will read. So every test here that touches the norm is guarding a semantic,
// not a number.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "../../../include/backend/tensor_network/mps.hpp"
#include "../../../include/backend/tensor_network/tn_noise.hpp"
#include "../../../include/error.hpp"

using qplayer::QubitIndex;
using qplayer::SimulatorError;
using qplayer::UnitaryMatrix;
using qplayer::detail::MpsState;
using qplayer::detail::apply_kraus_1q;
using qplayer::detail::apply_kraus_2q;
using qplayer::detail::apply_kraus_operator;

using Cx = std::complex<double>;

namespace {

// ── Kraus operators used below ───────────────────────────────────────────────

// Amplitude damping, gamma: K0 = [[1,0],[0,sqrt(1-g)]], K1 = [[0,sqrt(g)],[0,0]].
UnitaryMatrix damping_k0(double g) {
    return {{Cx{1.0, 0.0}, Cx{0.0, 0.0}},
            {Cx{0.0, 0.0}, Cx{std::sqrt(1.0 - g), 0.0}}};
}
UnitaryMatrix damping_k1(double g) {
    return {{Cx{0.0, 0.0}, Cx{std::sqrt(g), 0.0}},
            {Cx{0.0, 0.0}, Cx{0.0, 0.0}}};
}

// Pauli X as a Kraus operator (UnitaryMatrix form) -- a one-operator channel
// holding this is the shape a coherent over-rotation arrives in.
UnitaryMatrix x_gate_matrix() {
    return {{Cx{0.0, 0.0}, Cx{1.0, 0.0}},
            {Cx{1.0, 0.0}, Cx{0.0, 0.0}}};
}

UnitaryMatrix identity_2x2() {
    return {{Cx{1.0, 0.0}, Cx{0.0, 0.0}},
            {Cx{0.0, 0.0}, Cx{1.0, 0.0}}};
}

// A non-unitary 4x4 operator with a known norm factor: sqrt(p) * I_4.
UnitaryMatrix scaled_identity_4x4(double p) {
    const Cx s{std::sqrt(p), 0.0};
    UnitaryMatrix m(4, std::vector<Cx>(4, Cx{0.0, 0.0}));
    for (std::size_t i = 0; i < 4; ++i) m[i][i] = s;
    return m;
}

// Two-qubit Kraus factor that entangles: sqrt(p) * CNOT in operand order
// (local bit 0 = q0 = control), so (b1,b0) -> (b1^b0, b0).
UnitaryMatrix scaled_cnot_4x4(double p) {
    const Cx s{std::sqrt(p), 0.0};
    UnitaryMatrix m(4, std::vector<Cx>(4, Cx{0.0, 0.0}));
    m[0][0] = s; m[3][1] = s; m[2][2] = s; m[1][3] = s;
    return m;
}

// ── Gates, for preparing states ─────────────────────────────────────────────

std::vector<Cx> h_gate() {
    const double s = 1.0 / std::sqrt(2.0);
    return {Cx{s, 0}, Cx{s, 0}, Cx{s, 0}, Cx{-s, 0}};
}
std::vector<Cx> x_gate() {
    return {Cx{0, 0}, Cx{1, 0}, Cx{1, 0}, Cx{0, 0}};
}
std::vector<Cx> ry_gate(double th) {
    return {Cx{std::cos(th / 2), 0}, Cx{-std::sin(th / 2), 0},
            Cx{std::sin(th / 2), 0}, Cx{ std::cos(th / 2), 0}};
}
std::vector<Cx> cnot_gate() {
    std::vector<Cx> m(16, Cx{0, 0});
    m[0 * 4 + 0] = 1; m[3 * 4 + 1] = 1; m[2 * 4 + 2] = 1; m[1 * 4 + 3] = 1;
    return m;
}

// Independent oracle: apply a 2x2 operator to qubit `q` of a dense statevector.
// Written out longhand so it does not share code with the MPS path under test.
std::vector<Cx> dense_apply_1q(std::vector<Cx> v, const UnitaryMatrix& op, QubitIndex q) {
    const std::size_t bit = std::size_t{1} << q;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i & bit) continue;
        const Cx a0 = v[i];
        const Cx a1 = v[i | bit];
        v[i]       = op[0][0] * a0 + op[0][1] * a1;
        v[i | bit] = op[1][0] * a0 + op[1][1] * a1;
    }
    return v;
}

double squared_norm_of(const std::vector<Cx>& v) {
    double n = 0.0;
    for (const Cx& c : v) n += std::norm(c);
    return n;
}

qplayer::KrausChannel channel_1q(std::vector<UnitaryMatrix> ops, QubitIndex q) {
    qplayer::KrausChannel ch;
    ch.operators    = std::move(ops);
    ch.target_qubit = q;
    return ch;
}

}  // namespace

// ── 1Q operator: bond dimensions and the truncation ledger ──────────────────

TEST(TnNoise, Kraus1q_LeavesBondsAndTruncationLedgerUntouched) {
    // Arrange: an entangled chain, so the bonds are above 1 and a stray SVD
    // would be visible. A product state could not show that.
    MpsState s(3);
    s.apply_1q(h_gate(), 0);
    s.apply_2q(cnot_gate(), 0, 1);
    s.apply_2q(cnot_gate(), 1, 2);
    const std::uint32_t chi_before = s.max_bond_dimension();
    const std::size_t   errors_before = s.truncation_errors().size();

    // Act
    apply_kraus_1q(s, damping_k0(0.3), 1);

    // Assert: a single-site operator cannot create entanglement, so neither the
    // bond dimension nor the accuracy budget may move.
    EXPECT_EQ(s.max_bond_dimension(), chi_before);
    EXPECT_EQ(s.truncation_errors().size(), errors_before);
}

TEST(TnNoise, Kraus1q_PreservesTheNormReduction) {
    // Arrange: |1> on qubit 0, where damping K0 = diag(1, sqrt(1-g)) scales the
    // whole state by sqrt(1-g) — an analytically known factor.
    const double g = 0.36;
    MpsState s(2);
    s.apply_1q(x_gate(), 0);

    // Act
    apply_kraus_1q(s, damping_k0(g), 0);

    // Assert: the norm reduction IS the branch probability the trajectory loop
    // will read. Renormalising here would destroy that information.
    EXPECT_NEAR(s.squared_norm(), 1.0 - g, 1e-12);
}

TEST(TnNoise, Kraus1q_MatchesADenseReference) {
    // Arrange: an asymmetric entangled state. Per-qubit rotations differ so a
    // wrong-site application changes the amplitudes rather than permuting an
    // invariant state (the GHZ symmetry trap seen).
    MpsState s(3);
    s.apply_1q(ry_gate(0.4), 0);
    s.apply_1q(ry_gate(0.9), 1);
    s.apply_1q(ry_gate(1.3), 2);
    s.apply_2q(cnot_gate(), 0, 1);
    s.apply_2q(cnot_gate(), 1, 2);
    const std::vector<Cx> before = s.to_dense();
    const UnitaryMatrix K = damping_k1(0.45);

    // Act
    apply_kraus_1q(s, K, 1);
    const std::vector<Cx> after    = s.to_dense();
    const std::vector<Cx> expected = dense_apply_1q(before, K, 1);

    // Assert
    double max_diff = 0.0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(after[i] - expected[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

TEST(TnNoise, Kraus1q_TracePreservingChannelSumsToOne) {
    // Arrange: sum_i ||K_i psi||^2 == 1 for a trace-preserving channel. This is
    // the quantity will sample, so the primitive has to make it
    // readable — the test doubles as a usability check on that contract.
    const double g = 0.28;
    MpsState base(2);
    base.apply_1q(ry_gate(0.7), 0);
    base.apply_2q(cnot_gate(), 0, 1);

    // Act: each branch runs on its own copy, as a trajectory would.
    MpsState b0 = base;
    MpsState b1 = base;
    apply_kraus_1q(b0, damping_k0(g), 0);
    apply_kraus_1q(b1, damping_k1(g), 0);

    // Assert
    EXPECT_NEAR(b0.squared_norm() + b1.squared_norm(), 1.0, 1e-12);
}

// ── 2Q operator: the gate path, and the norm through the SVD ────────────────

TEST(TnNoise, Kraus2q_RecordsOneTruncationErrorLikeAGate) {
    // Arrange
    MpsState s(3);
    s.apply_1q(h_gate(), 0);
    const std::size_t errors_before = s.truncation_errors().size();

    // Act
    apply_kraus_2q(s, scaled_cnot_4x4(0.5), 0, 1);

    // Assert: it runs the gate path, so it pays the same accounting. Hiding the
    // entry would make 's accumulated budget under-report channel-induced
    // truncation.
    EXPECT_EQ(s.truncation_errors().size(), errors_before + 1);
}

TEST(TnNoise, Kraus2q_PreservesTheNormThroughTheSvd) {
    // Arrange: sqrt(p) * I_4 scales the state by sqrt(p) and nothing else, so
    // the post-call norm isolates whether the SVD path renormalised the Kraus
    // reduction away. This is the contract the task sheet calls out:
    // apply_2q's rescale restores the PRE-truncation norm, not 1.
    const double p = 0.4;
    MpsState s(2);
    s.apply_1q(ry_gate(0.8), 0);

    // Act
    apply_kraus_2q(s, scaled_identity_4x4(p), 0, 1);

    // Assert
    EXPECT_NEAR(s.squared_norm(), p, 1e-12);
}

TEST(TnNoise, Kraus2q_EntanglingOperatorRaisesTheBond) {
    // Arrange: a product state, where CNOT after H is what creates the
    // entanglement — bond must move from 1 to 2.
    MpsState s(2);
    s.apply_1q(h_gate(), 0);
    EXPECT_EQ(s.max_bond_dimension(), static_cast<std::uint32_t>(1));

    // Act
    apply_kraus_2q(s, scaled_cnot_4x4(1.0), 0, 1);

    // Assert
    EXPECT_EQ(s.max_bond_dimension(), static_cast<std::uint32_t>(2));
}

TEST(TnNoise, Kraus2q_NonAdjacentPairIsWalkedTogether) {
    // Arrange: sites 0 and 2 are not neighbours, so the gate path must SWAP them
    // together and back. Reusing apply_2q is what buys this for free.
    MpsState s(3);
    s.apply_1q(h_gate(), 0);

    // Act
    apply_kraus_2q(s, scaled_cnot_4x4(1.0), 0, 2);

    // Assert: |000> + |101> over the /2 normalisation, with qubit 1 untouched.
    const std::vector<Cx> v = s.to_dense();
    const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
    EXPECT_NEAR(std::abs(v[0]), inv_sqrt2, 1e-12);   // |000>
    EXPECT_NEAR(std::abs(v[5]), inv_sqrt2, 1e-12);   // |101> = bits 0 and 2 set
    EXPECT_NEAR(std::abs(v[1]), 0.0, 1e-12);
    EXPECT_NEAR(std::abs(v[4]), 0.0, 1e-12);
}

// ── Dispatcher ──────────────────────────────────────────────────────────────

TEST(TnNoise, KrausOperator_DispatchesOnOperatorWidth) {
    // Arrange: the observable difference between the two paths is whether a
    // truncation error is recorded, so the ledger reports which path ran.
    MpsState s(2);
    s.apply_1q(h_gate(), 0);

    // Act
    apply_kraus_operator(s, channel_1q({identity_2x2()}, 0), 0);
    const std::size_t after_1q = s.truncation_errors().size();

    qplayer::KrausChannel ch2;
    ch2.operators     = {scaled_identity_4x4(1.0)};
    ch2.target_qubit  = 0;
    ch2.target_qubit2 = 1;
    apply_kraus_operator(s, ch2, 0);
    const std::size_t after_2q = s.truncation_errors().size();

    // Assert
    EXPECT_EQ(after_1q, static_cast<std::size_t>(0));
    EXPECT_EQ(after_2q, static_cast<std::size_t>(1));
}

TEST(TnNoise, KrausOperator_UsesTheChannelTargetQubits) {
    // Arrange: damping K1 maps |1> -> |0>. Pointing the channel at qubit 1 must
    // move qubit 1's amplitude, not qubit 0's.
    MpsState s(2);
    s.apply_1q(x_gate(), 1);

    // Act
    apply_kraus_operator(s, channel_1q({damping_k1(1.0)}, 1), 0);

    // Assert: the state collapses to |00>, which sits at index 0.
    const std::vector<Cx> v = s.to_dense();
    EXPECT_NEAR(std::abs(v[0]), 1.0, 1e-12);
    EXPECT_NEAR(squared_norm_of(v), 1.0, 1e-12);
}

TEST(TnNoise, KrausOperator_SelectsTheRequestedOperator) {
    // Arrange: |1> with a two-operator damping channel. The branches land in
    // DIFFERENT basis states — K0 keeps |1> (scaled), K1 maps it to |0> — so the
    // index actually being read is observable. Single-operator channels cannot
    // show this, which is how a mutation that always applied operators[0]
    // survived the first version of these tests.
    const double g = 0.25;
    const qplayer::KrausChannel ch = channel_1q({damping_k0(g), damping_k1(g)}, 0);

    MpsState s0(2);
    s0.apply_1q(x_gate(), 0);
    MpsState s1(2);
    s1.apply_1q(x_gate(), 0);

    // Act
    apply_kraus_operator(s0, ch, 0);
    apply_kraus_operator(s1, ch, 1);
    const std::vector<Cx> v0 = s0.to_dense();
    const std::vector<Cx> v1 = s1.to_dense();

    // Assert: index 0 leaves the weight on |01> (bit 0 set), index 1 moves it to
    // |00>, and each carries its own branch probability.
    EXPECT_NEAR(std::abs(v0[1]), std::sqrt(1.0 - g), 1e-12);
    EXPECT_NEAR(std::abs(v0[0]), 0.0, 1e-12);
    EXPECT_NEAR(std::abs(v1[0]), std::sqrt(g), 1e-12);
    EXPECT_NEAR(std::abs(v1[1]), 0.0, 1e-12);
}

// ── Rejections ──────────────────────────────────────────────────────────────

TEST(TnNoise, KrausOperator_ThreeQubitOperator_Throws) {
    // Arrange: an 8x8 operator. The MPS path applies 1 or 2 sites only, and
    // FullState also dispatches 2x2/4x4 only — neither side has a kernel.
    MpsState s(3);
    qplayer::KrausChannel ch;
    ch.operators = {UnitaryMatrix(8, std::vector<Cx>(8, Cx{0.0, 0.0}))};

    // Act + Assert
    EXPECT_THROW(apply_kraus_operator(s, ch, 0), SimulatorError);
}

TEST(TnNoise, KrausOperator_EmptyOperatorList_Throws) {
    // Arrange
    MpsState s(2);
    qplayer::KrausChannel ch;

    // Act + Assert
    EXPECT_THROW(apply_kraus_operator(s, ch, 0), SimulatorError);
}

TEST(TnNoise, KrausOperator_IndexOutOfRange_Throws) {
    // Arrange
    MpsState s(2);

    // Act + Assert
    EXPECT_THROW(apply_kraus_operator(s, channel_1q({identity_2x2()}, 0), 1), SimulatorError);
}

TEST(TnNoise, KrausOperator_RaggedOperator_Throws) {
    // Arrange: rows of 3 and 1 — ragged, but the flattened length is exactly 4.
    // The row-count and total-length checks both pass on this shape, so only the
    // per-row check can reject it. A ragged operator whose flat size happens to
    // be wrong would be caught downstream by apply_1q for the wrong reason,
    // which is how the first version of this test passed against a mutation that
    // deleted the row check entirely.
    MpsState s(2);
    qplayer::KrausChannel ch;
    ch.operators = {{{Cx{1.0, 0.0}, Cx{0.0, 0.0}, Cx{0.0, 0.0}}, {Cx{0.0, 0.0}}}};

    // Act + Assert
    EXPECT_THROW(apply_kraus_operator(s, ch, 0), SimulatorError);
}

TEST(TnNoise, Kraus1q_WrongShape_Throws) {
    // Arrange
    MpsState s(2);

    // Act + Assert
    EXPECT_THROW(apply_kraus_1q(s, scaled_identity_4x4(1.0), 0), SimulatorError);
}

TEST(TnNoise, Kraus1q_QubitOutOfRange_Throws) {
    // Arrange
    MpsState s(2);

    // Act + Assert
    EXPECT_THROW(apply_kraus_1q(s, identity_2x2(), 2), SimulatorError);
}

TEST(TnNoise, Kraus2q_WrongShape_Throws) {
    // Arrange
    MpsState s(2);

    // Act + Assert
    EXPECT_THROW(apply_kraus_2q(s, identity_2x2(), 0, 1), SimulatorError);
}

TEST(TnNoise, Kraus2q_SameQubitTwice_Throws) {
    // Arrange
    MpsState s(2);

    // Act + Assert
    EXPECT_THROW(apply_kraus_2q(s, scaled_identity_4x4(1.0), 1, 1), SimulatorError);
}

// ── Branch probabilities ─────────────────────────────────────────────
//
// p_i = ||K_i psi||^2 is what the trajectory loop samples from, so these tests
// check it against an oracle that does NOT share the RDM path: apply the operator
// to a copy of the chain and read squared_norm(). That is exactly the alternative
// implementation weighed against the RDM sweep, which makes it
// a genuine cross-check rather than a restatement.

namespace {

// Oracle: copy, apply, read the norm. O(n*chi^3) per operator — the cost the
// RDM route avoids.
std::vector<double> probabilities_by_copy_and_norm(const MpsState& state,
                                                   const qplayer::KrausChannel& ch) {
    std::vector<double> out;
    out.reserve(ch.operators.size());
    for (std::size_t i = 0; i < ch.operators.size(); ++i) {
        MpsState copy = state;
        apply_kraus_operator(copy, ch, i);
        out.push_back(copy.squared_norm());
    }
    return out;
}

qplayer::KrausChannel channel_2q(std::vector<UnitaryMatrix> ops, QubitIndex q0, QubitIndex q1) {
    qplayer::KrausChannel ch;
    ch.operators      = std::move(ops);
    ch.target_qubit   = q0;
    ch.target_qubit2  = q1;
    return ch;
}

// Two-qubit depolarizing-style factors: sqrt(p)*I and sqrt(p)*(X on local bit 1).
UnitaryMatrix scaled_x_on_bit1(double p) {
    const Cx s{std::sqrt(p), 0.0};
    UnitaryMatrix m(4, std::vector<Cx>(4, Cx{0.0, 0.0}));
    // X on local bit 1 maps (b1,b0) -> (1-b1, b0): 0<->2, 1<->3.
    m[2][0] = s; m[3][1] = s; m[0][2] = s; m[1][3] = s;
    return m;
}

}  // namespace

TEST(TnNoise, BranchProbabilities_MatchTheCopyAndNormOracle) {
    // Arrange: an asymmetric entangled state, so a wrong-site RDM gives a
    // different answer rather than an equal one.
    MpsState s(3);
    s.apply_1q(ry_gate(0.5), 0);
    s.apply_1q(ry_gate(1.1), 1);
    s.apply_1q(ry_gate(0.8), 2);
    s.apply_2q(cnot_gate(), 0, 1);
    s.apply_2q(cnot_gate(), 1, 2);
    const double g = 0.31;
    const qplayer::KrausChannel ch = channel_1q({damping_k0(g), damping_k1(g)}, 1);

    // Act
    const std::vector<double> viaRdm  = qplayer::detail::kraus_branch_probabilities(s, ch);
    const std::vector<double> viaCopy = probabilities_by_copy_and_norm(s, ch);

    // Assert
    EXPECT_EQ(viaRdm.size(), viaCopy.size());
    for (std::size_t i = 0; i < viaRdm.size(); ++i) EXPECT_NEAR(viaRdm[i], viaCopy[i], 1e-12);
}

TEST(TnNoise, BranchProbabilities_SumToTheStateNorm) {
    // Arrange: trace-preserving channel on a normalised state -> sum == 1.
    MpsState s(2);
    s.apply_1q(ry_gate(0.9), 0);
    s.apply_2q(cnot_gate(), 0, 1);

    // Act
    const std::vector<double> p =
        qplayer::detail::kraus_branch_probabilities(s, channel_1q({damping_k0(0.4), damping_k1(0.4)}, 0));

    // Assert
    double total = 0.0;
    for (double x : p) total += x;
    EXPECT_NEAR(total, s.squared_norm(), 1e-12);
}

TEST(TnNoise, BranchProbabilities_TwoSiteChannelRespectsOperandOrder) {
    // Arrange: a 2-site channel whose operators are NOT symmetric under swapping
    // the operands (X acts on local bit 1 only). The RDM's bit order is ascending
    // SITE while the operator's is operand order, so passing (1,0) must give the
    // same answer as the oracle — which applies the operator directly and cannot
    // disagree about the convention.
    MpsState s(3);
    s.apply_1q(ry_gate(0.6), 0);
    s.apply_1q(ry_gate(1.2), 1);
    s.apply_2q(cnot_gate(), 0, 1);
    const auto ops = std::vector<UnitaryMatrix>{scaled_identity_4x4(0.7), scaled_x_on_bit1(0.3)};

    // Act: reversed operand order (q0 = 1 > q1 = 0) is the case that exposes a
    // missing basis permutation.
    const qplayer::KrausChannel rev = channel_2q(ops, 1, 0);
    const std::vector<double> viaRdm  = qplayer::detail::kraus_branch_probabilities(s, rev);
    const std::vector<double> viaCopy = probabilities_by_copy_and_norm(s, rev);

    // Assert
    for (std::size_t i = 0; i < viaRdm.size(); ++i) EXPECT_NEAR(viaRdm[i], viaCopy[i], 1e-12);
}

// ── Channel application ──────────────────────────────────────────────

TEST(TnNoise, ApplyChannel_KeepsTheStateNormalised) {
    // Arrange: the channel is trace-preserving, so after selection the state must
    // be renormalised — the trajectory loop assumes a unit-norm state at every
    // step, and the norm check at the end (1e-6) would otherwise trip.
    MpsState s(2);
    s.apply_1q(ry_gate(0.7), 0);
    s.apply_2q(cnot_gate(), 0, 1);
    std::mt19937_64 rng(12345);

    // Act
    qplayer::detail::apply_kraus_channel(s, channel_1q({damping_k0(0.5), damping_k1(0.5)}, 0), rng);

    // Assert
    EXPECT_NEAR(s.squared_norm(), 1.0, 1e-12);
}

TEST(TnNoise, ApplyChannel_SelectionFollowsTheProbabilities) {
    // Arrange: |1> with full damping (g = 1). K0 = diag(1, 0) annihilates it, so
    // p_0 = 0 and every draw must pick K1. A selector that ignored the weights
    // would land on the zero branch roughly half the time.
    const qplayer::KrausChannel ch = channel_1q({damping_k0(1.0), damping_k1(1.0)}, 0);
    std::mt19937_64 rng(7);

    for (int trial = 0; trial < 20; ++trial) {
        MpsState s(2);
        s.apply_1q(x_gate(), 0);

        // Act
        const std::size_t chosen = qplayer::detail::apply_kraus_channel(s, ch, rng);

        // Assert
        EXPECT_EQ(chosen, static_cast<std::size_t>(1));
    }
}

TEST(TnNoise, ApplyChannel_BranchFrequencyMatchesTheWeights) {
    // Arrange: |1> with damping g, where p_0 = 1-g and p_1 = g exactly. This is
    // the statistical contract the trajectory loop rests on, so it is worth
    // measuring rather than assuming.
    const double g = 0.25;
    const qplayer::KrausChannel ch = channel_1q({damping_k0(g), damping_k1(g)}, 0);
    std::mt19937_64 rng(20260825);
    const int trials = 4000;
    int ones = 0;

    // Act
    for (int t = 0; t < trials; ++t) {
        MpsState s(2);
        s.apply_1q(x_gate(), 0);
        if (qplayer::detail::apply_kraus_channel(s, ch, rng) == 1) ++ones;
    }

    // Assert: 3-sigma for n=4000, p=0.25 is about 0.021; 0.03 leaves headroom
    // while still failing a 50/50 selector by a wide margin.
    EXPECT_NEAR(static_cast<double>(ones) / trials, g, 0.03);
}

TEST(TnNoise, ApplyChannel_IsReproducibleForAFixedSeed) {
    // Arrange: same seed, same channel, same state (reproducibility).
    const qplayer::KrausChannel ch = channel_1q({damping_k0(0.4), damping_k1(0.4)}, 0);

    std::mt19937_64 rng_a(99);
    std::mt19937_64 rng_b(99);
    std::vector<std::size_t> picks_a;
    std::vector<std::size_t> picks_b;

    // Act
    for (int t = 0; t < 30; ++t) {
        MpsState a(2); a.apply_1q(ry_gate(0.9), 0);
        MpsState b(2); b.apply_1q(ry_gate(0.9), 0);
        picks_a.push_back(qplayer::detail::apply_kraus_channel(a, ch, rng_a));
        picks_b.push_back(qplayer::detail::apply_kraus_channel(b, ch, rng_b));
    }

    // Assert
    EXPECT_TRUE(picks_a == picks_b);
}

TEST(TnNoise, ApplyChannel_IdentityChannelLeavesTheStateAlone) {
    // Arrange: a single-operator identity channel is a boundary case — one
    // branch, probability 1, no rescale.
    MpsState s(2);
    s.apply_1q(ry_gate(1.0), 0);
    s.apply_2q(cnot_gate(), 0, 1);
    const std::vector<Cx> before = s.to_dense();
    std::mt19937_64 rng(3);

    // Act
    const std::size_t chosen =
        qplayer::detail::apply_kraus_channel(s, channel_1q({identity_2x2()}, 1), rng);

    // Assert
    EXPECT_EQ(chosen, static_cast<std::size_t>(0));
    const std::vector<Cx> after = s.to_dense();
    double max_diff = 0.0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(after[i] - before[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

TEST(TnNoise, ApplyChannel_AllBranchesZero_Throws) {
    // Arrange: an all-zero operator list gives total weight 0 — a numerically
    // divergent state rather than a valid draw.
    MpsState s(2);
    qplayer::KrausChannel ch;
    ch.operators = {UnitaryMatrix(2, std::vector<Cx>(2, Cx{0.0, 0.0}))};
    ch.target_qubit = 0;
    std::mt19937_64 rng(1);

    // Act + Assert
    EXPECT_THROW(qplayer::detail::apply_kraus_channel(s, ch, rng), SimulatorError);
}

// ── Single-operator (coherent) channel — W6,論 1 plan.md ────────────────
//
// A coherent over-rotation enters the noise model as a Kraus channel holding ONE
// unitary operator. Two properties have to hold for that to be usable, and
// neither is obvious from the multi-operator code path:
//   1. it is applied every time (probability 1), not sampled away, and
//   2. it still consumes exactly one RNG draw.
// (2) is what makes common random numbers work between a pair of arms that
// differ only in a channel: if a one-operator channel skipped the draw, the two
// arms' RNG streams would drift apart at the first coherent term.

TEST(TnNoise, SingleOperatorChannel_IsAlwaysApplied_1q) {
    // Arrange: one unitary operator (X). Ten different RNG states must all pick
    // branch 0, because there is no other branch.
    MpsState s(2);
    s.apply_1q(h_gate(), 0);
    const auto ch = channel_1q({x_gate_matrix()}, 0);

    // Act + Assert
    for (std::uint64_t seed = 0; seed < 10; ++seed) {
        std::mt19937_64 rng(seed);
        EXPECT_EQ(apply_kraus_channel(s, ch, rng), static_cast<std::size_t>(0));
    }
}

TEST(TnNoise, SingleOperatorChannel_1q_LeavesTheTruncationLedgerUntouched) {
    // Arrange: an entangled chain, so a stray SVD would show up in the ledger.
    MpsState s(3);
    s.apply_1q(h_gate(), 0);
    s.apply_2q(cnot_gate(), 0, 1);
    const std::size_t before = s.truncation_errors().size();

    // Act
    std::mt19937_64 rng(7);
    apply_kraus_channel(s, channel_1q({x_gate_matrix()}, 0), rng);

    // Assert: a 1Q operator needs no factorisation, coherent or not.
    EXPECT_EQ(s.truncation_errors().size(), before);
}

TEST(TnNoise, SingleOperatorChannel_2q_RecordsExactlyOneLedgerEntry) {
    // Arrange
    MpsState s(3);
    s.apply_1q(h_gate(), 0);
    qplayer::KrausChannel ch;
    ch.operators     = {scaled_cnot_4x4(1.0)};   // unitary: p = 1
    ch.target_qubit  = 0;
    ch.target_qubit2 = 1;
    const std::size_t before = s.truncation_errors().size();

    // Act
    std::mt19937_64 rng(7);
    const std::size_t chosen = apply_kraus_channel(s, ch, rng);

    // Assert: one entry per applied 2Q operator, matching the rule that
    // the ledger counts user-visible operations rather than internal SVDs.
    EXPECT_EQ(chosen, static_cast<std::size_t>(0));
    EXPECT_EQ(s.truncation_errors().size(), before + 1);
}

TEST(TnNoise, SingleOperatorChannel_ConsumesOneDrawLikeAnyChannel) {
    // Arrange: two identical RNGs. One is spent by a one-operator channel, the
    // other by a two-operator channel on an identical state.
    MpsState a(2), b(2);
    a.apply_1q(h_gate(), 0);
    b.apply_1q(h_gate(), 0);
    std::mt19937_64 rng_one(11), rng_two(11);

    // Act
    apply_kraus_channel(a, channel_1q({x_gate_matrix()}, 0), rng_one);
    apply_kraus_channel(b, channel_1q({damping_k0(0.3), damping_k1(0.3)}, 0), rng_two);

    // Assert: the streams are still aligned, so a pair of arms that differ only
    // in whether a channel is coherent stays synchronised (CRN).
    EXPECT_EQ(rng_one(), rng_two());
}

// ── Cached site marginals ────────────────────────────────────────
//
// The cache is only correct if every site-tensor write invalidates what depends
// on it, and a missed invalidation returns a marginal of an OLDER state -- a
// plausible wrong answer, not a crash. So every test below compares against the
// uncached free function, which rebuilds both environments from the chain.

namespace {

// Brickwork of ry + CNOT, deep enough that the middle bonds reach 64 on 12
// sites, so both the scalar and the GEMM step kernels (threshold 32) are hit.
MpsState brickwork_chain(QubitIndex n, int depth) {
    MpsState s(n);
    for (int d = 0; d < depth; ++d) {
        for (QubitIndex q = 0; q < n; ++q) s.apply_1q(ry_gate(0.3 + 0.17 * q + 0.11 * d), q);
        for (QubitIndex q = static_cast<QubitIndex>(d % 2); q + 1 < n; q += 2) {
            s.apply_2q(cnot_gate(), q, q + 1);
        }
    }
    return s;
}

// Largest entry-wise gap between the cached and the uncached marginal, over all sites.
double worst_marginal_gap(const MpsState& s) {
    double worst = 0.0;
    for (QubitIndex k = 0; k < s.num_qubits(); ++k) {
        const std::vector<Cx> cached = s.site_marginal(k);
        const std::vector<Cx> fresh  = qplayer::detail::site_marginal(s.graph(), k);
        for (std::size_t i = 0; i < 4; ++i) worst = std::max(worst, std::abs(cached[i] - fresh[i]));
    }
    return worst;
}

// Queries every site so the cache is fully populated before a mutation.
void warm_all_sites(const MpsState& s) {
    for (QubitIndex k = 0; k < s.num_qubits(); ++k) (void)s.site_marginal(k);
}

std::vector<Cx> flat_2x2(const UnitaryMatrix& m) {
    return {m[0][0], m[0][1], m[1][0], m[1][1]};
}

}  // namespace

TEST(TnNoise, SiteMarginalCache_FreshChain_MatchesUncached) {
    // Arrange: bonds reach 64, so the GEMM step kernel is exercised too.
    const MpsState s = brickwork_chain(12, 12);

    // Act
    const double gap = worst_marginal_gap(s);

    // Assert: same kernels on the same tensors, so only rounding may differ.
    EXPECT_TRUE(s.max_bond_dimension() >= 32u);
    EXPECT_NEAR(gap, 0.0, 1e-12);
}

TEST(TnNoise, SiteMarginalCache_AfterNonUnitary1q_MatchesUncached) {
    // Arrange
    MpsState s = brickwork_chain(12, 12);
    warm_all_sites(s);

    // Act: a damping branch is not unitary, so environments through site 5 die.
    s.apply_1q(flat_2x2(damping_k1(0.4)), 5);

    // Assert
    EXPECT_NEAR(worst_marginal_gap(s), 0.0, 1e-12);
}

TEST(TnNoise, SiteMarginalCache_AfterUnitary1q_KeepsCacheAndMatchesUncached) {
    // Arrange
    MpsState s = brickwork_chain(12, 12);
    warm_all_sites(s);

    // Act: a unitary leaves every other environment unchanged, so
    // the cache is kept -- this is the exemption, and the one place a stale
    // environment is reused on purpose.
    s.apply_1q(ry_gate(0.9), 6);
    s.apply_1q(h_gate(), 0);
    s.apply_1q(x_gate(), 11);

    // Assert: 1e-12 because the kept environments are mathematically, not
    // bitwise, equal to the rebuilt ones.
    EXPECT_NEAR(worst_marginal_gap(s), 0.0, 1e-12);
}

TEST(TnNoise, SiteMarginalCache_AfterWalked2q_MatchesUncached) {
    // Arrange
    MpsState s = brickwork_chain(12, 12);
    warm_all_sites(s);

    // Act: non-adjacent operands SWAP-walk through sites 3..9.
    s.apply_2q(cnot_gate(), 3, 9);

    // Assert
    EXPECT_NEAR(worst_marginal_gap(s), 0.0, 1e-12);
}

TEST(TnNoise, SiteMarginalCache_AfterRescale_MatchesUncached) {
    // Arrange
    MpsState s = brickwork_chain(12, 12);
    warm_all_sites(s);

    // Act
    s.rescale(0.7);

    // Assert: a scalar on one site still changes every environment through it.
    EXPECT_NEAR(worst_marginal_gap(s), 0.0, 1e-12);
}

TEST(TnNoise, SiteMarginalCache_AfterCanonicalize_MatchesUncached) {
    // Arrange
    MpsState s = brickwork_chain(12, 12);
    warm_all_sites(s);

    // Act: the gauge sweep rewrites every site without changing the state.
    s.canonicalize(5);

    // Assert
    EXPECT_NEAR(worst_marginal_gap(s), 0.0, 1e-12);
}

TEST(TnNoise, SiteMarginalCache_InterleavedChannelsAndQueries_MatchUncached) {
    // Arrange: a fixed-seed mix of gates, channels and partial queries, so the
    // two watermarks are left at many different positions between mutations.
    MpsState s = brickwork_chain(10, 8);
    std::mt19937_64 rng(2026);
    std::uniform_int_distribution<int> pick_site(0, 9);
    std::uniform_int_distribution<int> pick_op(0, 3);
    double worst = 0.0;

    // Act
    for (int step = 0; step < 60; ++step) {
        const QubitIndex q = static_cast<QubitIndex>(pick_site(rng));
        switch (pick_op(rng)) {
            case 0: s.apply_1q(ry_gate(0.2 + 0.01 * step), q); break;
            case 1: qplayer::detail::apply_kraus_channel(s, channel_1q({damping_k0(0.2), damping_k1(0.2)}, q), rng); break;
            case 2: s.apply_2q(cnot_gate(), q, static_cast<QubitIndex>((q + 3) % 10)); break;
            default: break;
        }
        const QubitIndex probe = static_cast<QubitIndex>(pick_site(rng));
        const std::vector<Cx> cached = s.site_marginal(probe);
        const std::vector<Cx> fresh  = qplayer::detail::site_marginal(s.graph(), probe);
        for (std::size_t i = 0; i < 4; ++i) worst = std::max(worst, std::abs(cached[i] - fresh[i]));
    }

    // Assert
    EXPECT_NEAR(worst, 0.0, 1e-12);
}

TEST(TnNoise, SiteMarginalCache_SiteOutOfRange_Throws) {
    // Arrange
    const MpsState s(3);

    // Act / Assert
    EXPECT_THROW(s.site_marginal(3), SimulatorError);
}

// ── Cached two-site marginals ─────────────────────────────────
//
// Reference is the general RDM sweep, which shares no code with the cached path
// beyond reading the same site tensors.

namespace {

// Largest entry-wise gap to mps_reduced_density_matrix over every site pair.
double worst_pair_gap(const MpsState& s) {
    double worst = 0.0;
    for (QubitIndex lo = 0; lo < s.num_qubits(); ++lo) {
        for (QubitIndex hi = lo + 1; hi < s.num_qubits(); ++hi) {
            const std::vector<Cx> cached = s.two_site_marginal(lo, hi);
            const std::vector<Cx> fresh  =
                qplayer::detail::mps_reduced_density_matrix(s.graph(), {lo, hi});
            for (std::size_t i = 0; i < 16; ++i) worst = std::max(worst, std::abs(cached[i] - fresh[i]));
        }
    }
    return worst;
}

}  // namespace

TEST(TnNoise, TwoSiteMarginalCache_EveryPair_MatchesGeneralRdm) {
    // Arrange: 8 sites keep the O(chi^4) reference cheap; depth 8 still takes
    // the middle bonds to 16, so adjacent and distant pairs both see real bonds.
    const MpsState s = brickwork_chain(8, 8);

    // Act
    const double gap = worst_pair_gap(s);

    // Assert
    EXPECT_NEAR(gap, 0.0, 1e-12);
}

TEST(TnNoise, TwoSiteMarginalCache_GemmBondsAdjacentAndDistant_MatchGeneralRdm) {
    // Arrange: bonds reach 64, so the open step takes its GEMM branch.
    const MpsState s = brickwork_chain(12, 12);

    // Act: the pairs that matter most -- adjacent in the middle, and the far span.
    double worst = 0.0;
    for (const auto& [lo, hi] : std::vector<std::pair<QubitIndex, QubitIndex>>{{5, 6}, {2, 9}, {0, 11}}) {
        const std::vector<Cx> cached = s.two_site_marginal(lo, hi);
        const std::vector<Cx> fresh  = qplayer::detail::mps_reduced_density_matrix(s.graph(), {lo, hi});
        for (std::size_t i = 0; i < 16; ++i) worst = std::max(worst, std::abs(cached[i] - fresh[i]));
    }

    // Assert
    EXPECT_TRUE(s.max_bond_dimension() >= 32u);
    EXPECT_NEAR(worst, 0.0, 1e-12);
}

TEST(TnNoise, TwoSiteMarginalCache_After2qChannelsAndGates_MatchesGeneralRdm) {
    // Arrange: warm the cache, then mutate through the 2Q Kraus path itself.
    MpsState s = brickwork_chain(8, 8);
    (void)worst_pair_gap(s);
    qplayer::KrausChannel ch;
    ch.operators     = {scaled_identity_4x4(0.6), scaled_cnot_4x4(0.4)};
    ch.target_qubit  = 5;
    ch.target_qubit2 = 2;
    std::mt19937_64 rng(99);

    // Act
    qplayer::detail::apply_kraus_channel(s, ch, rng);
    s.apply_2q(cnot_gate(), 1, 6);
    s.apply_1q(flat_2x2(damping_k1(0.3)), 3);

    // Assert
    EXPECT_NEAR(worst_pair_gap(s), 0.0, 1e-12);
}

TEST(TnNoise, TwoSiteMarginalCache_BadSitePair_Throws) {
    // Arrange
    const MpsState s(4);

    // Act / Assert: equal sites, reversed order, and out of range.
    EXPECT_THROW(s.two_site_marginal(2, 2), SimulatorError);
    EXPECT_THROW(s.two_site_marginal(3, 1), SimulatorError);
    EXPECT_THROW(s.two_site_marginal(1, 4), SimulatorError);
}

// ── squared_norm() through the cache ──────────────────────────

TEST(TnNoise, SquaredNormFromCache_MatchesDenseNorm) {
    // Arrange: a lossless 12-site chain, so the dense vector is the exact oracle.
    const MpsState s = brickwork_chain(12, 12);

    // Act
    const double from_cache = s.squared_norm();
    const double from_dense = squared_norm_of(s.to_dense());

    // Assert: both are sums of ~4096 products of O(1) terms.
    EXPECT_NEAR(from_cache, from_dense, 1e-12);
}

TEST(TnNoise, SquaredNormFromCache_FollowsMutationsAfterWarmCache) {
    // Arrange: cache fully populated, then the norm is moved by a known factor.
    MpsState s = brickwork_chain(12, 12);
    warm_all_sites(s);
    const double before = s.squared_norm();

    // Act: rescale by 0.7 scales |psi|^2 by 0.49; a damping branch then changes
    // it by a state-dependent amount the dense oracle knows.
    s.rescale(0.7);
    const double after_rescale = s.squared_norm();
    s.apply_1q(flat_2x2(damping_k1(0.4)), 7);

    // Assert
    EXPECT_NEAR(after_rescale, 0.49 * before, 1e-12);
    EXPECT_NEAR(s.squared_norm(), squared_norm_of(s.to_dense()), 1e-12);
}
