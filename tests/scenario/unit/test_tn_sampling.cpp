// test_tn_sampling.cpp — Unit tests for native MPS perfect sampling
// 1.1, 1.3 (Arrange/Act/Assert).
//
// The sampler's whole claim is that it draws from |psi|^2 exactly, without ever
// building the 2^n vector. Two failure modes are invisible to a "does it run"
// test and are what most of the cases below defend: a swapped conditional
// (p0 <-> p1) and a dropped renormalisation, which both still produce
// well-formed bitstrings. Symmetric states cannot catch either, so the fixtures
// here deliberately give every site a different bias.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <map>
#include <set>
#include <random>
#include <vector>

#include "../../../include/backend/tensor_network/mps.hpp"
#include "../../../include/backend/tensor_network/tn_sampling.hpp"
#include "../../../include/error.hpp"

using qplayer::QubitIndex;
using qplayer::SimulatorError;
using qplayer::detail::MpsSampler;
using qplayer::detail::MpsState;
using qplayer::detail::TensorGraph;
using qplayer::detail::TensorIndex;
using qplayer::detail::TensorNode;
using qplayer::detail::mps_sampling_environment_bytes;

using Cx = std::complex<double>;

namespace {

std::vector<Cx> h_gate() {
    const double s = 1.0 / std::sqrt(2.0);
    return {Cx{s, 0.0}, Cx{s, 0.0}, Cx{s, 0.0}, Cx{-s, 0.0}};
}

std::vector<Cx> x_gate() {
    return {Cx{0.0, 0.0}, Cx{1.0, 0.0}, Cx{1.0, 0.0}, Cx{0.0, 0.0}};
}

// Ry(theta): p(|1>) = sin^2(theta/2), so each site can be given its own bias.
std::vector<Cx> ry_gate(double theta) {
    const double c = std::cos(theta / 2.0);
    const double s = std::sin(theta / 2.0);
    return {Cx{c, 0.0}, Cx{-s, 0.0}, Cx{s, 0.0}, Cx{c, 0.0}};
}

std::vector<Cx> t_gate() {
    const double s = 1.0 / std::sqrt(2.0);
    return {Cx{1.0, 0.0}, Cx{0.0, 0.0}, Cx{0.0, 0.0}, Cx{s, s}};
}

// Operand order: local bit 0 is the first operand, so this flips the SECOND
// operand when the first is |1> — control q0, target q1.
std::vector<Cx> cnot_gate() {
    std::vector<Cx> u(16, Cx{0.0, 0.0});
    u[0 * 4 + 0] = Cx{1.0, 0.0};
    u[3 * 4 + 1] = Cx{1.0, 0.0};
    u[2 * 4 + 2] = Cx{1.0, 0.0};
    u[1 * 4 + 3] = Cx{1.0, 0.0};
    return u;
}

// Site-ordered bits -> basis index, bit i = site i. Matches MpsState::to_dense(),
// whose index bit q is site q.
std::size_t basis_of(const std::vector<int>& bits) {
    std::size_t idx = 0;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i] != 0) idx |= std::size_t{1} << i;
    }
    return idx;
}

// An entangled chain whose amplitudes are all distinct — no two basis states
// share a probability, so a permuted or swapped draw cannot pass by symmetry.
MpsState asymmetric_chain() {
    MpsState s(4);
    s.apply_1q(ry_gate(0.7), 0);
    s.apply_1q(ry_gate(1.9), 1);
    s.apply_1q(t_gate(), 1);
    s.apply_1q(ry_gate(2.5), 2);
    s.apply_1q(ry_gate(1.1), 3);
    s.apply_2q(cnot_gate(), 0, 1);
    s.apply_2q(cnot_gate(), 2, 3);
    s.apply_2q(cnot_gate(), 1, 2);
    return s;
}

}  // namespace

// ── Distribution correctness against the dense state (completion condition 1) ─

TEST(TnSampling, Draws_MatchDenseProbabilities) {
    // Arrange: entangled, asymmetric, and small enough to densify for reference.
    const MpsState state = asymmetric_chain();
    const std::vector<Cx> dense = state.to_dense();
    const MpsSampler sampler(state.graph());
    std::mt19937_64 rng(20260826ULL);
    const std::size_t nshots = 40000;

    // Act
    std::vector<std::uint64_t> hits(dense.size(), 0);
    std::vector<int> bits;
    for (std::size_t s = 0; s < nshots; ++s) {
        sampler.sample_into(rng, bits);
        ++hits[basis_of(bits)];
    }

    // Assert: 4 sigma of a binomial at n=40000 is under 0.01 for any p, so this
    // separates a correct sampler from a swapped conditional (which moves mass
    // by O(0.1)) without being flaky.
    for (std::size_t i = 0; i < dense.size(); ++i) {
        const double observed = static_cast<double>(hits[i]) / static_cast<double>(nshots);
        EXPECT_NEAR(observed, std::norm(dense[i]), 0.01);
    }
}

TEST(TnSampling, PerSiteMarginals_TrackDistinctBiases) {
    // Arrange: a product state where every site has its own p(1). A site
    // permutation or a p0/p1 swap changes at least one marginal, and unlike an
    // entangled fixture this pins each site independently.
    const std::vector<double> thetas{0.4, 1.2, 2.0, 2.8, 0.9};
    MpsState state(static_cast<QubitIndex>(thetas.size()));
    for (QubitIndex q = 0; q < thetas.size(); ++q) {
        state.apply_1q(ry_gate(thetas[q]), q);
    }
    const MpsSampler sampler(state.graph());
    std::mt19937_64 rng(777ULL);
    const std::size_t nshots = 40000;

    // Act
    std::vector<std::uint64_t> ones(thetas.size(), 0);
    std::vector<int> bits;
    for (std::size_t s = 0; s < nshots; ++s) {
        sampler.sample_into(rng, bits);
        for (std::size_t i = 0; i < bits.size(); ++i) ones[i] += static_cast<std::uint64_t>(bits[i]);
    }

    // Assert: statistical tolerance at nshots=40000 (4 sigma < 0.01).
    for (std::size_t i = 0; i < thetas.size(); ++i) {
        const double expected = std::sin(thetas[i] / 2.0) * std::sin(thetas[i] / 2.0);
        const double observed = static_cast<double>(ones[i]) / static_cast<double>(nshots);
        EXPECT_NEAR(observed, expected, 0.01);
    }
}

TEST(TnSampling, CorrelatedChain_OnlyProducesSupportedOutcomes) {
    // Arrange: GHZ support is 2 of 2^n states. This is the structural half of
    // correctness — no amount of marginal agreement proves the conditionals
    // carry correlation, and a sampler that drew each site independently from
    // its marginal would pass every marginal test above and fail this one.
    MpsState state(6);
    state.apply_1q(h_gate(), 0);
    for (QubitIndex q = 0; q + 1 < 6; ++q) state.apply_2q(cnot_gate(), q, q + 1);
    const MpsSampler sampler(state.graph());
    std::mt19937_64 rng(31337ULL);

    // Act
    std::map<std::size_t, std::uint64_t> seen;
    std::vector<int> bits;
    for (std::size_t s = 0; s < 4000; ++s) {
        sampler.sample_into(rng, bits);
        ++seen[basis_of(bits)];
    }

    // Assert
    EXPECT_EQ(seen.size(), std::size_t{2});
    EXPECT_TRUE(seen.count(0) == 1);
    EXPECT_TRUE(seen.count(63) == 1);
    EXPECT_NEAR(static_cast<double>(seen[0]) / 4000.0, 0.5, 0.04);  // 4 sigma at n=4000
}

// ── Determinism and reproducibility ───────────────────────────────

TEST(TnSampling, SameSeed_ReproducesTheSameShots) {
    // Arrange
    const MpsState state = asymmetric_chain();
    const MpsSampler sampler(state.graph());

    // Act
    std::mt19937_64 rng_a(99ULL);
    std::mt19937_64 rng_b(99ULL);
    std::vector<std::size_t> run_a, run_b;
    for (std::size_t s = 0; s < 200; ++s) run_a.push_back(basis_of(sampler.sample(rng_a)));
    for (std::size_t s = 0; s < 200; ++s) run_b.push_back(basis_of(sampler.sample(rng_b)));

    // Assert: a reproducibility claim, so the tolerance is exact equality. The
    // distinct-value check keeps the claim from being satisfied vacuously by a
    // sampler that always returns the same shot.
    EXPECT_TRUE(run_a == run_b);
    EXPECT_TRUE(std::set<std::size_t>(run_a.begin(), run_a.end()).size() > 1);
}

TEST(TnSampling, SampleConsumesOneDrawPerSite) {
    // Arrange: the draw budget is part of the seed contract — a sampler that
    // consumed a variable number of uniforms would make reproducibility
    // depend on the state, not just the seed.
    const MpsState state = asymmetric_chain();
    const MpsSampler sampler(state.graph());
    std::mt19937_64 counted(5ULL);
    std::mt19937_64 reference(5ULL);

    // Act
    (void)sampler.sample(counted);
    for (QubitIndex q = 0; q < 4; ++q) {
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        (void)u01(reference);
    }

    // Assert
    EXPECT_EQ(counted(), reference());
}

// ── Boundary conditions ──────────────────────────────────────────────────────

TEST(TnSampling, ProductState_IsDrawnDeterministically) {
    // Arrange: |0101> has one basis state with all the weight, so every shot
    // must agree — a chi=1 chain where any conditional bug shows up immediately.
    MpsState state(4);
    state.apply_1q(x_gate(), 1);
    state.apply_1q(x_gate(), 3);
    const MpsSampler sampler(state.graph());
    std::mt19937_64 rng(1ULL);

    // Act + Assert
    for (std::size_t s = 0; s < 50; ++s) {
        const std::vector<int> bits = sampler.sample(rng);
        EXPECT_EQ(basis_of(bits), std::size_t{0b1010});
    }
}

TEST(TnSampling, SingleQubitChain_IsSampled) {
    // Arrange: n = 1 leaves no interior site, so both environment sweeps are
    // empty — the degenerate case an index loop is most likely to mishandle.
    MpsState state(1);
    state.apply_1q(ry_gate(2.0), 0);
    const MpsSampler sampler(state.graph());
    std::mt19937_64 rng(11ULL);

    // Act
    std::uint64_t ones = 0;
    for (std::size_t s = 0; s < 20000; ++s) ones += static_cast<std::uint64_t>(sampler.sample(rng)[0]);

    // Assert: p(1) = sin^2(1.0); 4 sigma at n=20000 is under 0.015.
    EXPECT_NEAR(static_cast<double>(ones) / 20000.0, std::sin(1.0) * std::sin(1.0), 0.015);
}

TEST(TnSampling, TruncatedChain_StillSamplesFromTheStoredState) {
    // Arrange: a hard bond cap makes the stored state an approximation. The
    // sampler must draw from what is stored, not from what was intended — the
    // truncation error is reported separately and double-counting it
    // here would hide it.
    qplayer::detail::MpsTruncationConfig cfg;
    cfg.max_bond = 2;
    MpsState state(6, cfg);
    state.apply_1q(h_gate(), 0);
    state.apply_1q(ry_gate(1.1), 2);
    state.apply_1q(ry_gate(0.6), 4);
    for (QubitIndex q = 0; q + 1 < 6; ++q) state.apply_2q(cnot_gate(), q, q + 1);
    const std::vector<Cx> dense = state.to_dense();
    const MpsSampler sampler(state.graph());
    std::mt19937_64 rng(4242ULL);

    // Act
    std::vector<std::uint64_t> hits(dense.size(), 0);
    for (std::size_t s = 0; s < 20000; ++s) ++hits[basis_of(sampler.sample(rng))];

    // Assert: 4 sigma at n=20000 is under 0.015.
    for (std::size_t i = 0; i < dense.size(); ++i) {
        EXPECT_NEAR(static_cast<double>(hits[i]) / 20000.0, std::norm(dense[i]), 0.015);
    }
}

// ── Norm read-off (the O(n chi^3) replacement for squared_norm()) ────────────

TEST(TnSampling, SquaredNorm_AgreesWithTheChainSweep) {
    // Arrange
    const MpsState state = asymmetric_chain();

    // Act
    const MpsSampler sampler(state.graph());

    // Assert: same contraction, different order — 1e-12 is numerical agreement,
    // not a statistical bound.
    EXPECT_NEAR(sampler.squared_norm(), state.squared_norm(), 1e-12);
    EXPECT_NEAR(sampler.squared_norm(), 1.0, 1e-12);
}

// ── Rejections ───────────────────────────────────────────────────────────────

TEST(TnSampling, NonChainGraph_IsRejected) {
    // Arrange: a lone 2-index node is a valid TensorGraph but not an MPS chain.
    TensorGraph graph;
    TensorNode node;
    node.indices = {TensorIndex{TensorIndex::Kind::Physical, 2, 0},
                    TensorIndex{TensorIndex::Kind::Bond, 1, 0}};
    node.data = {Cx{1.0, 0.0}, Cx{0.0, 0.0}};
    graph.add_node(node);

    // Act + Assert
    EXPECT_THROW(MpsSampler{graph}, SimulatorError);
}

TEST(TnSampling, EmptyGraph_IsRejected) {
    // Arrange
    TensorGraph graph;

    // Act + Assert
    EXPECT_THROW(MpsSampler{graph}, SimulatorError);
}

TEST(TnSampling, ZeroNormChain_IsRejectedRatherThanSampled) {
    // Arrange: a chain annihilated to zero has no distribution to draw from.
    // Returning an arbitrary bitstring here would be a silent wrong answer, so
    // the sampler must fail loudly.
    MpsState state(3);
    state.apply_1q({Cx{0.0, 0.0}, Cx{0.0, 0.0}, Cx{0.0, 0.0}, Cx{0.0, 0.0}}, 0);

    // Act + Assert
    EXPECT_THROW(MpsSampler{state.graph()}, SimulatorError);
}

// ── Footprint estimate (feeds the trajectory thread budget) ──────────────────

TEST(TnSampling, EnvironmentBytes_MatchTheEstimate) {
    // Arrange: chi = 1 everywhere, so the actual footprint is n+1 scalars and the
    // estimate is its chi=1 evaluation — the one point where the worst-case
    // formula and reality must coincide exactly.
    MpsState state(5);
    const MpsSampler sampler(state.graph());

    // Act
    const std::uint64_t actual   = sampler.environment_bytes();
    const std::uint64_t estimate = mps_sampling_environment_bytes(5, 1);

    // Assert
    EXPECT_EQ(actual, estimate);
    EXPECT_TRUE(mps_sampling_environment_bytes(100, 256) >
                mps_sampling_environment_bytes(100, 128));
}
