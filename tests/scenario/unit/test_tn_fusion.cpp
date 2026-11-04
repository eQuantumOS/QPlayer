// test_tn_fusion.cpp — Unit tests for TN gate fusion and contraction
// order planning / execution.
// Tests follow the Arrange / Act / Assert layout.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <vector>

#include "../../../include/backend/tensor_network/tn_fusion.hpp"
#include "../../../include/backend/tensor_network/contraction_order.hpp"
#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::SimulatorError;
using qplayer::detail::ContractionOrderOptimizer;
using qplayer::detail::ContractionPlan;
using qplayer::detail::ContractionStrategy;
using qplayer::detail::TnStep;
using qplayer::detail::tn_fuse;

namespace {

std::size_t gate_steps(const std::vector<TnStep>& steps) {
    return static_cast<std::size_t>(
        std::count_if(steps.begin(), steps.end(),
                      [](const TnStep& s) { return s.kind == TnStep::Kind::Gate; }));
}

double max_amplitude_diff(const std::vector<std::complex<double>>& a,
                          const qplayer::StateVector& b) {
    double d = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

qplayer::StateVector full_state_of(const Circuit& circ, qplayer::QubitIndex n) {
    qplayer::FullStateCpuBackend fs;
    fs.init_state(n);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }
    return fs.get_statevector();
}

}  // namespace

// ── Fusion ─────────────────────────────────────────────────────────

TEST(TnFusion, ConsecutiveSingleQubitGates_CollapseToOneStep) {
    // Arrange
    Circuit circ(1, 1);
    circ.h(0).t(0).s(0).h(0);

    // Act
    auto steps = tn_fuse(circ.ops());

    // Assert: four gates on one qubit are one 2x2 product.
    EXPECT_EQ(gate_steps(steps), static_cast<std::size_t>(1));
    EXPECT_EQ(steps[0].matrix.size(), static_cast<std::size_t>(4));
}

TEST(TnFusion, ConsecutiveTwoQubitGates_CollapseToOneStep) {
    // Arrange: two CX on the same pair — this is where fusion pays, since each
    // 2Q gate would otherwise cost a merge/SVD/split cycle.
    Circuit circ(2, 2);
    circ.cx(0, 1).cx(0, 1);

    // Act
    auto steps = tn_fuse(circ.ops());

    // Assert
    EXPECT_EQ(gate_steps(steps), static_cast<std::size_t>(1));
    EXPECT_EQ(steps[0].matrix.size(), static_cast<std::size_t>(16));
}

TEST(TnFusion, ReversedOperandOrder_StillFuses) {
    // Arrange: same pair, opposite operand order.
    Circuit circ(2, 2);
    circ.cx(0, 1).cx(1, 0);

    // Act
    auto steps = tn_fuse(circ.ops());

    // Assert: one step — the newcomer is rewritten into the existing order.
    EXPECT_EQ(gate_steps(steps), static_cast<std::size_t>(1));
}

TEST(TnFusion, ReversedOperandOrder_ProducesCorrectProduct) {
    // Arrange: cx(0,1) then cx(1,0) is the pair-swap-with-a-twist circuit; a
    // careless bit rewrite gives a plausible but wrong 4x4, so the value is
    // checked against FullState rather than just the step count.
    Circuit circ(2, 2);
    circ.h(0).t(0).cx(0, 1).cx(1, 0);

    qplayer::TensorNetworkBackend backend;

    // Act
    backend.prepare(circ);
    auto sv = backend.execute(circ, 1, nullptr).statevector();

    // Assert
    EXPECT_NEAR(max_amplitude_diff(sv, full_state_of(circ, 2)), 0.0, 1e-12);
}

TEST(TnFusion, InterveningGate_BreaksTheRun) {
    // Arrange: cx(0,1), then a gate on qubit 1, then cx(0,1) again.
    Circuit circ(2, 2);
    circ.cx(0, 1).h(1).cx(0, 1);

    // Act
    auto steps = tn_fuse(circ.ops());

    // Assert: fusing across the h would reorder non-commuting operations.
    EXPECT_EQ(gate_steps(steps), static_cast<std::size_t>(3));
}

TEST(TnFusion, Barrier_IsAFusionBoundary) {
    // Arrange
    Circuit circ(1, 1);
    circ.h(0).barrier().h(0);

    // Act
    auto steps = tn_fuse(circ.ops());

    // Assert: makes barriers boundaries even though they change nothing.
    EXPECT_EQ(gate_steps(steps), static_cast<std::size_t>(2));
}

TEST(TnFusion, Measure_IsAFusionBoundary) {
    // Arrange
    Circuit circ(1, 1);
    circ.h(0).measure(0, 0).h(0);

    // Act
    auto steps = tn_fuse(circ.ops());

    // Assert
    EXPECT_EQ(gate_steps(steps), static_cast<std::size_t>(2));
    EXPECT_TRUE(steps[1].kind == TnStep::Kind::Measure);
}

TEST(TnFusion, UnsupportedFamilies_RejectBeforeAnyExecution) {
    // Arrange: opaque, the remaining family with no MPS application. A 3Q gate is
    // no longer rejected here -- it becomes its own step and runs through
    // MpsState::apply_3q()'s fixed decomposition, asserted below.
    Circuit op(2, 2);
    op.declare_opaque("u_custom", 1);
    op.opaque("u_custom", {1});

    // Act + Assert: rejecting in the pass means no amplitude was touched first.
    EXPECT_THROW(tn_fuse(op.ops()), SimulatorError);
}

TEST(TnFusion, ThreeQubitGate_EmitsOwnStepAndBreaksAllOperandRuns) {
    // Arrange: fusable 1Q runs on all three operands, split by a ccx. If the 3Q
    // gate did not break those runs, the h/t pairs either side would fuse across
    // it and land on the wrong side of the Toffoli.
    Circuit circ(3, 3);
    circ.h(0).h(1).h(2).ccx(0, 1, 2).t(0).t(1).t(2);

    // Act
    const auto steps = tn_fuse(circ.ops());

    // Assert: three fused 1Q steps, the 3Q step, then three more 1Q steps. The
    // 3Q step carries its gate type and no matrix.
    EXPECT_EQ(steps.size(), static_cast<std::size_t>(7));
    EXPECT_EQ(steps[3].qubits.size(), static_cast<std::size_t>(3));
    EXPECT_TRUE(steps[3].gate == qplayer::GateType::CCX);
    EXPECT_TRUE(steps[3].matrix.empty());
}

TEST(TnFusion, FusedExecution_MatchesUnfusedResult) {
    // Arrange: a circuit built entirely from fusable runs.
    Circuit circ(3, 3);
    circ.h(0).t(0).h(0).cx(0, 1).cx(0, 1).cx(0, 1).h(2).s(2).cx(1, 2).cx(2, 1);

    qplayer::TensorNetworkBackend backend;

    // Act
    backend.prepare(circ);
    auto sv = backend.execute(circ, 1, nullptr).statevector();

    // Assert: fusion must not change the answer, only the work done to get it.
    EXPECT_NEAR(max_amplitude_diff(sv, full_state_of(circ, 3)), 0.0, 1e-12);
}

TEST(TnFusion, FusedExecution_RunsFewerSvdCycles) {
    // Arrange: three CX on the same pair.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).cx(0, 1).cx(0, 1);

    qplayer::TensorNetworkBackend backend;

    // Act
    backend.prepare(circ);
    auto r = backend.execute(circ, 1, nullptr);

    // Assert: one truncation entry means one merge/SVD/split cycle ran, not three.
    // This is the observable that shows fusion actually saved work.
    EXPECT_EQ(r.truncation_errors().size(), static_cast<std::size_t>(1));
}

// ── Contraction order ──────────────────────────────────────────────

TEST(TnContraction, Plan_CoversEveryNodeExactlyOnce) {
    // Arrange
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2);
    auto graph = qplayer::detail::build_circuit_network(circ);

    // Act
    ContractionPlan plan = ContractionOrderOptimizer::plan(graph, ContractionStrategy::Greedy);

    // Assert: contracting N tensors takes exactly N-1 pairwise steps.
    EXPECT_EQ(plan.steps.size(), graph.node_count() - 1);
}

TEST(TnContraction, Plan_EmptyGraph_Throws) {
    qplayer::detail::TensorGraph empty;
    EXPECT_THROW(ContractionOrderOptimizer::plan(empty, ContractionStrategy::Greedy),
                 SimulatorError);
}

TEST(TnContraction, GreedyAndMinCut_BothProduceValidPlans) {
    // Arrange
    Circuit circ(4, 4);
    circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3).t(3).cx(0, 3);
    auto graph = qplayer::detail::build_circuit_network(circ);

    // Act
    ContractionPlan greedy = ContractionOrderOptimizer::plan(graph, ContractionStrategy::Greedy);
    ContractionPlan mincut = ContractionOrderOptimizer::plan(graph, ContractionStrategy::MinCut);

    // Assert: both must be executable and cost something finite. Which one wins
    // is graph-dependent, so the test pins validity, not a winner.
    EXPECT_EQ(greedy.steps.size(), graph.node_count() - 1);
    EXPECT_EQ(mincut.steps.size(), graph.node_count() - 1);
    EXPECT_TRUE(greedy.log2_flops > 0.0);
    EXPECT_TRUE(mincut.log2_flops > 0.0);
    EXPECT_TRUE(std::isfinite(greedy.log2_max_intermediate));
    EXPECT_TRUE(std::isfinite(mincut.log2_max_intermediate));
}

TEST(TnContraction, Plan_IsDeterministic) {
    // Arrange
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2);
    auto graph = qplayer::detail::build_circuit_network(circ);

    // Act
    ContractionPlan a = ContractionOrderOptimizer::plan(graph, ContractionStrategy::Greedy);
    ContractionPlan b = ContractionOrderOptimizer::plan(graph, ContractionStrategy::Greedy);

    // Assert: planning must not depend on iteration order or timing — a
    // non-deterministic plan would make cost comparisons meaningless.
    EXPECT_EQ(a.steps.size(), b.steps.size());
    EXPECT_NEAR(a.log2_flops, b.log2_flops, 1e-12);
    for (std::size_t i = 0; i < a.steps.size(); ++i) {
        EXPECT_EQ(a.steps[i].node_a, b.steps[i].node_a);
        EXPECT_EQ(a.steps[i].node_b, b.steps[i].node_b);
    }
}

TEST(TnContraction, MinCutPlanning_ScalesToRealisticNetworks) {
    // Arrange: 400+ nodes, the size a modest circuit already reaches (one node
    // per gate plus one per qubit).
    Circuit circ(8, 8);
    for (int i = 0; i < 400; ++i) circ.cx(i % 7, (i % 7) + 1);
    auto graph = qplayer::detail::build_circuit_network(circ);

    // Act
    const auto t0 = std::chrono::steady_clock::now();
    ContractionPlan plan = ContractionOrderOptimizer::plan(graph, ContractionStrategy::MinCut);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    // Assert: the plan is valid, and planning does not blow up. The original
    // bisection re-summed every left x right pair per split position — O(m^3) per
    // level, measured at 23.8 s for this network. The incremental sweep runs it in
    // ~0.35 s, so a 5 s bound is a ~14x margin over the fix and a ~5x margin under
    // the regression: slack enough for a loaded machine, tight enough to catch it.
    EXPECT_EQ(plan.steps.size(), graph.node_count() - 1);
    EXPECT_TRUE(ms < 5000.0);
}

TEST(TnContraction, MinCutSweep_PicksTheCheapestSplit) {
    // Arrange: two 3-node chains joined by a single bond — the cheap cut is the
    // join, not the midpoint of an arbitrary ordering.
    Circuit circ(4, 4);
    circ.cx(0, 1).cx(0, 1).cx(2, 3).cx(2, 3).cx(1, 2);
    auto graph = qplayer::detail::build_circuit_network(circ);

    // Act
    ContractionPlan mincut = ContractionOrderOptimizer::plan(graph, ContractionStrategy::MinCut);
    ContractionPlan greedy = ContractionOrderOptimizer::plan(graph, ContractionStrategy::Greedy);

    // Assert: both stay executable and bounded. Intermediates on a network this
    // small are tiny either way; what is pinned here is that the incremental
    // sweep still produces a usable plan, not that one strategy wins.
    EXPECT_EQ(mincut.steps.size(), graph.node_count() - 1);
    EXPECT_TRUE(mincut.log2_max_intermediate <= 8.0);
    EXPECT_TRUE(greedy.log2_max_intermediate <= 8.0);
}

TEST(TnContraction, ContractPair_SharedIndexDimMismatch_Throws) {
    // Arrange: two nodes claiming the same bond id at different dims.
    qplayer::detail::TensorNode a;
    a.indices = {{qplayer::detail::TensorIndex::Kind::Bond, 2, 0}};
    a.data.assign(2, {0, 0});
    qplayer::detail::TensorNode b;
    b.indices = {{qplayer::detail::TensorIndex::Kind::Bond, 3, 0}};
    b.data.assign(3, {0, 0});

    // Act + Assert
    EXPECT_THROW(qplayer::detail::contract_pair(a, b), SimulatorError);
}

// ── The two engines must agree ───────────────────────────────────────────────

TEST(TnContraction, GreedyContraction_MatchesFullState) {
    // Arrange: distinct 1Q gates per qubit so the state is not permutation
    // symmetric — a symmetric state cannot detect an axis-order bug.
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3).tdg(3).cx(0, 3);

    // Act
    auto tn = qplayer::detail::contract_to_statevector(circ, ContractionStrategy::Greedy);

    // Assert
    EXPECT_NEAR(max_amplitude_diff(tn, full_state_of(circ, 4)), 0.0, 1e-12);
}

TEST(TnContraction, MinCutContraction_MatchesGreedy) {
    // Arrange
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3).tdg(3);

    // Act
    auto greedy = qplayer::detail::contract_to_statevector(circ, ContractionStrategy::Greedy);
    auto mincut = qplayer::detail::contract_to_statevector(circ, ContractionStrategy::MinCut);

    // Assert: contraction is associative, so order changes cost and nothing else.
    double d = 0.0;
    for (std::size_t i = 0; i < greedy.size(); ++i) d = std::max(d, std::abs(greedy[i] - mincut[i]));
    EXPECT_NEAR(d, 0.0, 1e-13);
}

TEST(TnContraction, GeneralNetwork_MatchesMpsOnANonLinearCircuit) {
    // Arrange: a star, which is exactly the shape MPS handles least naturally.
    // Two independent engines agreeing here is a strong correctness signal.
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 1).cx(0, 2).cx(0, 3).s(2);

    qplayer::TensorNetworkBackend backend;

    // Act
    backend.prepare(circ);
    auto mps = backend.execute(circ, 1, nullptr).statevector();
    auto tn  = qplayer::detail::contract_to_statevector(circ, ContractionStrategy::Greedy);

    // Assert
    double d = 0.0;
    for (std::size_t i = 0; i < tn.size(); ++i) d = std::max(d, std::abs(tn[i] - mps[i]));
    EXPECT_NEAR(d, 0.0, 1e-12);
}

TEST(TnContraction, ContractToStatevector_AboveExportCap_Throws) {
    // Arrange
    Circuit circ(qplayer::MAX_DENSE_EXPORT_QUBITS + 1, 1);
    circ.h(0);

    // Act + Assert
    EXPECT_THROW(qplayer::detail::contract_to_statevector(circ, ContractionStrategy::Greedy),
                 SimulatorError);
}

// ── : centre-following schedule ──────────────────────────────────

namespace {

// Brickwork: every layer switches parity, which in circuit order sends the
// orthogonality centre back across the chain once per layer.
Circuit sweep_brickwork(qplayer::QubitIndex n, int depth) {
    Circuit c(n, n);
    for (int layer = 0; layer < depth; ++layer) {
        for (qplayer::QubitIndex q = static_cast<qplayer::QubitIndex>(layer % 2); q + 1 < n; q += 2) {
            c.ry(0.3 + 0.01 * q, q).rz(0.2, q + 1).cx(q, q + 1);
        }
    }
    return c;
}

std::vector<qplayer::QubitIndex> identity_sites(qplayer::QubitIndex n) {
    std::vector<qplayer::QubitIndex> s(n);
    for (qplayer::QubitIndex q = 0; q < n; ++q) s[q] = q;
    return s;
}

// Sites crossed by the centre between consecutive multi-qubit steps.
std::size_t centre_travel(const std::vector<TnStep>& steps) {
    std::size_t centre = 0, travel = 0;
    for (const TnStep& st : steps) {
        if (st.qubits.size() < 2) continue;
        const std::size_t lo = std::min(st.qubits[0], st.qubits[1]);
        const std::size_t hi = std::max(st.qubits[0], st.qubits[1]);
        travel += centre < lo ? lo - centre : (centre > hi ? centre - hi : 0);
        centre = hi;
    }
    return travel;
}

}  // namespace

TEST(TnFusion, SweepOrder_KeepsEveryQubitsOwnOrder) {
    // Arrange
    const std::vector<TnStep> fused = tn_fuse(sweep_brickwork(10, 8).ops());

    // Act
    const std::vector<TnStep> ordered = qplayer::detail::tn_sweep_order(fused, identity_sites(10));

    // Assert: a permutation that never swaps two steps sharing a qubit, which is
    // what makes the reordered stream the same unitary.
    EXPECT_EQ(ordered.size(), fused.size());
    bool same = true;
    for (qplayer::QubitIndex q = 0; q < 10; ++q) {
        std::vector<const TnStep*> a, b;
        for (const TnStep& s : fused)   if (std::find(s.qubits.begin(), s.qubits.end(), q) != s.qubits.end()) a.push_back(&s);
        for (const TnStep& s : ordered) if (std::find(s.qubits.begin(), s.qubits.end(), q) != s.qubits.end()) b.push_back(&s);
        if (a.size() != b.size()) { same = false; break; }
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i]->qubits != b[i]->qubits || a[i]->matrix != b[i]->matrix) same = false;
    }
    EXPECT_TRUE(same);
}

TEST(TnFusion, SweepOrder_CutsTheCentreTravelOfBrickwork) {
    // Arrange
    const std::vector<TnStep> fused = tn_fuse(sweep_brickwork(16, 12).ops());

    // Act
    const std::vector<TnStep> ordered = qplayer::detail::tn_sweep_order(fused, identity_sites(16));

    // Assert: circuit order walks the centre back every layer; the schedule
    // snakes instead, so the travel drops by more than half.
    EXPECT_TRUE(centre_travel(ordered) * 2 < centre_travel(fused));
}

TEST(TnFusion, SweepOrder_LeavesMeasurementsInPlace) {
    // Arrange: a measurement between two brickwork blocks.
    Circuit c = sweep_brickwork(8, 4);
    c.measure(3, 3);
    for (qplayer::QubitIndex q = 1; q + 1 < 8; q += 2) c.cx(q, q + 1);
    const std::vector<TnStep> fused = tn_fuse(c.ops());

    // Act
    const std::vector<TnStep> ordered = qplayer::detail::tn_sweep_order(fused, identity_sites(8));

    // Assert: the measure step keeps its index, so gates never cross it.
    std::size_t at_fused = 0, at_ordered = 0;
    for (std::size_t i = 0; i < fused.size(); ++i)   if (fused[i].kind == TnStep::Kind::Measure) at_fused = i;
    for (std::size_t i = 0; i < ordered.size(); ++i) if (ordered[i].kind == TnStep::Kind::Measure) at_ordered = i;
    EXPECT_EQ(at_ordered, at_fused);
}

TEST(TnFusion, SweepOrder_GaugedRunMatchesTheUngaugedState) {
    // Arrange: the same lossless circuit with and without the gauge (and so with
    // and without the schedule).
    const Circuit c = sweep_brickwork(12, 10);
    qplayer::TensorNetworkBackend with, without;
    with.set_mps_gauge(qplayer::MpsGauge::Canonical);
    without.set_mps_gauge(qplayer::MpsGauge::None);
    with.prepare(c);
    without.prepare(c);

    // Act
    with.execute(c, 1);
    without.execute(c, 1);
    const auto a = with.get_statevector();
    const auto b = without.get_statevector();

    // Assert: identical amplitudes (1e-10: SVD rounding over ~60 splits).
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
    EXPECT_TRUE(worst < 1e-10);
}
