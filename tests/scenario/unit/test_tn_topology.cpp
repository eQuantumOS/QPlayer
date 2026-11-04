// test_tn_topology.cpp — Unit tests for CircuitTopologyAnalyzer
//   and the site layout the TensorNetwork backend derives from it.
// Tests follow the Arrange / Act / Assert layout.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <utility>
#include <vector>

#include "../../../include/backend/tensor_network/topology.hpp"
#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"

using qplayer::Circuit;
using qplayer::QubitIndex;
using qplayer::detail::CircuitTopologyAnalyzer;
using qplayer::detail::NonLinearLayout;
using qplayer::detail::TnTopology;
using qplayer::detail::TopologyAnalysis;

namespace {

TopologyAnalysis analyze_pairs(std::vector<std::pair<QubitIndex, QubitIndex>> pairs,
                               QubitIndex n,
                               NonLinearLayout layout = NonLinearLayout::Identity) {
    return CircuitTopologyAnalyzer::analyze(pairs, n, layout);
}

}  // namespace

// ── Graph classification ─────────────────────────────────────────────────────

TEST(TnTopology, LinearChain_InOrder_IsLinearWithIdentityLayout) {
    // Arrange + Act: 0-1-2-3.
    TopologyAnalysis a = analyze_pairs({{0, 1}, {1, 2}, {2, 3}}, 4);

    // Assert
    EXPECT_TRUE(a.topology == TnTopology::Linear);
    EXPECT_TRUE(a.is_identity_order());
    EXPECT_EQ(a.coupling_edges, static_cast<std::size_t>(3));
    EXPECT_EQ(a.max_degree, static_cast<std::size_t>(2));
    EXPECT_EQ(a.components, static_cast<std::size_t>(1));
}

TEST(TnTopology, PermutedChain_ReportsTheChainOrder) {
    // Arrange + Act: the path 0-4-2-3-1 written out of index order.
    TopologyAnalysis a = analyze_pairs({{0, 4}, {4, 2}, {2, 3}, {3, 1}}, 5);

    // Assert: linear, but the useful layout is not the identity.
    EXPECT_TRUE(a.topology == TnTopology::Linear);
    EXPECT_FALSE(a.is_identity_order());
    std::vector<QubitIndex> expected{0, 4, 2, 3, 1};
    std::vector<QubitIndex> reversed{1, 3, 2, 4, 0};
    // Either walk direction is a valid layout — both make every gate adjacent.
    EXPECT_TRUE(a.chain_order == expected || a.chain_order == reversed);
}

TEST(TnTopology, Star_IsNonLinear) {
    // Arrange + Act: qubit 0 talks to 1, 2 and 3 — degree 3.
    TopologyAnalysis a = analyze_pairs({{0, 1}, {0, 2}, {0, 3}}, 4);

    // Assert: no ordering of a line can make three qubits all adjacent to one.
    EXPECT_TRUE(a.topology == TnTopology::NonLinear);
    EXPECT_EQ(a.max_degree, static_cast<std::size_t>(3));
    EXPECT_TRUE(a.is_identity_order());  // no re-layout unless opted
}

TEST(TnTopology, Ring_IsNonLinearDespiteDegreeTwo) {
    // Arrange + Act: 0-1-2-3-0. Every degree is 2, yet it is a cycle.
    TopologyAnalysis a = analyze_pairs({{0, 1}, {1, 2}, {2, 3}, {3, 0}}, 4);

    // Assert: degree alone is not the test — the edge count gives it away.
    EXPECT_TRUE(a.topology == TnTopology::NonLinear);
    EXPECT_EQ(a.max_degree, static_cast<std::size_t>(2));
    EXPECT_EQ(a.coupling_edges, static_cast<std::size_t>(4));
}

TEST(TnTopology, TwoDisjointChains_StayLinear) {
    // Arrange + Act: 0-1 and 2-3 never interact.
    TopologyAnalysis a = analyze_pairs({{0, 1}, {2, 3}}, 4);

    // Assert: independent chains lay out end to end on one MPS.
    EXPECT_TRUE(a.topology == TnTopology::Linear);
    EXPECT_EQ(a.components, static_cast<std::size_t>(2));
    EXPECT_EQ(a.chain_order.size(), static_cast<std::size_t>(4));
}

TEST(TnTopology, IsolatedQubits_DoNotBreakLinearity) {
    // Arrange + Act: only 1-2 interact; 0 and 3 are untouched.
    TopologyAnalysis a = analyze_pairs({{1, 2}}, 4);

    // Assert: an unentangled qubit can sit anywhere, so it is not a branch.
    EXPECT_TRUE(a.topology == TnTopology::Linear);
    EXPECT_EQ(a.components, static_cast<std::size_t>(3));
}

TEST(TnTopology, NoTwoQubitGates_IsLinear) {
    // Arrange + Act
    TopologyAnalysis a = analyze_pairs({}, 3);

    // Assert: a product-state circuit needs no bond at all.
    EXPECT_TRUE(a.topology == TnTopology::Linear);
    EXPECT_EQ(a.coupling_edges, static_cast<std::size_t>(0));
    EXPECT_TRUE(a.is_identity_order());
}

TEST(TnTopology, DuplicateEdges_CountOnce) {
    // Arrange + Act: the same pair gated repeatedly is still one coupling.
    TopologyAnalysis a = analyze_pairs({{0, 1}, {1, 0}, {0, 1}}, 2);

    // Assert
    EXPECT_EQ(a.coupling_edges, static_cast<std::size_t>(1));
    EXPECT_TRUE(a.topology == TnTopology::Linear);
}

TEST(TnTopology, ZeroQubits_IsHandled) {
    // Arrange + Act: degenerate input must not read past the end.
    TopologyAnalysis a = analyze_pairs({}, 0);

    // Assert
    EXPECT_TRUE(a.chain_order.empty());
    EXPECT_EQ(a.components, static_cast<std::size_t>(0));
}

// ── Circuit extraction ───────────────────────────────────────────────────────

TEST(TnTopology, FromCircuit_IgnoresSingleQubitGates) {
    // Arrange
    Circuit circ(3, 3);
    circ.h(0).t(1).x(2).cx(0, 1).cx(1, 2);

    // Act
    TopologyAnalysis a = CircuitTopologyAnalyzer::analyze(circ);

    // Assert: only 2Q gates create couplings.
    EXPECT_TRUE(a.topology == TnTopology::Linear);
    EXPECT_EQ(a.coupling_edges, static_cast<std::size_t>(2));
}

TEST(TnTopology, FromCircuit_ThreeQubitGate_IsNonLinear) {
    // Arrange
    Circuit circ(3, 3);
    circ.ccx(0, 1, 2);

    // Act
    TopologyAnalysis a = CircuitTopologyAnalyzer::analyze(circ);

    // Assert: all three operands must be mutually close, which no chain gives.
    EXPECT_TRUE(a.topology == TnTopology::NonLinear);
    EXPECT_EQ(a.coupling_edges, static_cast<std::size_t>(3));
}

// ── Backend layout (the payoff) ──────────────────────────────────────────────

TEST(TnTopology, Backend_PermutedChain_NeedsNoSwaps) {
    // Arrange: interactions form the path 0-4-2-3-1, written out of index order.
    qplayer::TensorNetworkBackend backend;
    Circuit circ(5, 5);
    circ.h(0).cx(0, 4).cx(4, 2).cx(2, 3).cx(3, 1);

    // Act
    backend.prepare(circ);
    backend.execute(circ, 1, nullptr);

    // Assert: laid out along the path, every gate is already adjacent. Under the
    // identity layout the same circuit would walk operands with SWAPs.
    EXPECT_TRUE(backend.topology().topology == TnTopology::Linear);
    EXPECT_FALSE(backend.topology().is_identity_order());
    EXPECT_EQ(backend.swap_steps(), static_cast<std::size_t>(0));
}

TEST(TnTopology, Backend_PermutedChain_StillMatchesFullState) {
    // Arrange: the same path, but every qubit gets a DIFFERENT single-qubit gate.
    // A GHZ-like circuit is invariant under qubit relabelling, so it cannot tell
    // a missing export permutation from a correct one — this one can.
    qplayer::TensorNetworkBackend backend;
    Circuit circ(5, 5);
    circ.h(0).t(0).cx(0, 4).s(4).cx(4, 2).t(2).h(2).cx(2, 3).h(3).cx(3, 1).tdg(1);

    // Act
    backend.prepare(circ);
    qplayer::Result r = backend.execute(circ, 1, nullptr);
    qplayer::StateVector tn_sv = r.statevector();

    qplayer::FullStateCpuBackend fs;
    fs.init_state(5);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }
    qplayer::StateVector fs_sv = fs.get_statevector();

    // Assert
    double max_diff = 0.0;
    for (std::size_t i = 0; i < tn_sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(tn_sv[i] - fs_sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-10);
}

TEST(TnTopology, Backend_StarCircuit_RunsExactlyOnMps) {
    // Arrange: a star is NonLinear, and the general TN path does not exist yet.
    // The contract is that MPS still returns the right answer (it only pays in
    // bond dimension) rather than refusing the circuit.
    qplayer::TensorNetworkBackend backend;
    Circuit circ(4, 4);
    circ.h(0).cx(0, 1).cx(0, 2).cx(0, 3);

    // Act
    backend.prepare(circ);
    qplayer::Result r = backend.execute(circ, 1, nullptr);
    qplayer::StateVector tn_sv = r.statevector();

    qplayer::FullStateCpuBackend fs;
    fs.init_state(4);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }
    qplayer::StateVector fs_sv = fs.get_statevector();

    // Assert
    EXPECT_TRUE(backend.topology().topology == TnTopology::NonLinear);
    double max_diff = 0.0;
    for (std::size_t i = 0; i < tn_sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(tn_sv[i] - fs_sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-10);
}

TEST(TnTopology, Backend_PermutedChain_SamplesCorrectly) {
    // Arrange: an ASYMMETRIC outcome on the path 0-3-1-2 — qubit 0 is certain to
    // read 1, qubit 2 certain to read 0, and the middle pair is the correlated
    // half. A symmetric GHZ would sample identically under any relabelling and
    // so could not detect a missing export permutation.
    qplayer::TensorNetworkBackend backend;
    backend.set_seed(777);
    Circuit circ(4, 4);
    circ.x(0).cx(0, 3).h(3).cx(3, 1);
    circ.measure(0, 0).measure(1, 1).measure(2, 2).measure(3, 3);

    // Act
    backend.prepare(circ);
    auto counts = backend.execute(circ, 2000, nullptr).counts();

    // Assert: bit order in a key is c3 c2 c1 c0, so qubit 0 = 1 and qubit 2 = 0
    // pin two positions; qubits 1 and 3 flip together.
    EXPECT_EQ(counts.size(), static_cast<std::size_t>(2));
    EXPECT_NEAR(static_cast<double>(counts["0001"]) / 2000.0, 0.5, 0.04);  // nshots=2000 band
    EXPECT_NEAR(static_cast<double>(counts["1011"]) / 2000.0, 0.5, 0.04);
}

// ── NonLinear layout (MinLA, opt-in since) ─────────────────

namespace {

// A 3x4 grid whose qubit labels are scrambled, so the identity order scatters
// neighbours along the chain and a good layout has something to recover.
constexpr QubitIndex kGridLabel[12] = {7, 2, 10, 0, 5, 11, 3, 8, 1, 9, 4, 6};

std::vector<std::pair<QubitIndex, QubitIndex>> scrambled_grid_pairs() {
    std::vector<std::pair<QubitIndex, QubitIndex>> pairs;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            const QubitIndex q = kGridLabel[y * 4 + x];
            if (x + 1 < 4) pairs.emplace_back(q, kGridLabel[y * 4 + x + 1]);
            if (y + 1 < 3) pairs.emplace_back(q, kGridLabel[(y + 1) * 4 + x]);
        }
    }
    return pairs;
}

bool is_permutation_of_sites(const std::vector<QubitIndex>& order) {
    std::vector<QubitIndex> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i] != static_cast<QubitIndex>(i)) return false;
    return true;
}

}  // namespace

TEST(TnTopology, NonLinearScrambledGrid_LayoutWalksLessThanIdentity) {
    // Act
    TopologyAnalysis a = analyze_pairs(scrambled_grid_pairs(), 12, NonLinearLayout::MinLA);

    // Assert
    EXPECT_TRUE(a.topology == TnTopology::NonLinear);
    EXPECT_TRUE(is_permutation_of_sites(a.chain_order));
    EXPECT_TRUE(a.layout_cost < a.identity_layout_cost);
}

// On a complete graph every order costs the same. Accepting ties would shuffle
// the sites for nothing and still reorder the native sampler's draws.
TEST(TnTopology, NonLinearCompleteGraph_KeepsIdentity) {
    // Arrange
    std::vector<std::pair<QubitIndex, QubitIndex>> pairs;
    for (QubitIndex a = 0; a < 6; ++a)
        for (QubitIndex b = a + 1; b < 6; ++b) pairs.emplace_back(a, b);

    // Act
    TopologyAnalysis a = analyze_pairs(pairs, 6, NonLinearLayout::MinLA);

    // Assert
    EXPECT_TRUE(a.topology == TnTopology::NonLinear);
    EXPECT_TRUE(a.is_identity_order());
    EXPECT_EQ(a.layout_cost, a.identity_layout_cost);
}

// The layout feeds the seed-reproducibility contract: the same
// circuit must land on the same sites every time.
TEST(TnTopology, NonLinearLayout_IsDeterministic) {
    // Act
    TopologyAnalysis first  = analyze_pairs(scrambled_grid_pairs(), 12, NonLinearLayout::MinLA);
    TopologyAnalysis second = analyze_pairs(scrambled_grid_pairs(), 12, NonLinearLayout::MinLA);

    // Assert
    EXPECT_TRUE(first.chain_order == second.chain_order);
}

// Every repeat of a pair walks again, so a heavily used pair must end adjacent
// even when a lightly used layout would put something else there.
TEST(TnTopology, NonLinearWeights_HeavyPairEndsAdjacent) {
    // Arrange: star 0-{1,2,3,4}, with the (0, 4) interaction repeated ten times.
    std::vector<std::pair<QubitIndex, QubitIndex>> pairs{{0, 1}, {0, 2}, {0, 3}};
    for (int i = 0; i < 10; ++i) pairs.emplace_back(0, 4);

    // Act
    TopologyAnalysis a = analyze_pairs(pairs, 5, NonLinearLayout::MinLA);

    // Assert
    std::vector<QubitIndex> pos(5);
    for (std::size_t s = 0; s < a.chain_order.size(); ++s) pos[a.chain_order[s]] = static_cast<QubitIndex>(s);
    const QubitIndex gap = pos[0] > pos[4] ? pos[0] - pos[4] : pos[4] - pos[0];
    EXPECT_EQ(gap, QubitIndex{1});
}

// The motivating circuit: rotated surface code d=3 with its ancillas numbered
// after the data (gen_surface.py), which parks them at the far end of the chain.
TEST(TnTopology, SurfaceCodeD3_LayoutCutsWalkCost) {
    // Arrange: one round of X and Z stabiliser extraction.
    const int d = 3, nd = d * d;
    std::vector<std::vector<int>> xs, zs;
    for (int r = 0; r < d - 1; ++r)
        for (int c = 0; c < d - 1; ++c) {
            std::vector<int> p{r * d + c, r * d + c + 1, (r + 1) * d + c, (r + 1) * d + c + 1};
            ((r + c) % 2 == 0 ? zs : xs).push_back(p);
        }
    xs.push_back({0, d});
    xs.push_back({d + d - 1, 2 * d + d - 1});
    zs.push_back({1, 2});
    zs.push_back({(d - 1) * d, (d - 1) * d + 1});
    Circuit circ(static_cast<QubitIndex>(2 * nd - 1), 1);
    for (std::size_t j = 0; j < xs.size(); ++j)
        for (int q : xs[j]) circ.cz(static_cast<QubitIndex>(nd + j), static_cast<QubitIndex>(q));
    for (std::size_t j = 0; j < zs.size(); ++j)
        for (int q : zs[j]) circ.cz(static_cast<QubitIndex>(q), static_cast<QubitIndex>(nd + xs.size() + j));

    // Act
    TopologyAnalysis a = CircuitTopologyAnalyzer::analyze(circ, NonLinearLayout::MinLA);

    // Assert: measured 196 -> 58. A factor of 3 leaves room for tie-break
    // changes while still failing if the layout stops being applied.
    EXPECT_TRUE(a.topology == TnTopology::NonLinear);
    EXPECT_TRUE(a.layout_cost * 3 < a.identity_layout_cost);
}

// The layout is only a relabelling: export must undo it exactly. Distinct 1Q
// gates per qubit make any missing permutation visible in the amplitudes.
TEST(TnTopology, Backend_ScrambledGrid_StillMatchesFullState) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    backend.set_nonlinear_layout(NonLinearLayout::MinLA);
    Circuit circ(12, 12);
    for (QubitIndex q = 0; q < 12; ++q) circ.ry(0.1 + 0.13 * q, q).rz(0.2 + 0.07 * q, q);
    for (const auto& [a, b] : scrambled_grid_pairs()) circ.cz(a, b);
    for (QubitIndex q = 0; q < 12; ++q) circ.rx(0.3 + 0.05 * q, q);

    // Act
    backend.prepare(circ);
    qplayer::Result r = backend.execute(circ, 1, nullptr);
    qplayer::StateVector tn_sv = r.statevector();
    qplayer::FullStateCpuBackend fs;
    fs.init_state(12);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }
    qplayer::StateVector fs_sv = fs.get_statevector();

    // Assert
    EXPECT_FALSE(backend.topology().is_identity_order());
    double max_diff = 0.0;
    for (std::size_t i = 0; i < tn_sv.size(); ++i) max_diff = std::max(max_diff, std::abs(tn_sv[i] - fs_sv[i]));
    EXPECT_NEAR(max_diff, 0.0, 1e-10);  // E cross-backend tolerance
}

// Same relabelling on the sampling side: a basis state through phase-only CZs
// reads out deterministically, so one misplaced bit shows as a wrong key.
TEST(TnTopology, Backend_ScrambledGrid_SamplesTheRightBits) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    backend.set_nonlinear_layout(NonLinearLayout::MinLA);
    backend.set_seed(4242);
    Circuit circ(12, 12);
    for (QubitIndex q : {0u, 3u, 4u, 9u}) circ.x(q);
    for (const auto& [a, b] : scrambled_grid_pairs()) circ.cz(a, b);
    circ.measure_all();

    // Act
    backend.prepare(circ);
    auto counts = backend.execute(circ, 64, nullptr).counts();

    // Assert: key is c11 ... c0, so bits 0, 3, 4, 9 are set.
    EXPECT_FALSE(backend.topology().is_identity_order());
    EXPECT_EQ(counts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(counts["001000011001"], static_cast<std::uint64_t>(64));
}

// the default is the identity. Re-laying out flipped a d=5 surface
// code from 2 s to more than 900 s under the paper-1 willow noise, so nothing
// moves unless QPLAYER_TN_LAYOUT=minla (or the test setter) asks for it.
TEST(TnTopology, NonLinearDefault_KeepsIdentityEvenWhenMinlaWouldWalkLess) {
    // Act
    TopologyAnalysis dflt  = analyze_pairs(scrambled_grid_pairs(), 12);
    TopologyAnalysis minla = analyze_pairs(scrambled_grid_pairs(), 12, NonLinearLayout::MinLA);

    // Assert
    EXPECT_TRUE(dflt.is_identity_order());
    EXPECT_EQ(dflt.layout_cost, dflt.identity_layout_cost);
    EXPECT_TRUE(minla.layout_cost < dflt.identity_layout_cost);  // so the default is a choice
}

TEST(TnTopology, Backend_NonLinearDefault_UsesIdentity) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    Circuit circ(12, 0);
    for (const auto& [a, b] : scrambled_grid_pairs()) circ.cz(a, b);

    // Act
    backend.prepare(circ);

    // Assert -- holds unless the test process itself exports QPLAYER_TN_LAYOUT.
    EXPECT_TRUE(backend.topology().topology == TnTopology::NonLinear);
    EXPECT_TRUE(backend.topology().is_identity_order());
}
