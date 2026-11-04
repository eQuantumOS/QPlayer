// test_mps_canonical.cpp — MpsState::canonicalize().
// 1.1, 1.3 (Arrange/Act/Assert).
//
// Canonicalisation changes the GAUGE, not the state. That is the whole contract,
// and it is why the decisive assertion here is `to_dense()` invariance: any
// mistake in the sweep -- a transposed factor, a missed conjugate, a carry
// folded the wrong way -- moves amplitudes, and no orthonormality check would
// have to notice. The orthonormality assertions check the MECHANISM; the dense
// comparison checks the OUTCOME, and the outcome is what callers depend on.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <vector>

#include "../../../include/backend/tensor_network/mps.hpp"

using qplayer::QubitIndex;
using qplayer::detail::MpsState;
using qplayer::detail::mps_is_left_orthonormal;
using qplayer::detail::mps_is_right_orthonormal;

namespace {

using Cx = std::complex<double>;

const std::vector<Cx> kH = {{M_SQRT1_2, 0}, {M_SQRT1_2, 0}, {M_SQRT1_2, 0}, {-M_SQRT1_2, 0}};
const std::vector<Cx> kCz = {{1, 0}, {0, 0}, {0, 0}, {0, 0},
                             {0, 0}, {1, 0}, {0, 0}, {0, 0},
                             {0, 0}, {0, 0}, {1, 0}, {0, 0},
                             {0, 0}, {0, 0}, {0, 0}, {-1, 0}};

std::vector<Cx> rx(double t) {
    const double c = std::cos(t / 2), s = std::sin(t / 2);
    return {{c, 0}, {0, -s}, {0, -s}, {c, 0}};
}

// Entangling enough that bonds grow past 1 and the sweep has real work to do.
MpsState entangled(QubitIndex n, int depth) {
    MpsState st(n);
    for (QubitIndex q = 0; q < n; ++q) st.apply_1q(kH, q);
    double a = 0.3;
    for (int d = 0; d < depth; ++d) {
        for (QubitIndex q = d % 2; q + 1 < n; q += 2) st.apply_2q(kCz, q, q + 1);
        for (QubitIndex q = 0; q < n; ++q) { st.apply_1q(rx(a), q); a += 0.11; }
    }
    return st;
}

double max_abs_delta(const std::vector<Cx>& a, const std::vector<Cx>& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

}  // namespace

// The contract. If this fails, the sweep is wrong no matter what else passes.
TEST(MpsCanonical, Canonicalize_LeavesTheStateUnchanged) {
    // Arrange
    MpsState st = entangled(8, 4);
    const std::vector<Cx> before = st.to_dense();

    // Act
    st.canonicalize(4);

    // Assert — gauge moved, state did not.
    const std::vector<Cx> after = st.to_dense();
    EXPECT_EQ(before.size(), after.size());
    EXPECT_NEAR(max_abs_delta(before, after), 0.0, 1e-12);  // 수식 유도 — 반올림만 허용
}

// Every centre position has to work, not just the middle one: the sweeps have
// different trip counts at the ends and an off-by-one there would survive a
// single mid-chain test.
TEST(MpsCanonical, EveryCentre_PreservesTheState) {
    for (QubitIndex centre = 0; centre < 6; ++centre) {
        MpsState st = entangled(6, 3);
        const std::vector<Cx> before = st.to_dense();
        st.canonicalize(centre);
        EXPECT_NEAR(max_abs_delta(before, st.to_dense()), 0.0, 1e-12);
        EXPECT_EQ(st.orthogonality_centre(), centre);
    }
}

// The mechanism: sites left of the centre are left-orthonormal, sites right of
// it right-orthonormal. This is what makes a marginal at the centre local and a
// truncation there globally optimal.
TEST(MpsCanonical, Canonicalize_EstablishesTheOrthonormalityPattern) {
    // Arrange
    MpsState st = entangled(7, 4);

    // Act
    st.canonicalize(3);

    // Assert
    for (std::size_t k = 0; k < 3; ++k) EXPECT_TRUE(mps_is_left_orthonormal(st.graph(), k));
    for (std::size_t k = 4; k < 7; ++k) EXPECT_TRUE(mps_is_right_orthonormal(st.graph(), k));
}

// With the gauge held, the whole norm sits on the centre tensor -- which is the
// property that removes the drift and makes squared_norm() O(chi^2).
TEST(MpsCanonical, Canonicalize_PutsTheWholeNormOnTheCentre) {
    // Arrange
    MpsState st = entangled(7, 4);
    const double total = st.squared_norm();

    // Act
    st.canonicalize(2);

    // Assert
    const auto& node = st.graph().node(2);
    double centre_norm = 0.0;
    for (const Cx& v : node.data) centre_norm += std::norm(v);
    EXPECT_NEAR(centre_norm, total, 1e-10);      // 수치 오차 허용
    EXPECT_NEAR(st.squared_norm(), total, 1e-10);
}

// A product state is already canonical in content but may carry redundant bond
// rank; the sweep must not inflate it.
TEST(MpsCanonical, ProductState_KeepsBondsAtOne) {
    // Arrange
    MpsState st(5);
    for (QubitIndex q = 0; q < 5; ++q) st.apply_1q(kH, q);

    // Act
    st.canonicalize(2);

    // Assert
    EXPECT_EQ(st.max_bond_dimension(), 1u);
    EXPECT_NEAR(st.squared_norm(), 1.0, 1e-12);
}

TEST(MpsCanonical, CentreOutOfRange_Throws) {
    MpsState st(4);
    EXPECT_THROW(st.canonicalize(4), qplayer::SimulatorError);
}

// ── : the gauge maintained through the gate path ────────────────────

namespace {

using qplayer::detail::MpsTruncationConfig;

// Every site left of the centre left-orthonormal, every site right of it
// right-orthonormal -- the property that makes a split at the centre optimal.
bool mixed_canonical_about(const MpsState& st, QubitIndex c) {
    for (QubitIndex k = 0; k < c; ++k)
        if (!mps_is_left_orthonormal(st.graph(), k, 1e-9)) return false;
    for (QubitIndex k = c + 1; k < st.num_qubits(); ++k)
        if (!mps_is_right_orthonormal(st.graph(), k, 1e-9)) return false;
    return true;
}

// Brickwork plus long-range CZs, so both adjacent splits and SWAP walks in
// either direction are exercised.
void scramble(MpsState& st) {
    const QubitIndex n = st.num_qubits();
    for (QubitIndex q = 0; q < n; ++q) st.apply_1q(kH, q);
    double a = 0.37;
    for (int d = 0; d < 6; ++d) {
        for (QubitIndex q = d % 2; q + 1 < n; q += 2) st.apply_2q(kCz, q, q + 1);
        st.apply_2q(kCz, 0, n - 1);
        st.apply_2q(kCz, n - 2, 1);
        for (QubitIndex q = 0; q < n; ++q) { st.apply_1q(rx(a), q); a += 0.13; }
    }
}

using qplayer::MpsGauge;

MpsTruncationConfig gauge_config(MpsGauge gauge, std::uint32_t max_bond) {
    MpsTruncationConfig cfg;
    cfg.max_bond = max_bond;
    cfg.cutoff   = 1e-12;
    cfg.gauge    = gauge;
    return cfg;
}

}  // namespace

TEST(MpsCanonical, Gauge_TwoQubitGatesLeaveAMixedCanonicalChain) {
    // Arrange
    MpsState st(10, gauge_config(MpsGauge::Canonical, 256));

    // Act
    scramble(st);

    // Assert: a centre is held, and the chain really is canonical about it.
    const QubitIndex c = st.orthogonality_centre();
    EXPECT_TRUE(c < st.num_qubits());
    EXPECT_TRUE(mixed_canonical_about(st, c));
}

TEST(MpsCanonical, Gauge_LeavesALosslessStateUnchanged) {
    // Arrange: nothing is truncated, so the gauge may only change the factors.
    MpsState with(10, gauge_config(MpsGauge::Canonical, 1024));
    MpsState without(10, gauge_config(MpsGauge::None, 1024));

    // Act
    scramble(with);
    scramble(without);

    // Assert: same amplitudes (1e-10: accumulated SVD rounding over ~100 splits).
    EXPECT_TRUE(max_abs_delta(with.to_dense(), without.to_dense()) < 1e-10);
}

TEST(MpsCanonical, Gauge_LossyTruncationStaysCloserToTheExactState) {
    // Arrange: a cap far below what the circuit needs, against an uncapped run.
    MpsState exact(12, gauge_config(MpsGauge::Canonical, 4096));
    MpsState with(12, gauge_config(MpsGauge::Canonical, 4));
    MpsState without(12, gauge_config(MpsGauge::None, 4));
    scramble(exact);

    // Act
    scramble(with);
    scramble(without);

    // Assert: the split at the centre discards the globally smallest weight, so
    // it ends nearer the exact state; the local split does not.
    const auto fidelity = [&](const MpsState& st) {
        const std::vector<Cx> a = exact.to_dense(), b = st.to_dense();
        Cx dot{0, 0};
        double na = 0.0, nb = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            dot += std::conj(a[i]) * b[i];
            na += std::norm(a[i]);
            nb += std::norm(b[i]);
        }
        return std::norm(dot) / (na * nb);
    };
    EXPECT_TRUE(with.total_truncation_error() > 0.0);
    EXPECT_TRUE(fidelity(with) > fidelity(without));
}

TEST(MpsCanonical, AutoGauge_LosslessRunIsBitIdenticalToNone) {
    // Arrange: nothing is discarded, so Auto must never arm.
    MpsState autog(10, gauge_config(MpsGauge::Auto, 1024));
    MpsState none(10, gauge_config(MpsGauge::None, 1024));

    // Act
    scramble(autog);
    scramble(none);

    // Assert: exactly the same arithmetic, hence exactly the same amplitudes.
    EXPECT_TRUE(max_abs_delta(autog.to_dense(), none.to_dense()) == 0.0);
    EXPECT_EQ(autog.orthogonality_centre(), autog.num_qubits());
}

TEST(MpsCanonical, AutoGauge_ArmsAtTheFirstLossAndMatchesCanonical) {
    // Arrange: the lossy cap of the test above.
    MpsState exact(12, gauge_config(MpsGauge::Canonical, 4096));
    MpsState autog(12, gauge_config(MpsGauge::Auto, 4));
    MpsState none(12, gauge_config(MpsGauge::None, 4));
    scramble(exact);

    // Act
    scramble(autog);
    scramble(none);

    // Assert: armed (a centre is held) and nearer the exact state than None.
    const auto overlap = [&](const MpsState& st) {
        const std::vector<Cx> a = exact.to_dense(), b = st.to_dense();
        Cx dot{0, 0};
        double na = 0.0, nb = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            dot += std::conj(a[i]) * b[i];
            na += std::norm(a[i]);
            nb += std::norm(b[i]);
        }
        return std::norm(dot) / (na * nb);
    };
    EXPECT_TRUE(autog.orthogonality_centre() < autog.num_qubits());
    EXPECT_TRUE(overlap(autog) > overlap(none));
}

TEST(MpsCanonical, Gauge_NonUnitaryFactorIsAbsorbedAtTheCentre) {
    // Arrange: a projector onto |0> on a site away from the current centre.
    MpsState st(10, gauge_config(MpsGauge::Canonical, 256));
    scramble(st);
    const QubitIndex target = (st.orthogonality_centre() < 5) ? 8 : 1;
    const std::vector<Cx> p0 = {{1, 0}, {0, 0}, {0, 0}, {0, 0}};

    // Act
    st.apply_1q(p0, target);

    // Assert: the centre moved there first, so the gauge survived the factor.
    EXPECT_EQ(st.orthogonality_centre(), target);
    EXPECT_TRUE(mixed_canonical_about(st, target));
}
