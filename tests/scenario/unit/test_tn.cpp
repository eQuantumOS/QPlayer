// test_tn.cpp — Unit tests for the TensorNetwork graph types:
//   TensorIndex / TensorNode / Bond / TensorGraph construction, wiring
// validation, and MPS |0...0> initialisation.
// Gate application and SVD truncation stay unimplemented.
// Tests follow the Arrange / Act / Assert layout.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/types.hpp"

using qplayer::CircuitError;
using qplayer::SimulatorError;
using qplayer::QubitIndex;
using qplayer::detail::Bond;
using qplayer::detail::TensorGraph;
using qplayer::detail::TensorIndex;
using qplayer::detail::TensorNode;

namespace {

// A [bond(dim l), physical(2), bond(dim r)] MPS-shaped site holding |0>.
TensorNode make_site(std::uint32_t left_dim, std::uint32_t right_dim,
                     std::uint32_t qubit, std::uint32_t left_id, std::uint32_t right_id) {
    TensorNode site;
    site.indices = {
        TensorIndex{TensorIndex::Kind::Bond,     left_dim,  left_id},
        TensorIndex{TensorIndex::Kind::Physical, 2,         qubit},
        TensorIndex{TensorIndex::Kind::Bond,     right_dim, right_id},
    };
    site.data.assign(static_cast<std::size_t>(left_dim) * 2u * right_dim, {0.0, 0.0});
    site.data[0] = {1.0, 0.0};
    return site;
}

}  // namespace

// ── TensorNode ───────────────────────────────────────────────────────────────

TEST(TensorGraph, ElementCount_ThreeIndexSite_MultipliesDims) {
    // Arrange
    TensorNode site = make_site(3, 5, /*qubit=*/0, /*left_id=*/0, /*right_id=*/1);

    // Act + Assert: 3 * 2 * 5 = 30 amplitudes.
    EXPECT_EQ(site.element_count(), static_cast<std::size_t>(30));
}

TEST(TensorGraph, ElementCount_NoIndices_IsScalarOne) {
    // Arrange
    TensorNode scalar;

    // Act + Assert: the empty product is 1, not 0 — a scalar tensor holds one value.
    EXPECT_EQ(scalar.element_count(), static_cast<std::size_t>(1));
}

// ── add_node / connect ───────────────────────────────────────────────────────

TEST(TensorGraph, AddNode_ReturnsSequentialIds) {
    // Arrange
    TensorGraph g;

    // Act
    std::uint32_t a = g.add_node(make_site(1, 1, 0, 0, 1));
    std::uint32_t b = g.add_node(make_site(1, 1, 1, 1, 2));

    // Assert
    EXPECT_EQ(a, static_cast<std::uint32_t>(0));
    EXPECT_EQ(b, static_cast<std::uint32_t>(1));
    EXPECT_EQ(g.node_count(), static_cast<std::size_t>(2));
}

TEST(TensorGraph, AddNode_DataLengthMismatch_Throws) {
    // Arrange: declared dims need 1*2*1 = 2 entries, but only 1 is supplied.
    TensorNode broken = make_site(1, 1, 0, 0, 1);
    broken.data.resize(1);
    TensorGraph g;

    // Act + Assert
    EXPECT_THROW(g.add_node(broken), SimulatorError);
}

TEST(TensorGraph, Connect_MatchingBondDims_CreatesBond) {
    // Arrange
    TensorGraph g;
    g.add_node(make_site(1, 4, 0, 0, 1));
    g.add_node(make_site(4, 1, 1, 1, 2));

    // Act
    g.connect(0, 2, 1, 0);

    // Assert
    EXPECT_EQ(g.bond_count(), static_cast<std::size_t>(1));
    EXPECT_EQ(g.bonds()[0].node_a, static_cast<std::uint32_t>(0));
    EXPECT_EQ(g.bonds()[0].index_b, static_cast<std::uint32_t>(0));
}

TEST(TensorGraph, Connect_DimMismatch_Throws) {
    // Arrange: right bond of site 0 is 4, left bond of site 1 is 2.
    TensorGraph g;
    g.add_node(make_site(1, 4, 0, 0, 1));
    g.add_node(make_site(2, 1, 1, 1, 2));

    // Act + Assert: a silent mismatch would corrupt the contraction later.
    EXPECT_THROW(g.connect(0, 2, 1, 0), SimulatorError);
}

TEST(TensorGraph, Connect_PhysicalLeg_Throws) {
    // Arrange
    TensorGraph g;
    g.add_node(make_site(1, 1, 0, 0, 1));
    g.add_node(make_site(1, 1, 1, 1, 2));

    // Act + Assert: index 1 is the physical leg and must never be contracted away.
    EXPECT_THROW(g.connect(0, 1, 1, 1), SimulatorError);
}

TEST(TensorGraph, Connect_LegAlreadyBonded_Throws) {
    // Arrange
    TensorGraph g;
    g.add_node(make_site(1, 1, 0, 0, 1));
    g.add_node(make_site(1, 1, 1, 1, 2));
    g.add_node(make_site(1, 1, 2, 2, 3));
    g.connect(0, 2, 1, 0);

    // Act + Assert: one leg belongs to at most one bond.
    EXPECT_THROW(g.connect(0, 2, 2, 0), SimulatorError);
}

TEST(TensorGraph, Connect_NodeOutOfRange_Throws) {
    // Arrange
    TensorGraph g;
    g.add_node(make_site(1, 1, 0, 0, 1));

    // Act + Assert
    EXPECT_THROW(g.connect(0, 2, 7, 0), SimulatorError);
}

TEST(TensorGraph, Connect_SelfLoop_Throws) {
    // Arrange
    TensorGraph g;
    g.add_node(make_site(1, 1, 0, 0, 1));

    // Act + Assert: a node contracting itself is a trace, not a bond (scope).
    EXPECT_THROW(g.connect(0, 0, 0, 2), SimulatorError);
}

TEST(TensorGraph, Node_OutOfRange_Throws) {
    // Arrange
    TensorGraph g;
    g.add_node(make_site(1, 1, 0, 0, 1));

    // Act + Assert
    EXPECT_THROW(g.node(1), SimulatorError);
}

// ── MPS |0...0> initialisation ─────────────────────────────────────

TEST(TensorGraph, MpsZeroState_FiveQubits_HasChainShape) {
    // Arrange + Act
    TensorGraph mps = TensorGraph::mps_zero_state(5);

    // Assert
    EXPECT_EQ(mps.node_count(), static_cast<std::size_t>(5));
    EXPECT_EQ(mps.bond_count(), static_cast<std::size_t>(4));
    EXPECT_TRUE(mps.is_mps_chain());
}

TEST(TensorGraph, MpsZeroState_ProductState_HasUnitBondDimension) {
    // Arrange + Act
    TensorGraph mps = TensorGraph::mps_zero_state(8);

    // Assert: |0...0> carries no entanglement, so every bond stays at chi = 1.
    EXPECT_EQ(mps.max_bond_dimension(), static_cast<std::uint32_t>(1));
}

TEST(TensorGraph, MpsZeroState_EverySite_HoldsKetZero) {
    // Arrange + Act
    TensorGraph mps = TensorGraph::mps_zero_state(4);

    // Assert: each 1x2x1 site is (1, 0) — exact values, not a statistical claim.
    for (std::uint32_t i = 0; i < 4; ++i) {
        const TensorNode& site = mps.node(i);
        EXPECT_EQ(site.data.size(), static_cast<std::size_t>(2));
        EXPECT_NEAR(site.data[0].real(), 1.0, 1e-15);  // exact literal, no arithmetic
        EXPECT_NEAR(site.data[0].imag(), 0.0, 1e-15);
        EXPECT_NEAR(std::abs(site.data[1]), 0.0, 1e-15);
    }
}

TEST(TensorGraph, MpsZeroState_SingleQubit_HasNoBonds) {
    // Arrange + Act
    TensorGraph mps = TensorGraph::mps_zero_state(1);

    // Assert: boundary bonds stay explicit at dim 1, but nothing is wired.
    EXPECT_EQ(mps.node_count(), static_cast<std::size_t>(1));
    EXPECT_EQ(mps.bond_count(), static_cast<std::size_t>(0));
    EXPECT_TRUE(mps.is_mps_chain());
}

TEST(TensorGraph, MpsZeroState_PhysicalLegsFollowQubitOrder) {
    // Arrange + Act
    TensorGraph mps = TensorGraph::mps_zero_state(3);

    // Assert: site i carries physical leg i — the ordering later routines assume.
    for (std::uint32_t i = 0; i < 3; ++i) {
        const TensorIndex& phys = mps.node(i).indices[1];
        EXPECT_TRUE(phys.kind == TensorIndex::Kind::Physical);
        EXPECT_EQ(phys.id, i);
    }
}

TEST(TensorGraph, MpsZeroState_ZeroQubits_Throws) {
    // Act + Assert
    EXPECT_THROW(TensorGraph::mps_zero_state(0), SimulatorError);
}

TEST(TensorGraph, MpsZeroState_AboveQubitCeiling_Throws) {
    // Act + Assert: 128 is the structural ceiling (StateIndex shift width).
    EXPECT_THROW(TensorGraph::mps_zero_state(qplayer::kMaxTensorNetworkQubits + 1),
                 SimulatorError);
}

TEST(TensorGraph, MpsZeroState_AtQubitCeiling_Succeeds) {
    // Act + Assert: the boundary itself is legal — 128 sites of 2 amplitudes each.
    TensorGraph mps = TensorGraph::mps_zero_state(qplayer::kMaxTensorNetworkQubits);
    EXPECT_EQ(mps.node_count(), static_cast<std::size_t>(qplayer::kMaxTensorNetworkQubits));
    EXPECT_TRUE(mps.is_mps_chain());
}

// ── is_mps_chain rejects graphs that are not chains ──────────────────────────

TEST(TensorGraph, IsMpsChain_UnwiredSites_IsFalse) {
    // Arrange: two well-shaped sites that were never connected.
    TensorGraph g;
    g.add_node(make_site(1, 1, 0, 0, 1));
    g.add_node(make_site(1, 1, 1, 1, 2));

    // Act + Assert
    EXPECT_FALSE(g.is_mps_chain());
}

TEST(TensorGraph, IsMpsChain_WrongPhysicalOrder_IsFalse) {
    // Arrange: physical legs run 1, 0 instead of 0, 1.
    TensorGraph g;
    g.add_node(make_site(1, 1, /*qubit=*/1, 0, 1));
    g.add_node(make_site(1, 1, /*qubit=*/0, 1, 2));
    g.connect(0, 2, 1, 0);

    // Act + Assert
    EXPECT_FALSE(g.is_mps_chain());
}

TEST(TensorGraph, IsMpsChain_NonUnitBoundaryBond_IsFalse) {
    // Arrange: the left boundary bond is 2, so the chain has a dangling dim.
    TensorGraph g;
    g.add_node(make_site(2, 1, 0, 0, 1));
    g.add_node(make_site(1, 1, 1, 1, 2));
    g.connect(0, 2, 1, 0);

    // Act + Assert
    EXPECT_FALSE(g.is_mps_chain());
}

// ── policy constants ────────────────────────────────────────────────

TEST(TensorGraph, TruncationConstants_MatchAdr0931) {
    // Assert: the defaults are the ADR's SSOT; drifting them silently changes
    // every TN result, so the values are pinned by test (B).
    EXPECT_EQ(qplayer::kMpsDefaultMaxBond, static_cast<std::uint32_t>(256));
    EXPECT_EQ(qplayer::kMpsMaxBondCeiling, static_cast<std::uint32_t>(4096));
    EXPECT_NEAR(qplayer::kMpsDefaultCutoff, 1e-12, 1e-24);
    EXPECT_NEAR(qplayer::kMpsErrorBudgetWarn, 0.1, 1e-15);
    EXPECT_TRUE(qplayer::kMpsDefaultMaxBond <= qplayer::kMpsMaxBondCeiling);
}

// ── Backend policy rejections ────
//
// These are permanent, not "not yet implemented": TN re-contracts site tensors
// per update, which does not fit Interactive's immediate per-op model, and no
// SoA buffer exists to hand out. They must therefore raise SimulatorError, not
// NotImplementedError — the distinction is what tells a caller whether waiting
// for a later phase would help.

TEST(TensorNetworkBackend, InteractiveSurface_ThrowsSimulatorError) {
    // Arrange
    qplayer::TensorNetworkBackend backend;

    // Act + Assert
    EXPECT_THROW(backend.init_state(2), SimulatorError);
    EXPECT_THROW(backend.apply_measure(0), SimulatorError);
    EXPECT_THROW(backend.apply_reset(0), SimulatorError);
}

TEST(TensorNetworkBackend, RawStateView_ThrowsSimulatorError) {
    // Arrange
    qplayer::TensorNetworkBackend backend;

    // Act + Assert: the state is a tensor graph, so there is no SoA buffer.
    EXPECT_THROW(backend.get_statevector_raw(), SimulatorError);
    EXPECT_THROW(backend.nonzero_amplitudes(), SimulatorError);
}

// replaced the prepare()/execute() stubs with the MPS path; what remains
// unimplemented is now rejected by op family rather than wholesale.

TEST(TensorNetworkBackend, Prepare_BuildsChainOfCircuitWidth) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    qplayer::Circuit circ(4, 4);

    // Act
    backend.prepare(circ);

    // Assert
    EXPECT_EQ(backend.num_qubits(), static_cast<qplayer::QubitIndex>(4));
}

TEST(TensorNetworkBackend, Execute_BellCircuit_SamplesBothOutcomes) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    backend.set_seed(12345);
    qplayer::Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act
    backend.prepare(circ);
    qplayer::Result r = backend.execute(circ, 4000, nullptr);
    auto counts = r.counts();

    // Assert: 50/50 over |00> and |11>, and no |01>/|10> leakage.
    EXPECT_NEAR(static_cast<double>(counts["00"]) / 4000.0, 0.5, 0.03);  // nshots=4000 statistical band
    EXPECT_NEAR(static_cast<double>(counts["11"]) / 4000.0, 0.5, 0.03);
    EXPECT_EQ(counts.count("01"), static_cast<std::size_t>(0));
    EXPECT_EQ(counts.count("10"), static_cast<std::size_t>(0));
}

TEST(TensorNetworkBackend, Execute_RecordsTruncationErrorsPerTwoQubitGate) {
    // Arrange: GHZ-4 has three CX gates.
    qplayer::TensorNetworkBackend backend;
    qplayer::Circuit circ(4, 4);
    circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3);

    // Act
    backend.prepare(circ);
    qplayer::Result r = backend.execute(circ, 1, nullptr);

    // Assert: the channel is populated, one entry per 2Q gate.
    EXPECT_TRUE(r.has_truncation_errors());
    EXPECT_EQ(r.truncation_errors().size(), static_cast<std::size_t>(3));
}

TEST(TensorNetworkBackend, Execute_MatchesFullStateStatevector) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    qplayer::Circuit circ(5, 5);
    circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4).t(2).cx(0, 4);

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

    // Assert: criterion for n <= 25.
    double max_diff = 0.0;
    for (std::size_t i = 0; i < tn_sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(tn_sv[i] - fs_sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-10);
}

TEST(TensorNetworkBackend, Execute_UnsupportedOpFamilies_ThrowRatherThanSkip) {
    // Arrange: opaque is the family with no MPS application. It is now the only
    // one -- 3Q gates gained a fixed decomposition and are covered
    // TnThreeQubit in test_tn_3q.cpp, including the paths that still reject them.
    qplayer::TensorNetworkBackend backend;
    qplayer::Circuit op(2, 2);
    op.declare_opaque("u_custom", 1);
    op.h(0).opaque("u_custom", {1});

    // Act + Assert: skipping it silently would return a plausible wrong answer.
    backend.prepare(op);
    EXPECT_THROW(backend.execute(op, 16, nullptr), SimulatorError);
}

TEST(TensorNetworkBackend, SetMpsTruncation_RejectsUnhonourableValues) {
    // Arrange
    qplayer::TensorNetworkBackend backend;

    // Act + Assert: rejected at the setter, not inside a later SVD.
    EXPECT_THROW(backend.set_mps_truncation(0, 1e-12), qplayer::CircuitError);
    EXPECT_THROW(backend.set_mps_truncation(qplayer::kMpsMaxBondCeiling + 1, 1e-12),
                 qplayer::CircuitError);
    EXPECT_NO_THROW(backend.set_mps_truncation(32, 1e-10));
}

TEST(TensorNetworkBackend, SetMpsTruncation_TightCapForcesRecordedLoss) {
    // Arrange: chi = 1 cannot hold the Bell pair the circuit creates.
    qplayer::TensorNetworkBackend backend;
    qplayer::Circuit circ(2, 2);
    circ.h(0).cx(0, 1);

    // Act
    backend.set_mps_truncation(1, 1e-12);
    backend.prepare(circ);
    qplayer::Result r = backend.execute(circ, 1, nullptr);

    // Assert: the loss is reported, not hidden.
    EXPECT_TRUE(r.has_truncation_errors());
    EXPECT_TRUE(r.truncation_errors()[0] > 0.1);
}

// Regression: execute() must start from |0...0> every time. Until 2026-09-04 it
// rebuilt the chain only when the width changed, so a second run at the same
// width evolved the previous run's chain and returned a plausible-looking wrong
// distribution -- no exception, no warning. The statevector backends always
// rebuild, so this is also a cross-backend consistency guard.
TEST(TensorNetworkBackend, Execute_RepeatedRunSameWidth_RestartsFromGroundState) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    backend.set_seed(12345);
    qplayer::Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act: the same circuit twice on the same backend instance.
    qplayer::Result first  = backend.execute(circ, 4000, nullptr);
    qplayer::Result second = backend.execute(circ, 4000, nullptr);
    auto c1 = first.counts();
    auto c2 = second.counts();

    // Assert: the second run is still a Bell distribution. The leakage keys are
    // the sharp end -- carrying the evolved chain over filled all four.
    EXPECT_EQ(c2.count("01"), static_cast<std::size_t>(0));
    EXPECT_EQ(c2.count("10"), static_cast<std::size_t>(0));
    EXPECT_NEAR(static_cast<double>(c2["00"]) / 4000.0, 0.5, 0.03);  // nshots=4000 statistical band
    EXPECT_NEAR(static_cast<double>(c2["11"]) / 4000.0, 0.5, 0.03);

    // Same seed, same circuit, same starting state -> identical draws.
    EXPECT_EQ(c2["00"], c1["00"]);
    EXPECT_EQ(c2["11"], c1["11"]);
}

// The same restart guarantee through a state-preserving run: a no-measurement
// circuit leaves an evolved chain behind, which is exactly what the next run
// must not inherit. This is the shape examples/estimator_demo.py hits when a
// Sampler follows an Estimator on one register.
TEST(TensorNetworkBackend, Execute_AfterStatePreservingRun_RestartsFromGroundState) {
    // Arrange
    qplayer::TensorNetworkBackend backend;
    backend.set_seed(12345);
    qplayer::Circuit prep(2, 2);       // no measure -> the chain survives the run
    prep.h(0).cx(0, 1);
    qplayer::Circuit measured(2, 2);
    measured.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act
    backend.execute(prep, 1, nullptr);
    qplayer::Result r = backend.execute(measured, 4000, nullptr);
    auto counts = r.counts();

    // Assert: a Bell distribution, not a Bell circuit applied to a Bell state.
    EXPECT_EQ(counts.count("01"), static_cast<std::size_t>(0));
    EXPECT_EQ(counts.count("10"), static_cast<std::size_t>(0));
    EXPECT_NEAR(static_cast<double>(counts["00"]) / 4000.0, 0.5, 0.03);  // nshots=4000 statistical band
    EXPECT_NEAR(static_cast<double>(counts["11"]) / 4000.0, 0.5, 0.03);
}
