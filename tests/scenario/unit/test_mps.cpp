// test_mps.cpp — Unit tests for MpsState: gate application, SVD
// truncation under the policy, truncation-error accounting, and
// dense contraction cross-checked against FullState.
// 1.1, 1.3 (Arrange/Act/Assert).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <cstdlib>
#include <vector>

#include "../../../include/backend/tensor_network/mps.hpp"
#include "../../../include/backend/full_state/full_state_backend.hpp"  // cross-check
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::CircuitError;
using qplayer::SimulatorError;
using qplayer::QubitIndex;
using qplayer::detail::MpsState;
using qplayer::detail::MpsTruncationConfig;

using Cx = std::complex<double>;

namespace {

const double kInvSqrt2 = M_SQRT1_2;

std::vector<Cx> h_matrix() {
    return {{kInvSqrt2, 0}, {kInvSqrt2, 0}, {kInvSqrt2, 0}, {-kInvSqrt2, 0}};
}
std::vector<Cx> x_matrix() { return {{0, 0}, {1, 0}, {1, 0}, {0, 0}}; }

// CX with local bit 0 = control, bit 1 = target (operand order).
std::vector<Cx> cx_matrix() {
    std::vector<Cx> m(16, Cx{0, 0});
    auto at = [&](int r, int c) -> Cx& { return m[static_cast<std::size_t>(r * 4 + c)]; };
    at(0, 0) = {1, 0};   // |00> -> |00>
    at(2, 2) = {1, 0};   // |01> (target set, control clear) unchanged
    at(3, 1) = {1, 0};   // |10> -> |11>
    at(1, 3) = {1, 0};   // |11> -> |10>
    return m;
}

double prob(const std::vector<Cx>& sv, std::size_t i) { return std::norm(sv[i]); }

}  // namespace

// ── Construction ─────────────────────────────────────────────────────────────

TEST(Mps, Construct_ZeroState_IsProductStateWithUnitNorm) {
    // Arrange + Act
    MpsState mps(4);

    // Assert: |0000> has amplitude 1 at index 0 and nothing elsewhere.
    std::vector<Cx> sv = mps.to_dense();
    EXPECT_EQ(sv.size(), static_cast<std::size_t>(16));
    EXPECT_NEAR(prob(sv, 0), 1.0, 1e-14);  // exact by construction
    EXPECT_NEAR(mps.squared_norm(), 1.0, 1e-14);
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(1));
}

TEST(Mps, Construct_ConfigAboveCeiling_Throws) {
    // Arrange
    MpsTruncationConfig cfg;
    cfg.max_bond = qplayer::kMpsMaxBondCeiling + 1;

    // Act + Assert: rejected where it is set, not deep inside an SVD (B).
    EXPECT_THROW(MpsState(2, cfg), CircuitError);
}

TEST(Mps, Construct_ZeroMaxBond_Throws) {
    // Arrange
    MpsTruncationConfig cfg;
    cfg.max_bond = 0;

    // Act + Assert
    EXPECT_THROW(MpsState(2, cfg), CircuitError);
}

// ── One-qubit gates ──────────────────────────────────────────────────────────

TEST(Mps, Apply1q_Hadamard_ProducesUniformSuperposition) {
    // Arrange
    MpsState mps(1);

    // Act
    mps.apply_1q(h_matrix(), 0);

    // Assert
    std::vector<Cx> sv = mps.to_dense();
    EXPECT_NEAR(prob(sv, 0), 0.5, 1e-14);
    EXPECT_NEAR(prob(sv, 1), 0.5, 1e-14);
}

TEST(Mps, Apply1q_NeverGrowsBondDimension) {
    // Arrange
    MpsState mps(3);

    // Act: one-qubit gates cannot create entanglement, so chi must stay at 1.
    mps.apply_1q(h_matrix(), 0);
    mps.apply_1q(h_matrix(), 1);
    mps.apply_1q(x_matrix(), 2);

    // Assert
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(1));
    EXPECT_TRUE(mps.truncation_errors().empty());
}

TEST(Mps, Apply1q_TargetsTheNamedQubitOnly) {
    // Arrange
    MpsState mps(3);

    // Act: flip qubit 1 only.
    mps.apply_1q(x_matrix(), 1);

    // Assert: |010> is index 2 under the bit-q-is-qubit-q convention.
    std::vector<Cx> sv = mps.to_dense();
    EXPECT_NEAR(prob(sv, 2), 1.0, 1e-14);
    EXPECT_NEAR(prob(sv, 1), 0.0, 1e-14);
    EXPECT_NEAR(prob(sv, 4), 0.0, 1e-14);
}

TEST(Mps, Apply1q_QubitOutOfRange_Throws) {
    MpsState mps(2);
    EXPECT_THROW(mps.apply_1q(h_matrix(), 2), SimulatorError);
}

TEST(Mps, Apply1q_WrongMatrixSize_Throws) {
    MpsState mps(2);
    EXPECT_THROW(mps.apply_1q({{1, 0}, {0, 0}}, 0), SimulatorError);
}

// ── Two-qubit gates ──────────────────────────────────────────────────────────

TEST(Mps, Apply2q_BellPair_HasBondDimensionTwo) {
    // Arrange
    MpsState mps(2);

    // Act: H(0) then CX(0,1).
    mps.apply_1q(h_matrix(), 0);
    mps.apply_2q(cx_matrix(), 0, 1);

    // Assert: one EPR pair across the single cut needs exactly chi = 2.
    std::vector<Cx> sv = mps.to_dense();
    EXPECT_NEAR(prob(sv, 0), 0.5, 1e-14);
    EXPECT_NEAR(prob(sv, 3), 0.5, 1e-14);
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(2));
}

TEST(Mps, Apply2q_RecordsOneTruncationErrorPerGate) {
    // Arrange
    MpsState mps(3);

    // Act
    mps.apply_1q(h_matrix(), 0);
    mps.apply_2q(cx_matrix(), 0, 1);
    mps.apply_2q(cx_matrix(), 1, 2);

    // Assert: the history is indexed by 2Q gate order, so the count
    // must match even when nothing was actually discarded.
    EXPECT_EQ(mps.truncation_errors().size(), static_cast<std::size_t>(2));
    for (double e : mps.truncation_errors()) EXPECT_NEAR(e, 0.0, 1e-12);
}

TEST(Mps, Apply2q_OperandOrderMatters_ReversedControlDiffers) {
    // Arrange: |10> (qubit 0 set) — an asymmetric input, so a swapped operand
    // order cannot pass by accident.
    MpsState forward(2);
    MpsState reversed(2);
    forward.apply_1q(x_matrix(), 0);
    reversed.apply_1q(x_matrix(), 0);

    // Act: cx(0,1) flips qubit 1; cx(1,0) does nothing (control qubit 1 is clear).
    forward.apply_2q(cx_matrix(), 0, 1);
    reversed.apply_2q(cx_matrix(), 1, 0);

    // Assert
    EXPECT_NEAR(prob(forward.to_dense(), 3), 1.0, 1e-14);   // |11>
    EXPECT_NEAR(prob(reversed.to_dense(), 1), 1.0, 1e-14);  // |01>, unchanged
}

TEST(Mps, Apply2q_NonAdjacentQubits_MatchesAdjacentSemantics) {
    // Arrange: cx(0,3) on a 4-site chain needs SWAPs to bring the pair together.
    MpsState mps(4);
    mps.apply_1q(x_matrix(), 0);

    // Act
    mps.apply_2q(cx_matrix(), 0, 3);

    // Assert: |1001> = index 1 + 8 = 9. A chain left permuted by the SWAP walk
    // would land the excitation on the wrong qubit.
    std::vector<Cx> sv = mps.to_dense();
    EXPECT_NEAR(prob(sv, 9), 1.0, 1e-13);
}

TEST(Mps, Apply2q_NonAdjacentReversedOrder_Works) {
    // Arrange: control to the right of the target (q0 > q1).
    MpsState mps(4);
    mps.apply_1q(x_matrix(), 3);

    // Act: cx(3,0) — control qubit 3 is set, so qubit 0 flips.
    mps.apply_2q(cx_matrix(), 3, 0);

    // Assert: |1001> = 9 again.
    EXPECT_NEAR(prob(mps.to_dense(), 9), 1.0, 1e-13);
}

TEST(Mps, Apply2q_SameQubitTwice_Throws) {
    MpsState mps(3);
    EXPECT_THROW(mps.apply_2q(cx_matrix(), 1, 1), SimulatorError);
}

TEST(Mps, Apply2q_QubitOutOfRange_Throws) {
    MpsState mps(3);
    EXPECT_THROW(mps.apply_2q(cx_matrix(), 0, 3), SimulatorError);
}

// ── Cross-check against FullState ────────────────────────────────────────────

TEST(Mps, GhzFiveQubits_MatchesFullStateAmplitudes) {
    // Arrange: GHZ-5 through the MPS path.
    MpsState mps(5);
    mps.apply_1q(h_matrix(), 0);
    for (QubitIndex q = 0; q + 1 < 5; ++q) mps.apply_2q(cx_matrix(), q, q + 1);

    // Act: same circuit through FullState CPU.
    qplayer::FullStateCpuBackend fs;
    qplayer::Circuit circ(5, 5);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < 5; ++q) circ.cx(q, q + 1);
    fs.init_state(5);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }

    // Assert: amplitude-by-amplitude, not just the distribution — a global phase
    // or a permuted chain would survive a probability-only comparison.
    std::vector<Cx> mps_sv = mps.to_dense();
    qplayer::StateVector fs_sv = fs.get_statevector();
    EXPECT_EQ(mps_sv.size(), fs_sv.size());
    double max_diff = 0.0;
    for (std::size_t i = 0; i < mps_sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(mps_sv[i] - fs_sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-12);  // no truncation occurred, so exact agreement
}

TEST(Mps, EntanglingLadder_MatchesFullStateWithinTolerance) {
    // Arrange: a denser 6-qubit circuit — H everywhere, then a CX ladder both ways.
    const QubitIndex n = 6;
    MpsState mps(n);
    qplayer::FullStateCpuBackend fs;
    qplayer::Circuit circ(n, n);
    fs.init_state(n);

    for (QubitIndex q = 0; q < n; ++q) { mps.apply_1q(h_matrix(), q); circ.h(q); }
    for (QubitIndex q = 0; q + 1 < n; ++q) { mps.apply_2q(cx_matrix(), q, q + 1); circ.cx(q, q + 1); }
    for (QubitIndex q = n - 1; q > 0; --q) { mps.apply_2q(cx_matrix(), q, q - 1); circ.cx(q, q - 1); }

    // Act
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }

    // Assert
    std::vector<Cx> mps_sv = mps.to_dense();
    qplayer::StateVector fs_sv = fs.get_statevector();
    double max_diff = 0.0;
    for (std::size_t i = 0; i < mps_sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(mps_sv[i] - fs_sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-10);  // E criterion for n <= 25
    EXPECT_NEAR(mps.squared_norm(), 1.0, 1e-10);
}

// ── Truncation policy ─────────────────────────────────────────────

TEST(Mps, Truncation_BondCapOne_ForcesLossAndRecordsError) {
    // Arrange: chi_max = 1 cannot represent a Bell pair at all.
    MpsTruncationConfig cfg;
    cfg.max_bond = 1;
    MpsState mps(2, cfg);

    // Act
    mps.apply_1q(h_matrix(), 0);
    mps.apply_2q(cx_matrix(), 0, 1);

    // Assert: both Schmidt values are 1/sqrt(2), so discarding one loses exactly
    // half the weight -> step error 1/sqrt(2).
    EXPECT_EQ(mps.truncation_errors().size(), static_cast<std::size_t>(1));
    EXPECT_NEAR(mps.truncation_errors()[0], kInvSqrt2, 1e-12);
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(1));
}

TEST(Mps, Truncation_RenormalisesAfterDiscarding) {
    // Arrange
    MpsTruncationConfig cfg;
    cfg.max_bond = 1;
    MpsState mps(2, cfg);

    // Act
    mps.apply_1q(h_matrix(), 0);
    mps.apply_2q(cx_matrix(), 0, 1);

    // Assert: an un-renormalised state would sit at norm 0.5 and quietly poison
    // every later probability (A requires renormalisation).
    EXPECT_NEAR(mps.squared_norm(), 1.0, 1e-12);
}

TEST(Mps, Truncation_TotalErrorAccumulatesInQuadrature) {
    // Arrange
    MpsTruncationConfig cfg;
    cfg.max_bond = 1;
    MpsState mps(3, cfg);

    // Act: two lossy truncations.
    mps.apply_1q(h_matrix(), 0);
    mps.apply_2q(cx_matrix(), 0, 1);
    mps.apply_1q(h_matrix(), 1);
    mps.apply_2q(cx_matrix(), 1, 2);

    // Assert: total = sqrt(sum of squares) of the recorded steps (C).
    double expected = 0.0;
    for (double e : mps.truncation_errors()) expected += e * e;
    EXPECT_NEAR(mps.total_truncation_error(), std::sqrt(expected), 1e-12);
    EXPECT_TRUE(mps.total_truncation_error() > qplayer::kMpsErrorBudgetWarn);
}

TEST(Mps, Truncation_CutoffBinds_DiscardsWithoutBondCap) {
    // Arrange: a generous bond cap, but a cutoff loose enough to bite. This is
    // the epsilon axis on its own — with only bond-cap tests, a cutoff rule that
    // never fires (the max/min slip) would go unnoticed.
    MpsTruncationConfig cfg;
    cfg.max_bond = qplayer::kMpsDefaultMaxBond;
    cfg.cutoff   = 0.8;
    MpsState mps(2, cfg);

    // Act: Bell has Schmidt values (1/sqrt2, 1/sqrt2), so dropping one leaves a
    // tail of 1/sqrt2 ~ 0.707, which is within the 0.8 cutoff.
    mps.apply_1q(h_matrix(), 0);
    mps.apply_2q(cx_matrix(), 0, 1);

    // Assert
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(1));
    EXPECT_NEAR(mps.truncation_errors()[0], kInvSqrt2, 1e-12);
}

TEST(Mps, Truncation_TightCutoff_KeepsBothSchmidtValues) {
    // Arrange: same state, cutoff just below the 1/sqrt2 tail.
    MpsTruncationConfig cfg;
    cfg.cutoff = 0.7;
    MpsState mps(2, cfg);

    // Act
    mps.apply_1q(h_matrix(), 0);
    mps.apply_2q(cx_matrix(), 0, 1);

    // Assert: 0.707 > 0.7, so neither value may be dropped.
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(2));
    EXPECT_NEAR(mps.truncation_errors()[0], 0.0, 1e-13);
}

TEST(Mps, Truncation_GenerousCap_KeepsStateExact) {
    // Arrange: the default cap is far above what a 4-qubit GHZ needs (chi = 2).
    MpsState mps(4);

    // Act
    mps.apply_1q(h_matrix(), 0);
    for (QubitIndex q = 0; q + 1 < 4; ++q) mps.apply_2q(cx_matrix(), q, q + 1);

    // Assert: every step error is at the numerical floor, not merely small.
    for (double e : mps.truncation_errors()) EXPECT_NEAR(e, 0.0, 1e-13);
    EXPECT_NEAR(mps.total_truncation_error(), 0.0, 1e-13);
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(2));
}

TEST(Mps, Truncation_LargerCapNeverIncreasesError) {
    // Arrange: same circuit under two caps.
    const QubitIndex n = 5;
    auto run = [n](std::uint32_t cap) {
        MpsTruncationConfig cfg;
        cfg.max_bond = cap;
        MpsState mps(n, cfg);
        for (QubitIndex q = 0; q < n; ++q) mps.apply_1q(h_matrix(), q);
        for (QubitIndex q = 0; q + 1 < n; ++q) mps.apply_2q(cx_matrix(), q, q + 1);
        for (QubitIndex q = n - 1; q > 0; --q) mps.apply_2q(cx_matrix(), q, q - 1);
        return mps.total_truncation_error();
    };

    // Act
    double tight = run(1);
    double loose = run(qplayer::kMpsDefaultMaxBond);

    // Assert: monotonicity is the falsifiable claim behind E — if a
    // bigger cap did not help, the truncation or renormalisation is wrong.
    EXPECT_TRUE(loose <= tight);
    EXPECT_NEAR(loose, 0.0, 1e-13);
}

// ── Dense export bound (11.7) ───────────────────────────────

TEST(Mps, ToDense_AboveExportCap_Throws) {
    // Arrange: 26 sites is one past the dense export bound.
    MpsState mps(qplayer::MAX_DENSE_EXPORT_QUBITS + 1);

    // Act + Assert
    EXPECT_THROW(mps.to_dense(), SimulatorError);
}

// ── Config resolution (D) ───────────────────────────────────────────

TEST(Mps, ConfigFromEnv_UnsetKeepsDefaults) {
    // Arrange
    unsetenv("QPLAYER_MPS_MAX_BOND");
    unsetenv("QPLAYER_MPS_CUTOFF");

    // Act
    MpsTruncationConfig cfg = MpsTruncationConfig::from_env();

    // Assert
    EXPECT_EQ(cfg.max_bond, qplayer::kMpsDefaultMaxBond);
    EXPECT_NEAR(cfg.cutoff, qplayer::kMpsDefaultCutoff, 1e-24);
}

TEST(Mps, ConfigFromEnv_ValidValuesOverride) {
    // Arrange
    setenv("QPLAYER_MPS_MAX_BOND", "64", 1);
    setenv("QPLAYER_MPS_CUTOFF", "1e-8", 1);

    // Act
    MpsTruncationConfig cfg = MpsTruncationConfig::from_env();

    // Assert
    EXPECT_EQ(cfg.max_bond, static_cast<std::uint32_t>(64));
    EXPECT_NEAR(cfg.cutoff, 1e-8, 1e-20);

    unsetenv("QPLAYER_MPS_MAX_BOND");
    unsetenv("QPLAYER_MPS_CUTOFF");
}

TEST(Mps, ConfigFromEnv_GarbageIsIgnoredNotFatal) {
    // Arrange: an env typo must not kill a long-running job (precedent).
    setenv("QPLAYER_MPS_MAX_BOND", "not-a-number", 1);
    setenv("QPLAYER_MPS_CUTOFF", "-1", 1);

    // Act
    MpsTruncationConfig cfg = MpsTruncationConfig::from_env();

    // Assert: defaults survive.
    EXPECT_EQ(cfg.max_bond, qplayer::kMpsDefaultMaxBond);
    EXPECT_NEAR(cfg.cutoff, qplayer::kMpsDefaultCutoff, 1e-24);

    unsetenv("QPLAYER_MPS_MAX_BOND");
    unsetenv("QPLAYER_MPS_CUTOFF");
}

TEST(Mps, ConfigFromEnv_AboveCeilingIsIgnored) {
    // Arrange
    setenv("QPLAYER_MPS_MAX_BOND", "999999", 1);

    // Act
    MpsTruncationConfig cfg = MpsTruncationConfig::from_env();

    // Assert: out-of-range env values fall back rather than throwing at startup.
    EXPECT_EQ(cfg.max_bond, qplayer::kMpsDefaultMaxBond);

    unsetenv("QPLAYER_MPS_MAX_BOND");
}

// ── Global norm gauge under truncation ────────────────────
//
// The per-gate rescale restores the merged BLOCK's Frobenius norm, which equals
// the global norm only on a canonical chain — and this chain is not canonical.
// These pin both halves of that fact: that the drift is real (so the guard must
// tolerate it) and that rescale() is the lever that undoes it.

TEST(Mps, Rescale_ScalesTheSquaredNormByTheFactorSquared) {
    // Arrange: an entangled chain, so the scale has to survive a real bond
    // structure rather than a single site tensor standing alone.
    MpsState st(4);
    st.apply_1q(h_matrix(), 0);
    st.apply_2q(cx_matrix(), 0, 1);
    st.apply_2q(cx_matrix(), 1, 2);
    EXPECT_NEAR(st.squared_norm(), 1.0, 1e-12);  // exact: no truncation happened

    // Act
    st.rescale(3.0);

    // Assert: |c psi|^2 = c^2 |psi|^2, and it is reversible.
    EXPECT_NEAR(st.squared_norm(), 9.0, 1e-12);
    st.rescale(1.0 / 3.0);
    EXPECT_NEAR(st.squared_norm(), 1.0, 1e-12);
}

TEST(Mps, Rescale_NonFiniteFactor_Throws) {
    MpsState st(2);
    EXPECT_THROW(st.rescale(std::nan("")), SimulatorError);
    EXPECT_THROW(st.rescale(HUGE_VAL), SimulatorError);
}

TEST(Mps, BondCapTruncation_WithoutGauge_DriftsTheGlobalNorm_AndRescaleRestoresIt) {
    // Arrange: 14 qubits of brickwork with rotations between the CX layers, run
    // at chi_max 4. The rotations matter — a Clifford brickwork on |0...0> stays
    // at a small chi and never forces the cap.
    //
    // Two things are asserted, and the first is the surprising one: a chain the
    // backend renormalises at every SVD still ends up away from 1, because that
    // renormalisation is local. showed the canonical gauge removes this
    // at its source but does not pay for itself in QPlayer's gate order, so the
    // drift stands and 's end-of-trajectory correction stays load-bearing.
    MpsTruncationConfig cfg;
    cfg.max_bond = 4;
    cfg.cutoff   = 1e-12;
    cfg.gauge    = qplayer::MpsGauge::None;   // the pre- split; the gauge removes this drift
    MpsState st(14, cfg);
    const auto ry = [](double t) {
        return std::vector<Cx>{{std::cos(t / 2), 0}, {-std::sin(t / 2), 0},
                               {std::sin(t / 2),  0}, {std::cos(t / 2),  0}};
    };
    for (int layer = 0; layer < 10; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < 14; q += 2) {
            st.apply_1q(ry(0.63), q);
            st.apply_1q(ry(0.41), q + 1);
            st.apply_2q(cx_matrix(), q, q + 1);
        }
    }

    // Act
    const double drifted = st.squared_norm();

    // Assert: the cap was hit, weight was lost, and the norm left the 1e-6 band
    // the old divergence guard demanded — which is why that guard had to split.
    EXPECT_EQ(st.max_bond_dimension(), std::uint32_t{4});
    EXPECT_TRUE(st.total_truncation_error() > 0.1);
    EXPECT_TRUE(std::abs(drifted - 1.0) > 1e-6);

    st.rescale(1.0 / std::sqrt(drifted));
    EXPECT_NEAR(st.squared_norm(), 1.0, 1e-9);
}

TEST(Mps, LosslessTruncation_KeepsTheGlobalNormAtOne) {
    // Arrange: the same circuit shape with room to spare. This is the control for
    // the test above — without it, "the norm drifts" could equally be read as
    // "the norm is never reliable", and then the tightened lossless branch of the
    // guard (still 1e-6) would have no justification.
    MpsTruncationConfig cfg;
    cfg.max_bond = 4096;
    cfg.cutoff   = 1e-12;
    MpsState st(14, cfg);
    const auto ry = [](double t) {
        return std::vector<Cx>{{std::cos(t / 2), 0}, {-std::sin(t / 2), 0},
                               {std::sin(t / 2),  0}, {std::cos(t / 2),  0}};
    };
    for (int layer = 0; layer < 10; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < 14; q += 2) {
            st.apply_1q(ry(0.63), q);
            st.apply_1q(ry(0.41), q + 1);
            st.apply_2q(cx_matrix(), q, q + 1);
        }
    }

    // Assert
    EXPECT_NEAR(st.total_truncation_error(), 0.0, 1e-15);
    EXPECT_NEAR(st.squared_norm(), 1.0, 1e-9);
}

// ── Peak bond dimension ───────────────────────────────────────────

TEST(Mps, PeakBondDimension_ProductStateIsOne) {
    // Arrange + Act: no 2Q gate, so no bond ever grows.
    MpsState st(3);
    st.apply_1q(h_matrix(), 0);
    st.apply_1q(x_matrix(), 2);

    // Assert
    EXPECT_EQ(st.peak_bond_dimension(), static_cast<std::uint32_t>(1));
}

TEST(Mps, PeakBondDimension_SurvivesABondThatShrinksBack) {
    // Arrange: entangle q0-q1 into a Bell pair, then undo it with a second CX.
    // The chain ends as a product state, so max_bond_dimension() reads 1 again --
    // which is precisely the premise corrects: the run needed 2.
    MpsState st(3);
    st.apply_1q(h_matrix(), 0);
    st.apply_2q(cx_matrix(), 0, 1);
    const std::uint32_t while_entangled = st.max_bond_dimension();

    // Act
    st.apply_2q(cx_matrix(), 0, 1);

    // Assert: the snapshot forgets, the peak does not.
    EXPECT_EQ(while_entangled, static_cast<std::uint32_t>(2));
    EXPECT_EQ(st.max_bond_dimension(), static_cast<std::uint32_t>(1));
    EXPECT_EQ(st.peak_bond_dimension(), static_cast<std::uint32_t>(2));
}

TEST(Mps, PeakBondDimension_IsNeverBelowTheCurrentBond) {
    // Arrange: a chain entangled across two cuts, so several bonds are above 1.
    MpsState st(4);
    st.apply_1q(h_matrix(), 0);
    st.apply_2q(cx_matrix(), 0, 1);
    st.apply_1q(h_matrix(), 2);
    st.apply_2q(cx_matrix(), 2, 3);

    // Act + Assert: an invariant, not a measured number -- it must hold whatever
    // the site layout does to the individual bonds.
    EXPECT_TRUE(st.peak_bond_dimension() >= st.max_bond_dimension());
    EXPECT_EQ(st.peak_bond_dimension(), st.max_bond_dimension());
}

TEST(Mps, PeakBondDimension_CountsTheSwapWalkOfANonAdjacentGate) {
    // Arrange: q0 and q3 are not adjacent, so apply_2q walks SWAPs there and
    // back. The walk entangles the sites it passes and then undoes it, which is
    // one of the two mechanisms that make the end-of-run bond an understatement.
    MpsState st(4);
    st.apply_1q(h_matrix(), 0);

    // Act
    st.apply_2q(cx_matrix(), 0, 3);

    // Assert: the peak is at least the surviving bond, and the walk is visible
    // in it. The exact value depends on the SWAP schedule, so the assertion is
    // the inequality the diagnostic promises, not a layout-specific number.
    EXPECT_TRUE(st.peak_bond_dimension() >= 2);
    EXPECT_TRUE(st.peak_bond_dimension() >= st.max_bond_dimension());
}

TEST(Mps, ConfigFromEnv_GaugeValues) {
    // Arrange / Act / Assert: the two spellings switch the gauge, a typo keeps
    // the default rather than failing the run (precedent).
    setenv("QPLAYER_MPS_GAUGE", "none", 1);
    EXPECT_TRUE(MpsTruncationConfig::from_env().gauge == qplayer::MpsGauge::None);
    setenv("QPLAYER_MPS_GAUGE", "canonical", 1);
    EXPECT_TRUE(MpsTruncationConfig::from_env().gauge == qplayer::MpsGauge::Canonical);
    setenv("QPLAYER_MPS_GAUGE", "sideways", 1);
    EXPECT_TRUE(MpsTruncationConfig::from_env().gauge == qplayer::kMpsDefaultGauge);
    unsetenv("QPLAYER_MPS_GAUGE");
}
