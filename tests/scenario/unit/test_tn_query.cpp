// test_tn_query.cpp — Unit tests for TN amplitude and reduced density
// matrix modes, plus the IdleOp fusion boundary,.
// 1.1, 1.3 (Arrange/Act/Assert).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/backend/tensor_network/mps.hpp"
#include "../../../include/backend/tensor_network/tn_fusion.hpp"
#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::QubitIndex;
using qplayer::SimulatorError;
using qplayer::TensorNetworkBackend;

using Cx = std::complex<double>;

namespace {

// Runs a circuit on the TN backend and hands the prepared backend back.
void run_on_tn(TensorNetworkBackend& backend, const Circuit& circ) {
    backend.prepare(circ);
    backend.execute(circ, 1, nullptr);
}

qplayer::StateVector full_state_of(const Circuit& circ, QubitIndex n) {
    qplayer::FullStateCpuBackend fs;
    fs.init_state(n);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }
    return fs.get_statevector();
}

std::vector<int> bits_of(std::size_t basis, QubitIndex n) {
    std::vector<int> bits(n, 0);
    for (QubitIndex q = 0; q < n; ++q) bits[q] = static_cast<int>((basis >> q) & 1u);
    return bits;
}

}  // namespace

// ── Amplitude mode ─────────────────────────────────────────────────

TEST(TnQuery, Amplitude_MatchesEveryStatevectorEntry) {
    // Arrange: distinct 1Q gates per qubit so no two amplitudes coincide by
    // symmetry — a symmetric state would let an index bug pass.
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3).tdg(3);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    const qplayer::StateVector expected = full_state_of(circ, 4);

    // Act + Assert: every basis state, one at a time, with no statevector built.
    double max_diff = 0.0;
    for (std::size_t basis = 0; basis < expected.size(); ++basis) {
        const Cx amp = backend.amplitude(bits_of(basis, 4));
        max_diff = std::max(max_diff, std::abs(amp - expected[basis]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

TEST(TnQuery, Amplitude_BitstringIsQubitIndexed) {
    // Arrange: |100> — qubit 0 set, qubits 1 and 2 clear.
    Circuit circ(3, 3);
    circ.x(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act + Assert: bitstring[q] is qubit q, so {1,0,0} is the populated state.
    EXPECT_NEAR(std::abs(backend.amplitude({1, 0, 0})), 1.0, 1e-14);
    EXPECT_NEAR(std::abs(backend.amplitude({0, 0, 1})), 0.0, 1e-14);
}

TEST(TnQuery, Amplitude_WorksUnderANonIdentitySiteLayout) {
    // Arrange: the path 0-3-1-2, which lays out non-identically. The
    // bitstring must be permuted into site order before the sweep.
    //
    // The trailing h(2)/t(1) matter: a bare GHZ chain populates only all-zeros
    // and all-ones, and BOTH are invariant under every qubit permutation — such
    // a state cannot tell a missing layout mapping from a correct one. Spreading
    // the amplitudes over asymmetric basis states is what makes this test bite.
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 3).s(3).cx(3, 1).cx(1, 2).h(2).t(1);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    const qplayer::StateVector expected = full_state_of(circ, 4);

    // Act + Assert
    EXPECT_FALSE(backend.topology().is_identity_order());
    double max_diff = 0.0;
    for (std::size_t basis = 0; basis < expected.size(); ++basis) {
        max_diff = std::max(max_diff, std::abs(backend.amplitude(bits_of(basis, 4)) - expected[basis]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

TEST(TnQuery, Amplitude_BeyondTheDenseExportCap) {
    // Arrange: 40 qubits — get_statevector() is impossible here, which is the
    // entire reason the amplitude mode exists.
    const QubitIndex n = 40;
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act: a 40-qubit GHZ has amplitude 1/sqrt2 on all-zeros and all-ones only.
    std::vector<int> zeros(n, 0);
    std::vector<int> ones(n, 1);
    std::vector<int> mixed(n, 0);
    mixed[7] = 1;

    // Assert
    EXPECT_NEAR(std::abs(backend.amplitude(zeros)), M_SQRT1_2, 1e-12);
    EXPECT_NEAR(std::abs(backend.amplitude(ones)),  M_SQRT1_2, 1e-12);
    EXPECT_NEAR(std::abs(backend.amplitude(mixed)), 0.0, 1e-12);
    EXPECT_THROW(backend.get_statevector(), SimulatorError);  // 2^40 is not exportable
}

TEST(TnQuery, Amplitude_WrongLength_Throws) {
    Circuit circ(3, 3);
    circ.h(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    EXPECT_THROW(backend.amplitude({0, 1}), SimulatorError);
}

TEST(TnQuery, Amplitude_BeforePrepare_Throws) {
    TensorNetworkBackend backend;
    EXPECT_THROW(backend.amplitude({0}), SimulatorError);
}

// ── Reduced density matrix ─────────────────────────────────────────

TEST(TnQuery, Rdm_BellPair_SingleQubitIsMaximallyMixed) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act
    auto rho = backend.reduced_density_matrix({0});

    // Assert: tracing out half a Bell pair leaves I/2 exactly — diagonal 0.5,
    // off-diagonal 0. This is the textbook case that catches a missing conjugate.
    EXPECT_EQ(rho.size(), static_cast<std::size_t>(4));
    EXPECT_NEAR(rho[0].real(), 0.5, 1e-12);
    EXPECT_NEAR(rho[3].real(), 0.5, 1e-12);
    EXPECT_NEAR(std::abs(rho[1]), 0.0, 1e-12);
    EXPECT_NEAR(std::abs(rho[2]), 0.0, 1e-12);
}

TEST(TnQuery, Rdm_ProductState_IsPure) {
    // Arrange: |+> on qubit 0, untouched qubit 1.
    Circuit circ(2, 2);
    circ.h(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act
    auto rho = backend.reduced_density_matrix({0});

    // Assert: a pure |+> has every entry 0.5 — including the off-diagonals that
    // a maximally mixed state would have lost.
    for (const Cx& e : rho) EXPECT_NEAR(e.real(), 0.5, 1e-12);
}

TEST(TnQuery, Rdm_HasUnitTraceAndIsHermitian) {
    // Arrange: an asymmetric entangled state.
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act
    auto rho = backend.reduced_density_matrix({1, 2});

    // Assert: trace 1 and rho = rho^dagger are the invariants any density matrix
    // must satisfy; violating them means the sweep lost or double-counted weight.
    const std::size_t dim = 4;
    Cx trace{0.0, 0.0};
    for (std::size_t i = 0; i < dim; ++i) trace += rho[i * dim + i];
    EXPECT_NEAR(trace.real(), 1.0, 1e-12);
    EXPECT_NEAR(trace.imag(), 0.0, 1e-12);

    double herm = 0.0;
    for (std::size_t r = 0; r < dim; ++r) {
        for (std::size_t c = 0; c < dim; ++c) {
            herm = std::max(herm, std::abs(rho[r * dim + c] - std::conj(rho[c * dim + r])));
        }
    }
    EXPECT_NEAR(herm, 0.0, 1e-12);
}

TEST(TnQuery, Rdm_MatchesFullStatePartialTrace) {
    // Arrange: compute the same RDM from the dense statevector as ground truth.
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3).tdg(3).cx(0, 3);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    const qplayer::StateVector sv = full_state_of(circ, 4);

    const std::vector<QubitIndex> keep{0, 2};  // deliberately non-adjacent
    const std::size_t dim = 4;
    std::vector<Cx> expected(dim * dim, Cx{0, 0});
    for (std::size_t basis = 0; basis < sv.size(); ++basis) {
        for (std::size_t other = 0; other < sv.size(); ++other) {
            // Same value on every traced-out qubit, else the term drops.
            bool rest_matches = true;
            for (QubitIndex q = 0; q < 4; ++q) {
                if (std::find(keep.begin(), keep.end(), q) != keep.end()) continue;
                if (((basis >> q) & 1u) != ((other >> q) & 1u)) { rest_matches = false; break; }
            }
            if (!rest_matches) continue;
            std::size_t r = 0, c = 0;
            for (std::size_t i = 0; i < keep.size(); ++i) {
                if ((basis >> keep[i]) & 1u) r |= std::size_t{1} << i;
                if ((other >> keep[i]) & 1u) c |= std::size_t{1} << i;
            }
            expected[r * dim + c] += sv[basis] * std::conj(sv[other]);
        }
    }

    // Act
    auto rho = backend.reduced_density_matrix(keep);

    // Assert
    double max_diff = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(rho[i] - expected[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

TEST(TnQuery, Rdm_BitOrderFollowsAscendingQubits) {
    // Arrange: qubit 0 is |1>, qubit 2 is |0> — an asymmetric pair, so a swapped
    // bit order lands the population on the wrong diagonal entry.
    Circuit circ(3, 3);
    circ.x(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act
    auto rho = backend.reduced_density_matrix({0, 2});

    // Assert: bit 0 = qubit 0 (set), bit 1 = qubit 2 (clear) -> index 1.
    EXPECT_NEAR(rho[1 * 4 + 1].real(), 1.0, 1e-12);
    EXPECT_NEAR(rho[2 * 4 + 2].real(), 0.0, 1e-12);
}

TEST(TnQuery, Rdm_UnderNonIdentityLayout_KeepsQubitBitOrder) {
    // Arrange: a layout-permuting path plus an asymmetric state.
    Circuit circ(4, 4);
    circ.x(0).cx(0, 3).cx(3, 1).cx(1, 2).x(2);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act
    auto rho_full = backend.reduced_density_matrix({0, 1, 2, 3});

    // Assert: the state is |1111> XOR the x(2) flip -> qubits 0,1,3 set, 2 clear
    // -> index 0b1011 = 11. A site-ordered result would put it elsewhere.
    EXPECT_FALSE(backend.topology().is_identity_order());
    EXPECT_NEAR(rho_full[11 * 16 + 11].real(), 1.0, 1e-12);
}

TEST(TnQuery, Rdm_TooManyQubits_Throws) {
    Circuit circ(14, 14);
    circ.h(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    std::vector<QubitIndex> too_many;
    for (QubitIndex q = 0; q <= qplayer::kMaxRdmSites; ++q) too_many.push_back(q);

    // 4^k entries: the cap is about memory, so it must reject rather than try.
    EXPECT_THROW(backend.reduced_density_matrix(too_many), SimulatorError);
}

TEST(TnQuery, Rdm_RepeatedQubit_Throws) {
    Circuit circ(3, 3);
    circ.h(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    EXPECT_THROW(backend.reduced_density_matrix({1, 1}), SimulatorError);
}

TEST(TnQuery, Rdm_QubitOutOfRange_Throws) {
    Circuit circ(3, 3);
    circ.h(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    EXPECT_THROW(backend.reduced_density_matrix({3}), SimulatorError);
}

// ── IdleOp dispatch slot ──────────────────────────────────────────

TEST(TnQuery, IdleOp_IsAFusionBoundary) {
    // Arrange: two gates on one qubit separated by an idle period.
    std::vector<qplayer::Op> ops;
    qplayer::GateOp h;
    h.type = qplayer::GateType::H;
    h.name = "h";
    h.qubits = {0};
    ops.push_back(h);
    ops.push_back(qplayer::IdleOp{0, 120.0});
    ops.push_back(h);

    // Act
    auto steps = qplayer::detail::tn_fuse(ops);

    // Assert: makes IdleOp a fusion boundary. It changes
    // nothing today, but Phase 10 applies a relaxation channel there — fusing
    // across it would move gates to the wrong side of that channel.
    const std::size_t gates = static_cast<std::size_t>(
        std::count_if(steps.begin(), steps.end(),
                      [](const qplayer::detail::TnStep& s) {
                          return s.kind == qplayer::detail::TnStep::Kind::Gate;
                      }));
    EXPECT_EQ(gates, static_cast<std::size_t>(2));
}

TEST(TnQuery, IdleOp_DoesNotChangeTheState) {
    // Arrange: idle with no noise model is identity (is Phase 10).
    std::vector<qplayer::Op> ops;
    qplayer::GateOp h;
    h.type = qplayer::GateType::H;
    h.name = "h";
    h.qubits = {0};
    ops.push_back(h);
    ops.push_back(qplayer::IdleOp{0, 500.0});

    // Act
    auto steps = qplayer::detail::tn_fuse(ops);

    // Assert: no step is emitted for the idle itself.
    EXPECT_EQ(steps.size(), static_cast<std::size_t>(1));
}

// ── Pauli expectation values ────────

TEST(TnQuery, Expectation_SingleQubitPauliOnAKnownState) {
    // Arrange: h(0) puts qubit 0 in |+>, so <X> = 1, <Z> = 0 exactly. Verifying
    // a hand-derivable state first isolates a sign or transpose error in the
    // 2x2 factor from a bug in the sweep itself.
    Circuit circ(1, 1);
    circ.h(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act + Assert: analytic values, so the tolerance is numerical only.
    EXPECT_NEAR(backend.expectation_value("X"),  1.0, 1e-12);
    EXPECT_NEAR(backend.expectation_value("Z"),  0.0, 1e-12);
    EXPECT_NEAR(backend.expectation_value("I"),  1.0, 1e-12);
}

TEST(TnQuery, Expectation_YFactorHasTheRightSign) {
    // Arrange: s(0) after h(0) gives (|0> + i|1>)/sqrt(2), whose <Y> = +1. A
    // transposed Y matrix returns -1 here — the failure the sign convention in
    // pauli_element() guards against.
    Circuit circ(1, 1);
    circ.h(0).s(0);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act + Assert
    EXPECT_NEAR(backend.expectation_value("Y"), 1.0, 1e-12);
}

TEST(TnQuery, Expectation_BellPairCorrelators) {
    // Arrange: the Bell state has <ZZ> = <XX> = 1 and <YY> = -1, which pins the
    // two-site factor placement and the Y sign together.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act + Assert
    EXPECT_NEAR(backend.expectation_value("ZZ"),  1.0, 1e-12);
    EXPECT_NEAR(backend.expectation_value("XX"),  1.0, 1e-12);
    EXPECT_NEAR(backend.expectation_value("YY"), -1.0, 1e-12);
    EXPECT_NEAR(backend.expectation_value("ZI"),  0.0, 1e-12);
}

TEST(TnQuery, Expectation_MatchesFullStateOnAnAsymmetricCircuit) {
    // Arrange: the cross-backend reference. The circuit is deliberately
    // asymmetric under qubit permutation — a GHZ-like state is invariant under
    // it and would hide a qubit/site mix-up (the trap and both hit).
    Circuit circ(4, 4);
    circ.h(0).t(0).cx(0, 1).s(1).cx(1, 2).h(2).cx(2, 3).t(3).rx(0, 0.37).ry(3, 1.11);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    qplayer::FullStateCpuBackend fs;
    fs.init_state(4);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }

    // Act + Assert: same Pauli string on both backends must agree to numerical
    // precision — the strings also exercise mixed I/X/Y/Z placement.
    for (const char* pauli : {"ZZZZ", "XIII", "IIIX", "XYZI", "IZXY", "YYYY"}) {
        const double tn = backend.expectation_value(pauli);
        const double ref = fs.expectation_value(pauli);
        EXPECT_NEAR(tn, ref, 1e-12);
    }
}

TEST(TnQuery, Expectation_EmptyStringIsAllZ) {
    // Arrange: the shared contract says "" means Z on every qubit.
    // Reusing compile_pauli_ops() is what makes this hold on both backends.
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).t(2);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act + Assert
    EXPECT_NEAR(backend.expectation_value(""), backend.expectation_value("ZZZ"), 1e-12);
}

TEST(TnQuery, Expectation_UnderNonIdentitySiteLayout) {
    // Arrange: the 0-3-1-2 path again. Pauli factors are indexed by qubit but
    // applied by site, so a missing permutation puts Z on the wrong qubit.
    //
    // The state must be only PARTIALLY entangled. A GHZ-like chain (h + CX
    // chain) leaves every single-qubit RDM maximally mixed, so every
    // single-qubit <P> is 0 and the assertions below are invariant under ANY
    // site permutation — no local rotation can fix that, because local unitaries
    // cannot change a maximally mixed RDM. Two mutation rounds survived on such
    // a circuit before this was diagnosed. Distinct pre-rotation angles ahead of
    // the CX chain keep each qubit's <Z> distinct and non-zero, which is what
    // makes a misplaced Pauli factor observable.
    Circuit circ(4, 4);
    circ.ry(0, 0.31).ry(1, 0.73).ry(2, 1.14).ry(3, 0.52);
    circ.cx(0, 3).cx(3, 1).cx(1, 2);
    circ.t(0).rx(2, 0.41);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);
    qplayer::FullStateCpuBackend fs;
    fs.init_state(4);
    for (const auto& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) fs.apply_gate(*g);
    }

    // Act + Assert
    EXPECT_FALSE(backend.topology().is_identity_order());
    for (const char* pauli : {"ZIII", "IZII", "IIZI", "IIIZ", "XYZI"}) {
        EXPECT_NEAR(backend.expectation_value(pauli), fs.expectation_value(pauli), 1e-12);
    }
}

TEST(TnQuery, Expectation_BeyondTheDenseExportCap) {
    // Arrange: 40 qubits. get_statevector() throws here, so this is the whole
    // point of computing <P> by contraction (section 11.7 "every qubit count").
    const QubitIndex n = 40;
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act + Assert: a 40-qubit GHZ state. <Z...Z> = 1 because both terms carry
    // an even number of ones, while <Z> on a single qubit is 0 — the pair is
    // what distinguishes a real correlator from a stray all-ones answer.
    EXPECT_THROW(backend.get_statevector(), SimulatorError);
    EXPECT_NEAR(backend.expectation_value(std::string(n, 'Z')), 1.0, 1e-10);
    EXPECT_NEAR(backend.expectation_value(std::string(n - 1, 'I') + "Z"), 0.0, 1e-10);
}

TEST(TnQuery, Expectation_BeforePrepare_Throws) {
    // Arrange: no circuit prepared — there is no state to measure.
    TensorNetworkBackend backend;

    // Act + Assert: a policy/state error, not a stub marker.
    EXPECT_THROW(backend.expectation_value("Z"), SimulatorError);
}

TEST(TnQuery, Expectation_InvalidPauliString_Throws) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1);
    TensorNetworkBackend backend;
    run_on_tn(backend, circ);

    // Act + Assert: same CircuitError contract as the FullState backends, since
    // both go through compile_pauli_ops().
    EXPECT_THROW(backend.expectation_value("ZQ"), qplayer::CircuitError);
    EXPECT_THROW(backend.expectation_value("ZZZ"), qplayer::CircuitError);
}
