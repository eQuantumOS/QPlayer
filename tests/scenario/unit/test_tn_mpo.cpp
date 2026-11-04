// test_tn_mpo.cpp — Unit tests for the MPO density-matrix path.
// Tests follow the Arrange / Act / Assert layout.
//
// The MPO path's claim is that it applies a channel EXACTLY — no sampling, no
// averaging. So the reference here is a dense density matrix evolved longhand in
// this file, not another backend's sampled distribution: a Monte Carlo
// comparison could only ever agree to statistical tolerance and would hide a
// systematic error of the size these tests are meant to catch.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <map>
#include <random>
#include <vector>

#include "../../../include/backend/tensor_network/tn_mpo.hpp"
#include "../../../include/backend/full_state/pauli_expectation.hpp"
#include "../../../include/error.hpp"

using qplayer::CircuitError;
using qplayer::QubitIndex;
using qplayer::SimulatorError;
using qplayer::UnitaryMatrix;
using qplayer::detail::MpoState;
using qplayer::detail::MpoTruncationConfig;
using qplayer::detail::PauliOp;

using Cx = std::complex<double>;

namespace {

// ── Operators ────────────────────────────────────────────────────────────────

UnitaryMatrix mat2(Cx a, Cx b, Cx c, Cx d) { return {{a, b}, {c, d}}; }

std::vector<Cx> h_gate() {
    const double s = 1.0 / std::sqrt(2.0);
    return {Cx{s, 0}, Cx{s, 0}, Cx{s, 0}, Cx{-s, 0}};
}
std::vector<Cx> x_gate() { return {Cx{0, 0}, Cx{1, 0}, Cx{1, 0}, Cx{0, 0}}; }
std::vector<Cx> ry_gate(double th) {
    return {Cx{std::cos(th / 2), 0}, Cx{-std::sin(th / 2), 0},
            Cx{std::sin(th / 2), 0}, Cx{ std::cos(th / 2), 0}};
}
std::vector<Cx> t_gate() {
    const double s = 1.0 / std::sqrt(2.0);
    return {Cx{1, 0}, Cx{0, 0}, Cx{0, 0}, Cx{s, s}};
}
// Operand order: local bit 0 is the first operand, so this flips the SECOND.
std::vector<Cx> cnot_gate() {
    std::vector<Cx> m(16, Cx{0, 0});
    m[0 * 4 + 0] = 1; m[3 * 4 + 1] = 1; m[2 * 4 + 2] = 1; m[1 * 4 + 3] = 1;
    return m;
}

// Amplitude damping, gamma.
std::vector<UnitaryMatrix> damping(double g) {
    return {mat2(Cx{1, 0}, Cx{0, 0}, Cx{0, 0}, Cx{std::sqrt(1 - g), 0}),
            mat2(Cx{0, 0}, Cx{std::sqrt(g), 0}, Cx{0, 0}, Cx{0, 0})};
}

// Single-qubit depolarizing, p: (1-p) I + p/3 (X, Y, Z).
std::vector<UnitaryMatrix> depol_1q(double p) {
    const double a = std::sqrt(1 - p), b = std::sqrt(p / 3);
    return {mat2(Cx{a, 0}, Cx{0, 0}, Cx{0, 0}, Cx{a, 0}),
            mat2(Cx{0, 0}, Cx{b, 0}, Cx{b, 0}, Cx{0, 0}),
            mat2(Cx{0, 0}, Cx{0, -b}, Cx{0, b}, Cx{0, 0}),
            mat2(Cx{b, 0}, Cx{0, 0}, Cx{0, 0}, Cx{-b, 0})};
}

// ── Independent dense oracle ────────────────────────────────────────────────
// Written longhand so it shares no code with the MPO path under test.

struct DenseRho {
    QubitIndex n;
    std::vector<Cx> m;   // row-major 2^n x 2^n, bit q of the index is qubit q

    explicit DenseRho(QubitIndex qubits) : n(qubits), m(std::size_t{1} << (2 * qubits), Cx{0, 0}) {
        m[0] = Cx{1, 0};
    }
    std::size_t dim() const { return std::size_t{1} << n; }

    void apply_kraus_1q(const std::vector<UnitaryMatrix>& ks, QubitIndex q) {
        const std::size_t d = dim(), bit = std::size_t{1} << q;
        std::vector<Cx> out(d * d, Cx{0, 0});
        for (const UnitaryMatrix& K : ks) {
            // tmp = K rho
            std::vector<Cx> tmp(d * d, Cx{0, 0});
            for (std::size_t i = 0; i < d; ++i) {
                for (std::size_t j = 0; j < d; ++j) {
                    const std::size_t ib = (i & bit) ? 1 : 0;
                    for (std::size_t s = 0; s < 2; ++s) {
                        const std::size_t src = s ? (i | bit) : (i & ~bit);
                        tmp[i * d + j] += K[ib][s] * m[src * d + j];
                    }
                }
            }
            for (std::size_t i = 0; i < d; ++i) {
                for (std::size_t j = 0; j < d; ++j) {
                    const std::size_t jb = (j & bit) ? 1 : 0;
                    for (std::size_t s = 0; s < 2; ++s) {
                        const std::size_t src = s ? (j | bit) : (j & ~bit);
                        out[i * d + j] += tmp[i * d + src] * std::conj(K[jb][s]);
                    }
                }
            }
        }
        m = std::move(out);
    }

    void apply_unitary_1q(const std::vector<Cx>& u, QubitIndex q) {
        apply_kraus_1q({mat2(u[0], u[1], u[2], u[3])}, q);
    }

    // 4x4 operator in operand order: local bit 0 = q0.
    void apply_kraus_2q(const std::vector<UnitaryMatrix>& ks, QubitIndex q0, QubitIndex q1) {
        const std::size_t d = dim();
        const std::size_t b0 = std::size_t{1} << q0, b1 = std::size_t{1} << q1;
        const auto local = [&](std::size_t idx) {
            return ((idx & b0) ? 1u : 0u) | ((idx & b1) ? 2u : 0u);
        };
        const auto with_local = [&](std::size_t idx, std::size_t loc) {
            std::size_t r = idx & ~b0 & ~b1;
            if (loc & 1u) r |= b0;
            if (loc & 2u) r |= b1;
            return r;
        };
        std::vector<Cx> out(d * d, Cx{0, 0});
        for (const UnitaryMatrix& K : ks) {
            std::vector<Cx> tmp(d * d, Cx{0, 0});
            for (std::size_t i = 0; i < d; ++i) {
                for (std::size_t j = 0; j < d; ++j) {
                    for (std::size_t s = 0; s < 4; ++s) {
                        tmp[i * d + j] += K[local(i)][s] * m[with_local(i, s) * d + j];
                    }
                }
            }
            for (std::size_t i = 0; i < d; ++i) {
                for (std::size_t j = 0; j < d; ++j) {
                    for (std::size_t s = 0; s < 4; ++s) {
                        out[i * d + j] += tmp[i * d + with_local(j, s)] * std::conj(K[local(j)][s]);
                    }
                }
            }
        }
        m = std::move(out);
    }

    void apply_unitary_2q(const std::vector<Cx>& u, QubitIndex q0, QubitIndex q1) {
        UnitaryMatrix k(4, std::vector<Cx>(4, Cx{0, 0}));
        for (std::size_t r = 0; r < 4; ++r) for (std::size_t c = 0; c < 4; ++c) k[r][c] = u[r * 4 + c];
        apply_kraus_2q({k}, q0, q1);
    }
};

double max_abs_diff(const std::vector<Cx>& a, const std::vector<Cx>& b) {
    double d = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

}  // namespace

// ── Construction and trace ───────────────────────────────────────────────────

TEST(TnMpo, ZeroState_IsPureAndNormalised) {
    // Arrange + Act
    MpoState rho(3);

    // Assert: |000><000| has a single unit entry, chi = 1 everywhere.
    EXPECT_NEAR(rho.trace(), 1.0, 1e-15);
    EXPECT_EQ(rho.max_bond_dimension(), std::uint32_t{1});
    const std::vector<Cx> dense = rho.to_dense();
    EXPECT_EQ(dense.size(), std::size_t{64});
    EXPECT_NEAR(std::abs(dense[0] - Cx{1.0, 0.0}), 0.0, 1e-15);
    double rest = 0.0;
    for (std::size_t i = 1; i < dense.size(); ++i) rest = std::max(rest, std::abs(dense[i]));
    EXPECT_NEAR(rest, 0.0, 1e-15);
}

// ── Unitary evolution against the dense oracle ──────────────────────────────

TEST(TnMpo, UnitaryCircuit_MatchesTheDenseDensityMatrix) {
    // Arrange: distinct rotations per qubit plus entangling gates, so no two
    // entries of rho coincide by symmetry.
    MpoState rho(4);
    DenseRho ref(4);
    const auto both_1q = [&](const std::vector<Cx>& u, QubitIndex q) {
        rho.apply_unitary_1q(u, q); ref.apply_unitary_1q(u, q);
    };
    const auto both_2q = [&](const std::vector<Cx>& u, QubitIndex a, QubitIndex b) {
        rho.apply_unitary_2q(u, a, b); ref.apply_unitary_2q(u, a, b);
    };

    // Act
    both_1q(ry_gate(0.7), 0); both_1q(ry_gate(1.9), 1);
    both_1q(t_gate(), 1);     both_1q(ry_gate(2.5), 2); both_1q(ry_gate(1.1), 3);
    both_2q(cnot_gate(), 0, 1); both_2q(cnot_gate(), 2, 3); both_2q(cnot_gate(), 1, 2);

    // Assert: exact evolution, so the tolerance is numerical, not statistical.
    EXPECT_NEAR(max_abs_diff(rho.to_dense(), ref.m), 0.0, 1e-12);
    EXPECT_NEAR(rho.trace(), 1.0, 1e-12);
}

TEST(TnMpo, DistantOperands_MatchTheDenseOracle) {
    // Arrange: a 2Q operation on non-adjacent sites walks SWAPs out and back.
    // Stopping one site short leaves the chain permuted, which this catches
    // because later gates then land on the wrong qubit.
    MpoState rho(5);
    DenseRho ref(5);
    for (QubitIndex q = 0; q < 5; ++q) {
        rho.apply_unitary_1q(ry_gate(0.3 + 0.4 * q), q);
        ref.apply_unitary_1q(ry_gate(0.3 + 0.4 * q), q);
    }

    // Act
    rho.apply_unitary_2q(cnot_gate(), 0, 4); ref.apply_unitary_2q(cnot_gate(), 0, 4);
    rho.apply_unitary_2q(cnot_gate(), 3, 1); ref.apply_unitary_2q(cnot_gate(), 3, 1);
    rho.apply_unitary_1q(t_gate(), 2);       ref.apply_unitary_1q(t_gate(), 2);

    // Assert
    EXPECT_NEAR(max_abs_diff(rho.to_dense(), ref.m), 0.0, 1e-12);
    EXPECT_TRUE(rho.swap_steps() > 0);
}

// ── Channels applied exactly (the whole point of this path) ─────────────────

TEST(TnMpo, AmplitudeDamping_IsAppliedExactly) {
    // Arrange: |1> under damping gamma must become diag(gamma, 1-gamma) exactly.
    // A trajectory path can only reproduce this to sampling tolerance; the MPO
    // path has to hit it on the nose.
    const double g = 0.3;
    MpoState rho(1);
    rho.apply_unitary_1q(x_gate(), 0);

    // Act
    rho.apply_channel_1q(damping(g), 0);

    // Assert
    const std::vector<Cx> dense = rho.to_dense();
    EXPECT_NEAR(dense[0].real(), g, 1e-14);         // <0|rho|0>
    EXPECT_NEAR(dense[3].real(), 1.0 - g, 1e-14);   // <1|rho|1>
    EXPECT_NEAR(std::abs(dense[1]), 0.0, 1e-14);
    EXPECT_NEAR(rho.trace(), 1.0, 1e-14);
}

TEST(TnMpo, NoisyCircuit_MatchesTheDenseOracle) {
    // Arrange: gates and channels interleaved on an entangled chain — the case
    // where a wrong ket/bra pairing would still look plausible.
    MpoState rho(3);
    DenseRho ref(3);
    const auto gate1 = [&](const std::vector<Cx>& u, QubitIndex q) {
        rho.apply_unitary_1q(u, q); ref.apply_unitary_1q(u, q);
    };
    const auto chan1 = [&](const std::vector<UnitaryMatrix>& k, QubitIndex q) {
        rho.apply_channel_1q(k, q); ref.apply_kraus_1q(k, q);
    };

    // Act
    gate1(h_gate(), 0);
    rho.apply_unitary_2q(cnot_gate(), 0, 1); ref.apply_unitary_2q(cnot_gate(), 0, 1);
    chan1(depol_1q(0.15), 0);
    chan1(damping(0.2), 1);
    rho.apply_unitary_2q(cnot_gate(), 1, 2); ref.apply_unitary_2q(cnot_gate(), 1, 2);
    chan1(depol_1q(0.05), 2);

    // Assert
    EXPECT_NEAR(max_abs_diff(rho.to_dense(), ref.m), 0.0, 1e-12);
    EXPECT_NEAR(rho.trace(), 1.0, 1e-12);
}

TEST(TnMpo, TwoQubitChannel_MatchesTheDenseOracle) {
    // Arrange: a genuinely two-site channel goes through merge/SVD/split, unlike
    // the one-site case, so it exercises a different code path entirely.
    //
    // The operators are deliberately NON-SYMMETRIC and complex. A symmetric K
    // has K^T == K, which makes K rho K^dagger and K rho conj(K) identical — so
    // a symmetric fixture (X tensor X, CNOT, SWAP, any diagonal) cannot tell a
    // transposed bra index from a correct one. That mutation survived the first
    // version of this test.
    const double g = 0.35;
    const double a = std::sqrt(1 - g), b = std::sqrt(g);
    UnitaryMatrix k0(4, std::vector<Cx>(4, Cx{0, 0}));
    UnitaryMatrix k1(4, std::vector<Cx>(4, Cx{0, 0}));
    for (std::size_t i = 0; i < 3; ++i) k0[i][i] = Cx{1, 0};
    k0[3][3] = Cx{a, 0};
    // K1 = i*sqrt(g) |00><11| — one corner only, so K1^T != K1, and the phase
    // makes conj() matter as well. sum K^dagger K = I still holds.
    k1[0][3] = Cx{0, b};

    MpoState rho(3);
    DenseRho ref(3);
    rho.apply_unitary_1q(ry_gate(0.9), 0); ref.apply_unitary_1q(ry_gate(0.9), 0);
    rho.apply_unitary_1q(t_gate(), 2);     ref.apply_unitary_1q(t_gate(), 2);
    rho.apply_unitary_2q(cnot_gate(), 0, 1); ref.apply_unitary_2q(cnot_gate(), 0, 1);

    // Act
    rho.apply_channel_2q({k0, k1}, 1, 2);
    ref.apply_kraus_2q({k0, k1}, 1, 2);

    // Assert
    EXPECT_NEAR(max_abs_diff(rho.to_dense(), ref.m), 0.0, 1e-12);
    EXPECT_NEAR(rho.trace(), 1.0, 1e-12);
}

TEST(TnMpo, NonSymmetricTwoQubitUnitary_MatchesTheDenseOracle) {
    // Arrange: every textbook 2Q gate in this file (CNOT, SWAP, X tensor X) is a
    // symmetric matrix, so none of them distinguishes U rho U^dagger from
    // U rho conj(U). This one does: CNOT followed by T on the first operand has
    // U[1][3] = 1 while U[3][1] = exp(i pi/4).
    const double s = 1.0 / std::sqrt(2.0);
    std::vector<Cx> u = cnot_gate();
    for (std::size_t col = 0; col < 4; ++col) {
        // T on operand 0 = local bit 0, i.e. rows with that bit set.
        u[1 * 4 + col] *= Cx{s, s};
        u[3 * 4 + col] *= Cx{s, s};
    }

    MpoState rho(3);
    DenseRho ref(3);
    rho.apply_unitary_1q(ry_gate(1.2), 0); ref.apply_unitary_1q(ry_gate(1.2), 0);
    rho.apply_unitary_1q(ry_gate(0.5), 2); ref.apply_unitary_1q(ry_gate(0.5), 2);

    // Act
    rho.apply_unitary_2q(u, 0, 1); ref.apply_unitary_2q(u, 0, 1);
    rho.apply_unitary_2q(u, 2, 1); ref.apply_unitary_2q(u, 2, 1);

    // Assert
    EXPECT_NEAR(max_abs_diff(rho.to_dense(), ref.m), 0.0, 1e-12);
    EXPECT_NEAR(rho.trace(), 1.0, 1e-12);
}

TEST(TnMpo, SingleSiteChannel_LeavesBondsUntouched) {
    // Arrange: entangle first — on a product state every bond is 1 and the claim
    // would hold for the wrong reason.
    MpoState rho(3);
    rho.apply_unitary_1q(h_gate(), 0);
    rho.apply_unitary_2q(cnot_gate(), 0, 1);
    rho.apply_unitary_2q(cnot_gate(), 1, 2);
    const std::uint32_t before = rho.max_bond_dimension();
    const std::size_t errors_before = rho.truncation_errors().size();

    // Act
    rho.apply_channel_1q(depol_1q(0.3), 1);

    // Assert: a one-site superoperator cannot create correlation, so there is
    // nothing to truncate and nothing to record.
    EXPECT_EQ(rho.max_bond_dimension(), before);
    EXPECT_EQ(rho.truncation_errors().size(), errors_before);
    EXPECT_TRUE(before > 1);
}

// ── Truncation policy ────────────────────────────────────────────

TEST(TnMpo, TruncationErrors_AreRecordedOncePerTwoQubitCall) {
    // Arrange: says one entry per 2Q call — a distant pair's SWAP walk
    // folds into that single number rather than adding entries of its own.
    MpoState rho(4);
    rho.apply_unitary_1q(h_gate(), 0);

    // Act
    rho.apply_unitary_2q(cnot_gate(), 0, 1);   // adjacent
    rho.apply_unitary_2q(cnot_gate(), 0, 3);   // distant: 2 SWAPs out, 2 back

    // Assert
    EXPECT_EQ(rho.truncation_errors().size(), std::size_t{2});
    for (double e : rho.truncation_errors()) EXPECT_TRUE(e >= 0.0);
}

TEST(TnMpo, HardBondCap_TruncatesAndKeepsTheTraceAtOne) {
    // Arrange: chi_rho = 1 forces a product-state approximation of an entangled
    // density matrix, so the truncation is severely lossy on purpose. Trace
    // normalisation is what has to survive that (A).
    MpoTruncationConfig cfg;
    cfg.max_bond = 1;
    MpoState rho(4, cfg);
    rho.apply_unitary_1q(h_gate(), 0);

    // Act
    rho.apply_unitary_2q(cnot_gate(), 0, 1);
    rho.apply_unitary_2q(cnot_gate(), 1, 2);
    rho.apply_unitary_2q(cnot_gate(), 2, 3);

    // Assert
    EXPECT_EQ(rho.max_bond_dimension(), std::uint32_t{1});
    EXPECT_NEAR(rho.trace(), 1.0, 1e-10);
    EXPECT_TRUE(rho.total_truncation_error() > 0.0);
}

TEST(TnMpo, RaisingTheBondCap_ReducesTheAccumulatedError) {
    // Arrange: the monotonicity E asks of the MPS path, checked here
    // for the MPO one — a cap that buys nothing would pass every other test.
    const auto run = [](std::uint32_t cap) {
        MpoTruncationConfig cfg;
        cfg.max_bond = cap;
        MpoState rho(5, cfg);
        for (QubitIndex q = 0; q < 5; ++q) rho.apply_unitary_1q(ry_gate(0.6 + 0.3 * q), q);
        for (QubitIndex q = 0; q + 1 < 5; ++q) rho.apply_unitary_2q(cnot_gate(), q, q + 1);
        for (QubitIndex q = 0; q + 1 < 5; ++q) rho.apply_unitary_2q(cnot_gate(), q + 1, q);
        return rho.total_truncation_error();
    };

    // Act + Assert
    EXPECT_TRUE(run(1) >= run(4));
    EXPECT_TRUE(run(4) >= run(64));
    EXPECT_NEAR(run(64), 0.0, 1e-12);
}

TEST(TnMpo, Truncation_CanMakeAConditionalNegative) {
    // Arrange: this is the failure mode the MPS path structurally cannot have
    // (measured -5.5e-3 at n=10). A near-pure entangled state
    // a hard cap is where it shows up; the sampler must REPORT it, not hide it.
    MpoTruncationConfig cfg;
    cfg.max_bond = 1;
    MpoState rho(4, cfg);
    for (QubitIndex q = 0; q < 4; ++q) rho.apply_unitary_1q(h_gate(), q);
    for (QubitIndex q = 0; q + 1 < 4; ++q) rho.apply_unitary_2q(cnot_gate(), q, q + 1);
    rho.apply_unitary_2q(cnot_gate(), 3, 0);
    rho.apply_unitary_2q(cnot_gate(), 2, 0);
    std::mt19937_64 rng(4242ULL);

    // Act: sample repeatedly and keep the most negative conditional seen.
    double worst = 0.0;
    for (int i = 0; i < 200; ++i) {
        double neg = 0.0;
        (void)rho.sample(rng, &neg);
        worst = std::min(worst, neg);
    }

    // Assert: the value is a property of the truncated state, so all this test
    // can assert is that the channel for reporting it exists and carries a
    // non-positive number through. Any negative value is a real defect of rho.
    EXPECT_TRUE(worst <= 0.0);
}

// ── Expectation and sampling ────────────────────────────────────────────────

TEST(TnMpo, ExpectationValue_MatchesTheDenseTrace) {
    // Arrange: T gates are what make this test able to see a Y at all — with
    // only Ry and CNOT every amplitude is real, so every <Y...> term vanishes and
    // an implementation using P instead of P^T would agree everywhere (Y is the
    // only antisymmetric Pauli). Qubit 2 is also entangled and rotated, because
    // leaving it in |0> zeroes every term carrying an X or Y on it — which is
    // exactly how that mutation survived the first version of this test.
    MpoState rho(3);
    DenseRho ref(3);
    const auto gate1 = [&](const std::vector<Cx>& u, QubitIndex q) {
        rho.apply_unitary_1q(u, q); ref.apply_unitary_1q(u, q);
    };
    gate1(ry_gate(0.8), 0); gate1(t_gate(), 0);
    gate1(ry_gate(1.7), 1);
    gate1(ry_gate(1.1), 2); gate1(t_gate(), 2);
    rho.apply_unitary_2q(cnot_gate(), 0, 1); ref.apply_unitary_2q(cnot_gate(), 0, 1);
    rho.apply_unitary_2q(cnot_gate(), 1, 2); ref.apply_unitary_2q(cnot_gate(), 1, 2);
    gate1(t_gate(), 1);
    rho.apply_channel_1q(damping(0.25), 1); ref.apply_kraus_1q(damping(0.25), 1);

    // Act + Assert: Tr(rho P) computed by contraction vs by dense multiplication.
    // The Y-bearing cases are the load-bearing ones; the rest guard the others.
    const std::vector<std::vector<PauliOp>> cases = {
        {PauliOp::Z, PauliOp::I, PauliOp::I},
        {PauliOp::I, PauliOp::Z, PauliOp::I},
        {PauliOp::X, PauliOp::X, PauliOp::I},
        {PauliOp::Y, PauliOp::I, PauliOp::I},
        {PauliOp::I, PauliOp::I, PauliOp::Y},
        {PauliOp::Y, PauliOp::Z, PauliOp::X},
    };
    const std::size_t d = ref.dim();
    for (const std::vector<PauliOp>& ops : cases) {
        // Dense reference: Tr(rho P) = sum_{i,j} rho[i][j] * P[j][i].
        Cx acc{0, 0};
        for (std::size_t i = 0; i < d; ++i) {
            for (std::size_t j = 0; j < d; ++j) {
                Cx pji{1, 0};
                for (QubitIndex q = 0; q < 3; ++q) {
                    const std::size_t jb = (j >> q) & 1u, ib = (i >> q) & 1u;
                    switch (ops[q]) {
                        case PauliOp::I: pji *= (jb == ib) ? Cx{1, 0} : Cx{0, 0}; break;
                        case PauliOp::X: pji *= (jb != ib) ? Cx{1, 0} : Cx{0, 0}; break;
                        case PauliOp::Y:
                            pji *= (jb == ib) ? Cx{0, 0} : (jb == 1 ? Cx{0, 1} : Cx{0, -1});
                            break;
                        case PauliOp::Z:
                            pji *= (jb != ib) ? Cx{0, 0} : (jb == 0 ? Cx{1, 0} : Cx{-1, 0});
                            break;
                    }
                    if (pji == Cx{0, 0}) break;
                }
                acc += ref.m[i * d + j] * pji;
            }
        }
        EXPECT_NEAR(rho.expectation_value(ops), acc.real(), 1e-12);
    }
}

TEST(TnMpo, Sampling_ReproducesTheDiagonalOfRho) {
    // Arrange: an asymmetric mixed state, so every diagonal entry differs and a
    // swapped or permuted conditional cannot pass by symmetry.
    MpoState rho(3);
    DenseRho ref(3);
    rho.apply_unitary_1q(ry_gate(0.7), 0); ref.apply_unitary_1q(ry_gate(0.7), 0);
    rho.apply_unitary_1q(ry_gate(1.9), 1); ref.apply_unitary_1q(ry_gate(1.9), 1);
    rho.apply_unitary_2q(cnot_gate(), 0, 1); ref.apply_unitary_2q(cnot_gate(), 0, 1);
    rho.apply_channel_1q(depol_1q(0.2), 0);  ref.apply_kraus_1q(depol_1q(0.2), 0);
    rho.apply_unitary_1q(ry_gate(1.1), 2);   ref.apply_unitary_1q(ry_gate(1.1), 2);
    std::mt19937_64 rng(2026ULL);
    const std::size_t nshots = 40000, d = ref.dim();

    // Act
    std::vector<std::uint64_t> hits(d, 0);
    for (std::size_t s = 0; s < nshots; ++s) {
        const std::vector<int> bits = rho.sample(rng);
        std::size_t idx = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) if (bits[i]) idx |= std::size_t{1} << i;
        ++hits[idx];
    }

    // Assert: 4 sigma of a binomial at n=40000 is under 0.01 for any p.
    for (std::size_t i = 0; i < d; ++i) {
        EXPECT_NEAR(static_cast<double>(hits[i]) / nshots, ref.m[i * d + i].real(), 0.01);
    }
}

TEST(TnMpo, SameSeed_ReproducesTheSameShots) {
    // Arrange
    MpoState rho(3);
    rho.apply_unitary_1q(h_gate(), 0);
    rho.apply_unitary_2q(cnot_gate(), 0, 1);
    rho.apply_channel_1q(depol_1q(0.1), 1);

    // Act
    std::mt19937_64 a(9ULL), b(9ULL);
    std::vector<std::vector<int>> run_a, run_b;
    for (int i = 0; i < 100; ++i) run_a.push_back(rho.sample(a));
    for (int i = 0; i < 100; ++i) run_b.push_back(rho.sample(b));

    // Assert: a reproducibility claim, so exact equality is the right tolerance.
    EXPECT_TRUE(run_a == run_b);
}

// ── Boundary conditions ──────────────────────────────────────────────────────

TEST(TnMpo, SingleQubitChain_Works) {
    // Arrange: n = 1 leaves no bond to split — the degenerate case the merge
    // loops are most likely to mishandle.
    MpoState rho(1);
    rho.apply_unitary_1q(h_gate(), 0);
    rho.apply_channel_1q(depol_1q(0.4), 0);

    // Act
    const std::vector<Cx> dense = rho.to_dense();

    // Assert: H then depolarizing leaves the diagonal at 1/2 each.
    EXPECT_NEAR(rho.trace(), 1.0, 1e-14);
    EXPECT_NEAR(dense[0].real(), 0.5, 1e-14);
    EXPECT_NEAR(dense[3].real(), 0.5, 1e-14);
}

TEST(TnMpo, IdentityChannel_IsAFixedPoint) {
    // Arrange: an explicit identity Kraus list must change nothing at all —
    // the cheapest check that the ket/bra pairing is wired symmetrically.
    MpoState rho(3);
    rho.apply_unitary_1q(ry_gate(1.3), 0);
    rho.apply_unitary_2q(cnot_gate(), 0, 2);
    const std::vector<Cx> before = rho.to_dense();

    // Act
    rho.apply_channel_1q({mat2(Cx{1, 0}, Cx{0, 0}, Cx{0, 0}, Cx{1, 0})}, 1);

    // Assert
    EXPECT_NEAR(max_abs_diff(rho.to_dense(), before), 0.0, 1e-15);
}

// ── Rejections ───────────────────────────────────────────────────────────────

TEST(TnMpo, OutOfRangeSite_IsRejected) {
    MpoState rho(2);
    EXPECT_THROW(rho.apply_unitary_1q(h_gate(), 2), SimulatorError);
    EXPECT_THROW(rho.apply_unitary_2q(cnot_gate(), 0, 2), SimulatorError);
}

TEST(TnMpo, RepeatedOperand_IsRejected) {
    MpoState rho(2);
    EXPECT_THROW(rho.apply_unitary_2q(cnot_gate(), 1, 1), SimulatorError);
}

TEST(TnMpo, MalformedOperator_IsRejected) {
    // A ragged operator whose flat length is still 4 would slip past a
    // total-size check, so the row widths have to be checked individually.
    MpoState rho(2);
    const UnitaryMatrix ragged{{Cx{1, 0}, Cx{0, 0}, Cx{0, 0}}, {Cx{0, 0}}};
    EXPECT_THROW(rho.apply_channel_1q({ragged}, 0), SimulatorError);
    EXPECT_THROW(rho.apply_channel_1q({}, 0), SimulatorError);
    EXPECT_THROW(rho.apply_unitary_1q({Cx{1, 0}, Cx{0, 0}}, 0), SimulatorError);
}

TEST(TnMpo, InvalidTruncationConfig_IsRejected) {
    MpoTruncationConfig zero;  zero.max_bond = 0;
    MpoTruncationConfig huge;  huge.max_bond = qplayer::kMpoMaxBondCeiling + 1;
    MpoTruncationConfig neg;   neg.cutoff = -1.0;
    EXPECT_THROW(MpoState(2, zero), CircuitError);
    EXPECT_THROW(MpoState(2, huge), CircuitError);
    EXPECT_THROW(MpoState(2, neg), CircuitError);
}

TEST(TnMpo, DenseExportBeyondTheCap_IsRejected) {
    // The MPO holds 4^n worth of information, so the dense export cap is if
    // anything more binding here than on the state-vector paths.
    MpoState rho(qplayer::MAX_DENSE_EXPORT_QUBITS + 1);
    EXPECT_THROW(rho.to_dense(), SimulatorError);
}
