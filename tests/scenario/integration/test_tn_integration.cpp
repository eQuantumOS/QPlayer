// test_tn_integration.cpp — TN backend integration tests:
// QFT at 50 qubits through the MPS path, truncation-error history
// over random circuits, the accuracy
// criteria, and a Greedy vs MinCut contraction-cost comparison.
// 1.1, 1.3 (Arrange/Act/Assert).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/backend/tensor_network/contraction_order.hpp"
#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::QubitIndex;
using qplayer::SimulatorError;
using qplayer::TensorNetworkBackend;
using qplayer::detail::ContractionOrderOptimizer;
using qplayer::detail::ContractionStrategy;

using Cx = std::complex<double>;

namespace {

// Textbook QFT: H on each qubit followed by controlled phases from every later
// qubit, no final swaps (the bit-reversal is a relabelling, not a state change).
Circuit qft_circuit(QubitIndex n) {
    Circuit c(n, n);
    for (QubitIndex i = 0; i < n; ++i) {
        c.h(i);
        for (QubitIndex j = i + 1; j < n; ++j) {
            c.cu1(M_PI / std::pow(2.0, static_cast<double>(j - i)), j, i);
        }
    }
    return c;
}

// Brickwork of entangling pairs — the shape that actually grows chi.
Circuit random_brickwork(QubitIndex n, int layers, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> angle(0.0, 2.0 * M_PI);
    Circuit c(n, n);
    for (QubitIndex q = 0; q < n; ++q) c.h(q);
    for (int l = 0; l < layers; ++l) {
        for (QubitIndex q = static_cast<QubitIndex>(l % 2); q + 1 < n; q += 2) {
            c.cx(q, q + 1);
            c.rz(angle(rng), q + 1);   // angle first (circuit.hpp): rz(theta, qubit)
            c.cx(q, q + 1);
            c.rx(angle(rng), q);
        }
    }
    return c;
}

qplayer::StateVector full_state_of(const Circuit& circ, QubitIndex n) {
    qplayer::FullStateCpuBackend fs;
    fs.init_state(n);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }
    return fs.get_statevector();
}

std::size_t count_two_qubit_gates(const Circuit& c) {
    std::size_t n = 0;
    for (const auto& op : c.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) {
            if (g->qubits.size() == 2) ++n;
        }
    }
    return n;
}

}  // namespace

// ── QFT at 50 qubits (completion condition) ───────────────────────────────

TEST(TnIntegration, Qft50Qubits_AmplitudesAreUniform) {
    // Arrange: QFT applied to |0...0> gives the uniform superposition, so every
    // amplitude is exactly 2^-25. Entanglement stays at zero — each qubit ends in
    // |+> — which is why an MPS handles 50 qubits at all.
    const QubitIndex n = 50;
    TensorNetworkBackend backend;
    const Circuit circ = qft_circuit(n);

    // Act
    backend.prepare(circ);
    backend.execute(circ, 1, nullptr);

    // Assert: three distinct bitstrings, all at the same magnitude.
    const double expected = std::pow(2.0, -0.5 * static_cast<double>(n));
    std::vector<int> zeros(n, 0), ones(n, 1), mixed(n, 0);
    for (QubitIndex q = 0; q < n; q += 3) mixed[q] = 1;
    EXPECT_NEAR(std::abs(backend.amplitude(zeros)), expected, 1e-15);
    EXPECT_NEAR(std::abs(backend.amplitude(ones)),  expected, 1e-15);
    EXPECT_NEAR(std::abs(backend.amplitude(mixed)), expected, 1e-15);
}

TEST(TnIntegration, Qft50Qubits_StaysAtBondDimensionOne) {
    // Arrange
    const QubitIndex n = 50;
    TensorNetworkBackend backend;
    const Circuit circ = qft_circuit(n);

    // Act
    backend.prepare(circ);
    auto r = backend.execute(circ, 1, nullptr);

    // Assert: a product state needs no bond, so every truncation is lossless.
    // If chi grew here, the controlled phases would be entangling something they
    // should not — the amplitude test alone would not localise that.
    for (double e : r.truncation_errors()) EXPECT_NEAR(e, 0.0, 1e-13);
    EXPECT_THROW(backend.get_statevector(), SimulatorError);  // 2^50 is not exportable
}

TEST(TnIntegration, Qft8Qubits_MatchesFullState) {
    // Arrange: the same generator at a size FullState can check exactly, so the
    // 50-qubit test above is anchored to a verified circuit rather than to the
    // uniform-amplitude claim alone.
    const QubitIndex n = 8;
    TensorNetworkBackend backend;
    const Circuit circ = qft_circuit(n);

    // Act
    backend.prepare(circ);
    auto sv = backend.execute(circ, 1, nullptr).statevector();
    const qplayer::StateVector expected = full_state_of(circ, n);

    // Assert
    double max_diff = 0.0;
    for (std::size_t i = 0; i < sv.size(); ++i) max_diff = std::max(max_diff, std::abs(sv[i] - expected[i]));
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

// ── Truncation error history ─────────────────────────────

TEST(TnIntegration, RandomCircuit_TruncationHistoryLengthMatchesTwoQubitGateCount) {
    // Arrange: 100 random 2Q gates as specifies.
    const QubitIndex n = 8;
    const Circuit circ = random_brickwork(n, 26, 2026);
    const std::size_t two_q = count_two_qubit_gates(circ);
    TensorNetworkBackend backend;

    // Act
    backend.prepare(circ);
    auto r = backend.execute(circ, 1, nullptr);

    // Assert: one entry per 2Q gate, indexed by application order —
    // fusion is off the table here because each pair is separated by rotations.
    EXPECT_TRUE(two_q >= 100);
    EXPECT_EQ(r.truncation_errors().size(), two_q);
    EXPECT_TRUE(r.has_truncation_errors());
}

TEST(TnIntegration, RandomCircuit_TruncationErrorsAreNonNegativeAndBounded) {
    // Arrange: a tight cap forces real loss so the distribution is not all zeros.
    const QubitIndex n = 10;
    const Circuit circ = random_brickwork(n, 12, 99);
    TensorNetworkBackend backend;
    backend.set_mps_truncation(4, 1e-12);

    // Act
    backend.prepare(circ);
    auto r = backend.execute(circ, 1, nullptr);

    // Assert: each step error is a normalised discarded weight, so it lives in
    // [0, 1]; and with chi=4 on a 10-qubit brickwork at least one must be > 0.
    bool saw_loss = false;
    for (double e : r.truncation_errors()) {
        EXPECT_TRUE(e >= 0.0);
        EXPECT_TRUE(e <= 1.0);
        if (e > 1e-9) saw_loss = true;
    }
    EXPECT_TRUE(saw_loss);
}

// ── accuracy criteria ─────────────────────────────────────

TEST(TnIntegration, Adr0931_LosslessRegionMatchesFullState) {
    // Arrange: 10 qubits with a generous cap — chi never reaches the cap, so the
    // criterion is exact agreement, not "close enough".
    const QubitIndex n = 10;
    const Circuit circ = random_brickwork(n, 6, 4242);
    TensorNetworkBackend backend;

    // Act
    backend.prepare(circ);
    auto sv = backend.execute(circ, 1, nullptr).statevector();
    const qplayer::StateVector expected = full_state_of(circ, n);

    // Assert: criterion for n <= 25.
    double max_diff = 0.0;
    for (std::size_t i = 0; i < sv.size(); ++i) max_diff = std::max(max_diff, std::abs(sv[i] - expected[i]));
    EXPECT_NEAR(max_diff, 0.0, 1e-10);
}

TEST(TnIntegration, Adr0931_LargerCapMonotonicallyReducesError) {
    // Arrange: the same circuit under a rising bond cap.
    const QubitIndex n = 10;
    const Circuit circ = random_brickwork(n, 10, 555);
    const auto total_error = [&circ, n](std::uint32_t cap) {
        TensorNetworkBackend backend;
        backend.set_mps_truncation(cap, 1e-12);
        backend.prepare(circ);
        auto r = backend.execute(circ, 1, nullptr);
        double sq = 0.0;
        for (double e : r.truncation_errors()) sq += e * e;
        (void)n;
        return std::sqrt(sq);
    };

    // Act
    const double e2  = total_error(2);
    const double e8  = total_error(8);
    const double e32 = total_error(32);

    // Assert: monotonicity is the falsifiable part of — if a
    // bigger cap did not reduce the error, truncation or renormalisation is wrong.
    EXPECT_TRUE(e8 <= e2);
    EXPECT_TRUE(e32 <= e8);
    EXPECT_TRUE(e2 > 0.0);       // the tight cap really did lose weight
    EXPECT_NEAR(e32, 0.0, 1e-12);  // 2^5 = 32 covers a 10-qubit half-cut exactly
}

// ── Greedy vs MinCut contraction cost (completion condition) ──────────────

TEST(TnIntegration, GreedyVsMinCut_CostsAreComparableAndBothExecutable) {
    // Arrange: three shapes, since which strategy wins is graph-dependent.
    struct Case { const char* name; Circuit circ; };
    std::vector<Case> cases;
    {
        Circuit chain(6, 6);
        chain.h(0);
        for (QubitIndex q = 0; q + 1 < 6; ++q) chain.cx(q, q + 1);
        cases.push_back({"chain", chain});

        Circuit star(6, 6);
        star.h(0);
        for (QubitIndex q = 1; q < 6; ++q) star.cx(0, q);
        cases.push_back({"star", star});

        cases.push_back({"brickwork", random_brickwork(6, 4, 17)});
    }

    for (const Case& c : cases) {
        auto graph = qplayer::detail::build_circuit_network(c.circ);

        // Act
        auto greedy = ContractionOrderOptimizer::plan(graph, ContractionStrategy::Greedy);
        auto mincut = ContractionOrderOptimizer::plan(graph, ContractionStrategy::MinCut);

        // Assert: both plans consume the network, and both stay within a sane
        // intermediate size. The comparison is recorded rather than adjudicated —
        // neither strategy dominates, which is exactly why both exist.
        EXPECT_EQ(greedy.steps.size(), graph.node_count() - 1);
        EXPECT_EQ(mincut.steps.size(), graph.node_count() - 1);
        EXPECT_TRUE(greedy.log2_max_intermediate <= 20.0);
        EXPECT_TRUE(mincut.log2_max_intermediate <= 20.0);
    }
}

TEST(TnIntegration, GreedyAndMinCut_ProduceTheSameState) {
    // Arrange: an asymmetric circuit — a symmetric one could not detect an axis
    // permutation introduced by a different contraction order.
    Circuit circ(6, 6);
    circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3).tdg(3).cx(3, 4).cx(4, 5);

    // Act
    auto greedy = qplayer::detail::contract_to_statevector(circ, ContractionStrategy::Greedy);
    auto mincut = qplayer::detail::contract_to_statevector(circ, ContractionStrategy::MinCut);
    const qplayer::StateVector expected = full_state_of(circ, 6);

    // Assert: contraction is associative, so the order may change cost but never
    // the result — and both must match the dense engine.
    double dg = 0.0, dm = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        dg = std::max(dg, std::abs(greedy[i] - expected[i]));
        dm = std::max(dm, std::abs(mincut[i] - expected[i]));
    }
    EXPECT_NEAR(dg, 0.0, 1e-12);
    EXPECT_NEAR(dm, 0.0, 1e-12);
}

// ── : MPS is the execution representation ────────────────────────────

TEST(TnIntegration, Adr0932_NonLinearCircuitStillRunsOnMpsExactly) {
    // Arrange: a star is NonLinear, and fixes MPS as the single
    // execution representation — the topology verdict must not change the answer.
    Circuit circ(5, 5);
    circ.h(0).t(0);
    for (QubitIndex q = 1; q < 5; ++q) circ.cx(0, q);
    circ.s(3);
    TensorNetworkBackend backend;

    // Act
    backend.prepare(circ);
    auto sv = backend.execute(circ, 1, nullptr).statevector();
    const qplayer::StateVector expected = full_state_of(circ, 5);

    // Assert
    EXPECT_TRUE(backend.topology().topology == qplayer::detail::TnTopology::NonLinear);
    double max_diff = 0.0;
    for (std::size_t i = 0; i < sv.size(); ++i) max_diff = std::max(max_diff, std::abs(sv[i] - expected[i]));
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

// ── Native MPS sampling past the dense cap ─────────
//
// These are the completion evidence for : measurement distributions
// widths where the statevector cannot exist. FullState cannot reach any of them
// — 2^100 amplitudes is 10^13 TiB — so there is no reference engine to compare
// against, and each test asserts a property the true distribution must have
// rather than a number some other backend produced.

TEST(TnIntegration, Ghz100Qubits_CountsComeOutWithoutADenseVector) {
    // Arrange: GHZ keeps chi = 2 at every bond, so the chain is small while the
    // Hilbert space is 2^100. Support is exactly two bitstrings, which is a
    // property a marginal-only sampler could not reproduce.
    const QubitIndex n = 100;
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);
    circ.measure_all();
    TensorNetworkBackend backend;
    backend.set_seed(20260826ULL);

    // Act
    backend.prepare(circ);
    const auto counts = backend.execute(circ, 2000, nullptr).counts();

    // Assert
    const std::string all_zero(n, '0');
    const std::string all_one(n, '1');
    EXPECT_EQ(counts.size(), std::size_t{2});
    EXPECT_TRUE(counts.count(all_zero) == 1);
    EXPECT_TRUE(counts.count(all_one) == 1);
    std::uint64_t total = 0;
    for (const auto& kv : counts) total += kv.second;
    EXPECT_EQ(total, std::uint64_t{2000});
    // 4 sigma at n=2000 is under 0.045.
    EXPECT_NEAR(static_cast<double>(counts.at(all_zero)) / 2000.0, 0.5, 0.045);
}

TEST(TnIntegration, Brickwork100Qubits_CountsAreProducedAndNormalised) {
    // Arrange: a 1D brickwork actually grows entanglement, so this exercises the
    // sampler on a chain with real bond structure rather than the chi=2 of GHZ.
    // The bond cap keeps the SVDs small enough for a unit-test budget; the point
    // here is that a 100-qubit distribution comes out at all.
    const QubitIndex n = 100;
    Circuit circ(n, n);
    for (QubitIndex q = 0; q < n; ++q) circ.h(q);
    for (int layer = 0; layer < 4; ++layer) {
        const QubitIndex start = static_cast<QubitIndex>(layer % 2);
        for (QubitIndex q = start; q + 1 < n; q += 2) {
            circ.cx(q, q + 1);
            circ.t(q + 1);
        }
    }
    circ.measure_all();
    TensorNetworkBackend backend;
    backend.set_mps_truncation(16, 1e-10);
    backend.set_seed(4242ULL);

    // Act
    backend.prepare(circ);
    const auto counts = backend.execute(circ, 500, nullptr).counts();

    // Assert: every shot lands in exactly one bin, every key is a full-width
    // bitstring, and the distribution is broad — a sampler stuck on one
    // conditional would collapse onto a handful of keys.
    std::uint64_t total = 0;
    for (const auto& kv : counts) {
        EXPECT_EQ(kv.first.size(), static_cast<std::size_t>(n));
        total += kv.second;
    }
    EXPECT_EQ(total, std::uint64_t{500});
    EXPECT_TRUE(counts.size() > 400);
}

TEST(TnIntegration, PastTheDenseCap_CountsWorkWhileStateAccessStillRefuses) {
    // Arrange: lifts the cap on *sampling*, not on state export — the
    // statevector genuinely does not fit. Asserting both halves in one place
    // keeps a future change from quietly widening the wrong one.
    const QubitIndex n = 30;
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);
    circ.measure_all();
    TensorNetworkBackend backend;
    backend.set_seed(7ULL);

    // Act
    backend.prepare(circ);
    const auto counts = backend.execute(circ, 200, nullptr).counts();

    // Assert
    EXPECT_EQ(counts.size(), std::size_t{2});
    EXPECT_THROW(backend.get_statevector(), SimulatorError);
}

TEST(TnIntegration, WideSampling_IsReproducibleUnderAFixedSeed) {
    // Arrange: shot seeds derive from the shot index, so the
    // counts must not depend on how many threads drew them — same seed, same
    // histogram, exactly.
    const QubitIndex n = 40;
    Circuit circ(n, n);
    for (QubitIndex q = 0; q < n; ++q) circ.h(q);
    for (QubitIndex q = 0; q + 1 < n; q += 2) circ.cx(q, q + 1);
    circ.measure_all();

    // Act
    TensorNetworkBackend a;
    a.set_seed(555ULL);
    a.prepare(circ);
    const auto counts_a = a.execute(circ, 400, nullptr).counts();

    TensorNetworkBackend b;
    b.set_seed(555ULL);
    b.prepare(circ);
    const auto counts_b = b.execute(circ, 400, nullptr).counts();

    // Assert: a reproducibility claim, so exact equality is the right tolerance.
    EXPECT_TRUE(counts_a == counts_b);
    EXPECT_TRUE(counts_a.size() > 300);
}

TEST(TnIntegration, WideSampling_UnderNonIdentitySiteLayout_ReportsQubitOrder) {
    // Arrange: the sampler draws SITE-ordered bits, and the backend has to undo
    // the layout permutation before writing classical bits. Above the dense cap
    // there is no FullState reference to catch a missed permutation, so this
    // builds a circuit whose outcome is a single KNOWN basis state:
    //
    //   - the 2Q gates trace a Hamiltonian path in scrambled qubit order, which
    // lays out non-identically (asserted below, so the test cannot
    //     silently degenerate into the identity case), and
    //   - X + CX keeps the state a basis state, so the distribution is a point
    //     mass whose bit pattern the test computes independently.
    //
    // A permuted report changes which key appears, not how many — an all-ones or
    // GHZ fixture would be invariant under exactly the bug being hunted.
    const QubitIndex n = 30;
    std::vector<QubitIndex> path;
    for (QubitIndex i = 0; i < n / 2; ++i) {
        path.push_back(i);
        path.push_back(static_cast<QubitIndex>(i + n / 2));
    }

    Circuit circ(n, n);
    std::vector<int> expected_bits(n, 0);
    for (std::size_t i = 0; i < path.size(); i += 4) {
        circ.x(path[i]);
        expected_bits[path[i]] ^= 1;
    }
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        circ.cx(path[i], path[i + 1]);
        expected_bits[path[i + 1]] ^= expected_bits[path[i]];
    }
    circ.measure_all();

    TensorNetworkBackend backend;
    backend.set_seed(2024ULL);

    // Act
    backend.prepare(circ);
    const auto counts = backend.execute(circ, 64, nullptr).counts();

    // Assert: keys are MSB-first over the classical register, so qubit q sits at
    // n-1-q.
    EXPECT_FALSE(backend.topology().is_identity_order());
    std::string expected_key(n, '0');
    for (QubitIndex q = 0; q < n; ++q) {
        expected_key[static_cast<std::size_t>(n - 1u - q)] =
            static_cast<char>('0' + expected_bits[q]);
    }
    EXPECT_EQ(counts.size(), std::size_t{1});
    EXPECT_EQ(counts.begin()->first, expected_key);
    EXPECT_EQ(counts.begin()->second, std::uint64_t{64});
}
