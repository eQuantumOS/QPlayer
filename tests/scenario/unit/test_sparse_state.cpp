// test_sparse_state.cpp — Unit tests for SparseStateBackend :
//   Internal representation, init_state(), get_statevector(), threshold helpers,
// and permanent error paths. Gate/measure/execute remain stubs.
// (Arrange/Act/Assert).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <complex>
#include <variant>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "../../../include/backend/sparse_state/sparse_state_backend.hpp"
#include "../../../include/backend/sparse_state/feasibility.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/error.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/types.hpp"

using qplayer::SparseStateBackend;
using qplayer::SimulatorError;
using qplayer::NotImplementedError;
using qplayer::QubitIndex;
using qplayer::StateIndex;
using qplayer::StateVector;
using qplayer::MAX_STATEVECTOR_QUBITS;
using qplayer::kSparseDefaultThreshold;

namespace {

// Fixture clears QPLAYER_SPARSE_THRESHOLD in TearDown so env override tests do
// not leak state into subsequent cases.
class SparseStateFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override {
        ::unsetenv("QPLAYER_SPARSE_THRESHOLD");
        backend = std::make_unique<SparseStateBackend>();
    }
    void TearDown() override {
        ::unsetenv("QPLAYER_SPARSE_THRESHOLD");
        ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");  // /3: defensive cleanup
        backend.reset();
    }
    std::unique_ptr<SparseStateBackend> backend;
};

}  // namespace

// ── -1: ground state after init_state ─────────────────────────────────────

TEST_F(SparseStateFixture, InitState_GroundState_SingleNonzeroAmplitude) {
    // Arrange: 3-qubit system; expected dim == 8.

    // Act: init_state sets |000>.
    backend->init_state(3);
    const StateVector sv = backend->get_statevector();

    // Assert: index 0 amplitude == (1.0, 0.0); all others == (0.0, 0.0).
    // Tolerance 1e-12: exact arithmetic (no sampling), matches kSparseDefaultThreshold
    // floor.
    EXPECT_EQ(sv.size(), std::size_t{8});
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    for (std::size_t i = 1; i < 8; ++i) {
        EXPECT_NEAR(sv[i].real(), 0.0, 1e-12);
        EXPECT_NEAR(sv[i].imag(), 0.0, 1e-12);
    }
}

// ── -2: single-qubit ground state ─────────────────────────────────────────

TEST_F(SparseStateFixture, InitState_OneQubit_GroundState) {
    // Arrange / Act.
    backend->init_state(1);
    const StateVector sv = backend->get_statevector();

    // Assert: |0> → dim == 2, amplitude[0] == (1, 0), amplitude[1] == (0, 0).
    // Tolerance 1e-12: exact value expected.
    EXPECT_EQ(sv.size(), std::size_t{2});
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-12);
}

// ── -3: re-init clears previous state ─────────────────────────────────────

TEST_F(SparseStateFixture, InitState_Reinit_ClearsPreviousState) {
    // Arrange: first init to 2 qubits.
    backend->init_state(2);

    // Act: reinit to 3 qubits — previous 2-qubit state must be discarded.
    backend->init_state(3);
    const StateVector sv = backend->get_statevector();

    // Assert: dimension is now 8, not 4; only index 0 is non-zero.
    // Tolerance 1e-12: exact arithmetic.
    EXPECT_EQ(sv.size(), std::size_t{8});
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    for (std::size_t i = 1; i < 8; ++i) {
        EXPECT_NEAR(sv[i].real(), 0.0, 1e-12);
    }
}

// ── -4: default threshold when env is absent ──────────────────────────────

TEST(SparseState, DefaultThreshold_NoEnv_Is1e12) {
    // Arrange: ensure env is unset before constructing the backend.
    ::unsetenv("QPLAYER_SPARSE_THRESHOLD");
    SparseStateBackend backend;

    // Act: init 1-qubit ground state, then verify the densified vector is exact.
    // The ground-state amplitude is exactly 1.0 so it must survive any threshold
    // <= 1.0; the densified vector at index 0 must remain (1.0, 0.0).
    backend.init_state(1);
    const StateVector sv = backend.get_statevector();

    // Assert: amplitude[0] is intact (well above threshold 1e-12);
    // amplitude[1] remains 0.0 (sparse map never created the entry).
    // Tolerance 1e-12: exact arithmetic, no sampling.
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-12);

    ::unsetenv("QPLAYER_SPARSE_THRESHOLD");
}

// ── -5: env override sets a custom threshold ──────────────────────────────

TEST(SparseState, EnvOverride_CustomThreshold_Accepted) {
    // Arrange: set a larger threshold to simulate lossy opt-in.
    ::setenv("QPLAYER_SPARSE_THRESHOLD", "0.1", 1);
    SparseStateBackend backend;

    // Act: init and check that the ground-state amplitude (1.0 >> 0.1) survives.
    backend.init_state(1);
    const StateVector sv = backend.get_statevector();

    // Assert: amplitude[0] = 1.0 is above the 0.1 threshold and must be stored.
    // Tolerance 1e-12: exact arithmetic, threshold only removes sub-threshold entries.
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);

    ::unsetenv("QPLAYER_SPARSE_THRESHOLD");
}

// ── -6: invalid env value falls back to default ───────────────────────────

TEST(SparseState, EnvOverride_InvalidValue_FallsBackToDefault) {
    // Arrange: set a non-numeric env value; the backend must warn and use 1e-12.
    ::setenv("QPLAYER_SPARSE_THRESHOLD", "not_a_number", 1);
    // Should not throw — just warn internally (budget_from_env pattern).
    SparseStateBackend backend;

    // Act: verify normal operation — ground state intact.
    backend.init_state(2);
    const StateVector sv = backend.get_statevector();

    // Assert: amplitude[0] == (1, 0); fallback threshold is the default 1e-12.
    // Tolerance 1e-12: exact arithmetic.
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);

    ::unsetenv("QPLAYER_SPARSE_THRESHOLD");
}

// ── -7: negative env value falls back to default ─────────────────────────

TEST(SparseState, EnvOverride_NegativeValue_FallsBackToDefault) {
    // Arrange: negative threshold is physically meaningless; must be rejected.
    ::setenv("QPLAYER_SPARSE_THRESHOLD", "-1e-6", 1);
    SparseStateBackend backend;

    // Act.
    backend.init_state(2);
    const StateVector sv = backend.get_statevector();

    // Assert: backend still works correctly, amplitude[0] intact.
    // Tolerance 1e-12: exact arithmetic (— negative rejected, default used).
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);

    ::unsetenv("QPLAYER_SPARSE_THRESHOLD");
}

// ── -8: get_statevector_raw always throws SimulatorError ─────────────────

TEST_F(SparseStateFixture, GetStatevectorRaw_Always_ThrowsSimulatorError) {
    // Arrange: init to any valid state.
    backend->init_state(2);

    // Act / Assert: SparseState has no SoA buffer — permanent error.
    EXPECT_THROW(backend->get_statevector_raw(), SimulatorError);
}

// Also test without calling init_state first — should still throw SimulatorError.
TEST_F(SparseStateFixture, GetStatevectorRaw_BeforeInit_ThrowsSimulatorError) {
    // Act / Assert: permanent error regardless of state.
    EXPECT_THROW(backend->get_statevector_raw(), SimulatorError);
}

// ── -9: get_statevector exceeds MAX_STATEVECTOR_QUBITS ────────────────────

TEST_F(SparseStateFixture, GetStatevector_ExceedsMaxQubits_Throws) {
    // Arrange: init to MAX_STATEVECTOR_QUBITS + 1 (currently 26 qubits).
    // Densification would require 2^26 * 16 bytes ~ 1 GiB which is disallowed.
    const QubitIndex too_many = static_cast<QubitIndex>(MAX_STATEVECTOR_QUBITS) + 1u;
    backend->init_state(too_many);

    // Act / Assert: densification guard rejects the request.
    EXPECT_THROW(backend->get_statevector(), SimulatorError);
}

// ── -10: get_statevector at exactly MAX_STATEVECTOR_QUBITS is allowed ─────

TEST_F(SparseStateFixture, GetStatevector_AtMaxQubits_Succeeds) {
    // Arrange: init to the maximum allowed qubit count.
    const QubitIndex at_max = static_cast<QubitIndex>(MAX_STATEVECTOR_QUBITS);
    backend->init_state(at_max);

    // Act: densification at the boundary must succeed.
    StateVector sv;
    EXPECT_NO_THROW(sv = backend->get_statevector());

    // Assert: dim == 2^25; index 0 amplitude is (1, 0).
    // Tolerance 1e-12: exact arithmetic.
    const std::size_t expected_dim = std::size_t{1} << MAX_STATEVECTOR_QUBITS;
    EXPECT_EQ(sv.size(), expected_dim);
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
}

// ── -11: prepare() now calls init_state (implemented) ──────────────

TEST_F(SparseStateFixture, Prepare_InitialisesGroundState) {
    // Arrange: create a 2-qubit circuit.
    qplayer::Circuit circ(2);

    // Act: prepare() must initialise the state to |00> without throwing.
    EXPECT_NO_THROW(backend->prepare(circ));

    // Assert: get_statevector reflects a valid 2-qubit ground state.
    const StateVector sv = backend->get_statevector();
    EXPECT_EQ(sv.size(), std::size_t{4});
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[3].real(), 0.0, 1e-12);
}

// ── -1: H then CX produces Bell state ────────────────────────────────────

TEST_F(SparseStateFixture, ApplyGate_HThenCX_ProducesBellState) {
    // Arrange: 2-qubit system.
    backend->init_state(2);

    // Act: H(0) then CX(0,1) produces (|00>+|11>)/sqrt(2).
    qplayer::GateOp h_op;
    h_op.type   = qplayer::GateType::H;
    h_op.qubits = {0};
    backend->apply_gate(h_op);

    qplayer::GateOp cx_op;
    cx_op.type   = qplayer::GateType::CX;
    cx_op.qubits = {0, 1};
    backend->apply_gate(cx_op);

    const StateVector sv = backend->get_statevector();

    // Assert: Bell state (|00>+|11>)/sqrt(2): amp[00]=amp[11]=1/sqrt(2).
    // Tolerance 1e-12: exact arithmetic, no sampling.
    const double INV_SQRT2 = M_SQRT1_2;
    EXPECT_NEAR(sv[0].real(), INV_SQRT2, 1e-12);
    EXPECT_NEAR(sv[0].imag(), 0.0,       1e-12);
    EXPECT_NEAR(sv[1].real(), 0.0,       1e-12);
    EXPECT_NEAR(sv[1].imag(), 0.0,       1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0,       1e-12);
    EXPECT_NEAR(sv[2].imag(), 0.0,       1e-12);
    EXPECT_NEAR(sv[3].real(), INV_SQRT2, 1e-12);
    EXPECT_NEAR(sv[3].imag(), 0.0,       1e-12);
}

// ── -2: execute Bell circuit → 50/50 distribution ───────────────────────

TEST_F(SparseStateFixture, Execute_BellCircuit_FiftyFiftyDistribution) {
    // Arrange: 2-qubit, 2-cubit circuit: H(0).CX(0,1).measure(0,0).measure(1,1).
    qplayer::Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Use a fixed seed for reproducibility.
    backend->set_seed(42);

    // Act.
    const auto result = backend->execute(circ, 10000);

    // Assert: only "00" and "11" states appear; each ~50% (Bell state).
    // Tolerance 0.02: nshots=10000 statistical tolerance.
    const auto& c = result.counts();
    const double p00 = (c.count("00") ? static_cast<double>(c.at("00")) : 0.0) / 10000.0;
    const double p11 = (c.count("11") ? static_cast<double>(c.at("11")) : 0.0) / 10000.0;
    EXPECT_NEAR(p00, 0.5, 0.02);
    EXPECT_NEAR(p11, 0.5, 0.02);
    EXPECT_TRUE(c.count("01") == 0 || c.at("01") == 0);
    EXPECT_TRUE(c.count("10") == 0 || c.at("10") == 0);
    EXPECT_EQ(result.shots(), std::uint64_t{10000});
}

// ── -3: 3-qubit GHZ → "000" and "111" each ~50% ────────────────────────

TEST_F(SparseStateFixture, Execute_GHZ3q_Correct) {
    // Arrange: 3-qubit GHZ circuit.
    qplayer::Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(0, 2).measure(0, 0).measure(1, 1).measure(2, 2);

    backend->set_seed(123);

    // Act.
    const auto result = backend->execute(circ, 10000);

    // Assert: only "000" and "111" (GHZ); each ~50%.
    // Tolerance 0.02: nshots=10000 statistical tolerance.
    const auto& c = result.counts();
    const double p000 = (c.count("000") ? static_cast<double>(c.at("000")) : 0.0) / 10000.0;
    const double p111 = (c.count("111") ? static_cast<double>(c.at("111")) : 0.0) / 10000.0;
    EXPECT_NEAR(p000, 0.5, 0.02);
    EXPECT_NEAR(p111, 0.5, 0.02);
    EXPECT_TRUE(c.count("001") == 0 || c.at("001") == 0);
    EXPECT_TRUE(c.count("010") == 0 || c.at("010") == 0);
    EXPECT_TRUE(c.count("100") == 0 || c.at("100") == 0);
    EXPECT_TRUE(c.count("110") == 0 || c.at("110") == 0);
}

// ── -4: 100-qubit sparse GHZ — two-term sparsity check ──────────────────

TEST_F(SparseStateFixture, Execute_LargeSparseGHZ_ExactTwoTerms) {
    // Arrange: 100-qubit GHZ-like circuit — H(0) + CX chain.
    // The state is (|0...0> + |1...1>)/sqrt(2) — exactly 2 non-zero amplitudes.
    constexpr QubitIndex N = 100;
    qplayer::Circuit circ(N, N);
    circ.h(0);
    for (QubitIndex q = 0; q < N - 1; ++q) {
        circ.cx(0, q + 1);
    }
    for (QubitIndex q = 0; q < N; ++q) {
        circ.measure(q, q);
    }

    backend->set_seed(999);

    // Act: execute nshots=2000; the only outcomes must be all-0 or all-1.
    const auto result = backend->execute(circ, 2000);

    // Assert: exactly two distinct bitstrings — all-0 and all-1 strings.
    // Each should appear ~50% of the time.
    // Tolerance 0.04: nshots=2000 (larger tolerance than 10k shot tests).
    const auto& c = result.counts();
    EXPECT_EQ(c.size(), std::size_t{2});

    const std::string all_zeros(N, '0');
    const std::string all_ones(N, '1');
    EXPECT_TRUE(c.count(all_zeros) > 0);
    EXPECT_TRUE(c.count(all_ones)  > 0);

    const double p0 = static_cast<double>(c.at(all_zeros)) / 2000.0;
    const double p1 = static_cast<double>(c.at(all_ones))  / 2000.0;
    EXPECT_NEAR(p0, 0.5, 0.04);
    EXPECT_NEAR(p1, 0.5, 0.04);

    // get_statevector() must not be called for 100-qubit state (densification
    // would require 2^100 entries). This test verifies sparse execution correctness
    // without attempting densification.
}

// ── -5: apply_measure collapses and correlates both qubits ───────────────

TEST_F(SparseStateFixture, ApplyMeasure_CollapsesAndRenormalizes) {
    // Arrange: build Bell state manually via apply_gate.
    backend->init_state(2);
    qplayer::GateOp h_op;
    h_op.type   = qplayer::GateType::H;
    h_op.qubits = {0};
    backend->apply_gate(h_op);
    qplayer::GateOp cx_op;
    cx_op.type   = qplayer::GateType::CX;
    cx_op.qubits = {0, 1};
    backend->apply_gate(cx_op);

    backend->set_seed(7);

    // Act: measure qubit 0; should collapse both qubits to the same value.
    const int out0 = backend->apply_measure(0);
    const StateVector sv = backend->get_statevector();

    // Assert: the post-measurement state is |00> or |11> (fully correlated).
    // Tolerance 1e-12: exact amplitude after collapse.
    if (out0 == 0) {
        // Collapsed to |00>: amplitude[0] must be 1, amplitude[3] must be 0.
        EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
        EXPECT_NEAR(sv[3].real(), 0.0, 1e-12);
    } else {
        // Collapsed to |11>: amplitude[3] must be 1, amplitude[0] must be 0.
        EXPECT_NEAR(sv[3].real(), 1.0, 1e-12);
        EXPECT_NEAR(sv[0].real(), 0.0, 1e-12);
    }
    // Either way, cross-terms must be zero (no |01> or |10>).
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0, 1e-12);
}

// ── -6: SWAP and CCX correctness ────────────────────────────────────────

TEST_F(SparseStateFixture, Swap_And_CCX_Correct) {
    // Arrange sub-test 1: SWAP |01> → |10>
    {
        backend->init_state(2);
        // Set qubit 0 to |1> via X gate (prepare |01> = index 1 in our convention).
        qplayer::GateOp x0;
        x0.type   = qplayer::GateType::X;
        x0.qubits = {0};
        backend->apply_gate(x0);

        qplayer::GateOp sw;
        sw.type   = qplayer::GateType::SWAP;
        sw.qubits = {0, 1};
        backend->apply_gate(sw);

        const StateVector sv = backend->get_statevector();
        // After SWAP: qubit 0 = 0, qubit 1 = 1 → index 2 (binary: q1q0 = 10).
        // Tolerance 1e-12: exact arithmetic.
        EXPECT_NEAR(sv[2].real(), 1.0, 1e-12);
        EXPECT_NEAR(sv[0].real(), 0.0, 1e-12);
        EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
        EXPECT_NEAR(sv[3].real(), 0.0, 1e-12);
    }

    // Arrange sub-test 2: CCX |110> → |111> (Toffoli gate)
    {
        backend->init_state(3);
        // Set qubits 1 and 2 to |1> so control bits are both 1.
        qplayer::GateOp x1, x2;
        x1.type   = qplayer::GateType::X;
        x1.qubits = {1};
        x2.type   = qplayer::GateType::X;
        x2.qubits = {2};
        backend->apply_gate(x1);
        backend->apply_gate(x2);
        // State is now |110> = index 6 (binary q2q1q0 = 110).

        qplayer::GateOp ccx;
        ccx.type   = qplayer::GateType::CCX;
        ccx.qubits = {1, 2, 0};  // control=q1,q2; target=q0
        backend->apply_gate(ccx);

        const StateVector sv = backend->get_statevector();
        // After Toffoli (controls both 1): |110> → |111> = index 7.
        // Tolerance 1e-12: exact arithmetic.
        EXPECT_NEAR(sv[7].real(), 1.0, 1e-12);
        EXPECT_NEAR(sv[6].real(), 0.0, 1e-12);
    }
}

// ── : unsupported-feature rejection paths ───────────

// SparseState does not support noise.
TEST_F(SparseStateFixture, SupportsNoise_Always_ReturnsFalse) {
    // Act + Assert: metadata advertises no noise support.
    EXPECT_FALSE(backend->supports_noise());
}

// SparseState has no GPU backend (— CUDA hash-map coalescing problem).
TEST_F(SparseStateFixture, SupportsGpu_Always_ReturnsFalse) {
    // Act + Assert.
    EXPECT_FALSE(backend->supports_gpu());
}

// A GPU request for SparseState never yields a GPU backend: the factory always
// produces the CPU sparse backend (factory.cpp — ProcType ignored for sparse).
TEST(SparseState, Factory_GpuRequest_ProducesCpuSparseBackend) {
    // Act: ask for a GPU SparseState backend explicitly.
    auto backend = qplayer::make_backend(qplayer::SimType::SparseState,
                                         qplayer::ProcType::GPU);

    // Assert: it is a SparseState backend with no GPU support.
    EXPECT_EQ(static_cast<int>(backend->sim_type()),
              static_cast<int>(qplayer::SimType::SparseState));
    EXPECT_FALSE(backend->supports_gpu());
}

// execute() rejects any non-null NoiseModel with SimulatorError.
TEST_F(SparseStateFixture, Execute_WithNoiseModel_ThrowsSimulatorError) {
    // Arrange: a trivial circuit plus a default noise model.
    qplayer::Circuit circ(1, 1);
    circ.h(0).measure(0, 0);
    qplayer::NoiseModel noise;

    // Act + Assert: passing a noise model is rejected immediately.
    EXPECT_THROW(backend->execute(circ, 100, &noise), SimulatorError);
}

// A circuit carrying an IdleOp is rejected: SparseState skips the
// IdleNoiseInserterPass and throws on IdleOp dispatch (arch).
// IdleOp cannot be added via the public builder, so it is injected through the
// detail bridge used by the parser / passes.
TEST_F(SparseStateFixture, Execute_CircuitWithIdleOp_ThrowsSimulatorError) {
    // Arrange: build a 1-qubit circuit and splice an IdleOp before measurement.
    qplayer::Circuit circ(1, 1);
    circ.h(0);
    qplayer::detail_circuit_push_op(circ, qplayer::IdleOp{/*qubit=*/0,
                                                          /*duration_ns=*/100.0});
    circ.measure(0, 0);

    // Act + Assert: IdleOp dispatch raises SimulatorError (noise unsupported).
    EXPECT_THROW(backend->execute(circ, 100), SimulatorError);
}

// ── : 128-qubit structural cap ─────────────────────────────

// n=128 is the largest safe qubit count (gate shifts reach 127 < 128). It must
// be accepted, not rejected. get_statevector() is NOT called (would need 2^128).
TEST_F(SparseStateFixture, InitState_128Qubits_NoThrow) {
    EXPECT_NO_THROW(backend->init_state(128));
}

// n=129 would shift StateIndex (__uint128_t) by 128 = undefined behaviour, so it
// is rejected at the structural cap before any allocation.
TEST_F(SparseStateFixture, InitState_129Qubits_ThrowsSimulatorError) {
    EXPECT_THROW(backend->init_state(129), SimulatorError);
}

// A rejected init_state must leave the previous state intact: the guard runs
// before amplitudes_.clear().
TEST_F(SparseStateFixture, InitState_OverCapAfterValid_PreservesPriorState) {
    // Arrange: a valid 2-qubit ground state.
    backend->init_state(2);

    // Act: an over-cap request throws.
    EXPECT_THROW(backend->init_state(129), SimulatorError);

    // Assert: the prior 2-qubit state is still queryable and intact.
    const StateVector sv = backend->get_statevector();
    EXPECT_EQ(sv.size(), std::size_t{4});
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);
}

// prepare() funnels through init_state(), so a 129-qubit circuit is rejected.
TEST_F(SparseStateFixture, Prepare_129QubitCircuit_ThrowsSimulatorError) {
    // Arrange: a 129-qubit circuit is valid IR; only SparseState rejects it.
    qplayer::Circuit circ(129, 1);

    EXPECT_THROW(backend->prepare(circ), SimulatorError);
}

// max_qubits() reports the structural 128 ceiling, independent of WorkingSet and
// of the densify cap MAX_STATEVECTOR_QUBITS=25.
TEST_F(SparseStateFixture, MaxQubits_ReportsStructural128) {
    EXPECT_EQ(backend->max_qubits(qplayer::detail::WorkingSet::Execute),
              std::uint32_t{128});
    EXPECT_EQ(backend->max_qubits(qplayer::detail::WorkingSet::Materialize),
              std::uint32_t{128});
}

// ── : pre-flight feasibility analysis ────────────────────

// GHZ = H(0) + CX chain. H branches S 1->2; the CX chain is a permutation (index
// relabel) so S stays exactly 2. The direct-count estimator must NOT over-count
// to 2^n (the popcount first draft did — rejected as alternative E1).
TEST_F(SparseStateFixture, Feasibility_GHZ_EstimatesTwoStates) {
    qplayer::Circuit circ(8, 8);
    circ.h(0);
    for (qplayer::QubitIndex q = 0; q < 7; ++q) {
        circ.cx(0, q + 1);
    }

    const auto f = qplayer::detail::analyze_sparse_feasibility(circ, UINT64_MAX);

    EXPECT_EQ(static_cast<std::uint64_t>(f.estimated_max_states), std::uint64_t{2});
    EXPECT_TRUE(f.feasible);
}

// k independent H gates branch each basis state once: S = 2^k. No permutation
// reabsorbs the growth here.
TEST_F(SparseStateFixture, Feasibility_AllH_EstimatesTwoPowN) {
    qplayer::Circuit circ(5, 5);
    for (qplayer::QubitIndex q = 0; q < 5; ++q) {
        circ.h(q);
    }

    const auto f = qplayer::detail::analyze_sparse_feasibility(circ, UINT64_MAX);

    EXPECT_EQ(static_cast<std::uint64_t>(f.estimated_max_states),
              std::uint64_t{32});  // 2^5
}

// Diagonal (Z, S, T, RZ) and permutation (X, CX) gates after one branch leave S
// unchanged at 2 — only off-diagonal-coupling gates branch.
TEST_F(SparseStateFixture, Feasibility_DiagonalAndPermutation_PreserveCount) {
    qplayer::Circuit circ(3, 3);
    circ.h(0).z(0).s(0).t(0).x(1).cx(0, 2).rz(0.7, 0);

    const auto f = qplayer::detail::analyze_sparse_feasibility(circ, UINT64_MAX);

    EXPECT_EQ(static_cast<std::uint64_t>(f.estimated_max_states), std::uint64_t{2});
}

// RY(pi) is a pure permutation (zero diagonal, non-zero off-diagonal) and must
// not branch; RY(pi/2) couples diagonal and off-diagonal and must branch. The
// estimator inspects the 2x2 matrix, so it gets the angle special cases right.
TEST_F(SparseStateFixture, Feasibility_RyPi_PreservesRyHalfPiBranches) {
    qplayer::Circuit perm(2, 2);
    perm.ry(M_PI, 0).ry(M_PI, 1);
    const auto fp = qplayer::detail::analyze_sparse_feasibility(perm, UINT64_MAX);
    EXPECT_EQ(static_cast<std::uint64_t>(fp.estimated_max_states), std::uint64_t{1});

    qplayer::Circuit branch(2, 2);
    branch.ry(M_PI / 2, 0).ry(M_PI / 2, 1);
    const auto fb = qplayer::detail::analyze_sparse_feasibility(branch, UINT64_MAX);
    EXPECT_EQ(static_cast<std::uint64_t>(fb.estimated_max_states), std::uint64_t{4});
}

// A high-branch circuit under a tiny budget is reported infeasible; the
// estimated byte count exceeds the budget.
TEST_F(SparseStateFixture, Feasibility_HighBranchTinyBudget_Infeasible) {
    qplayer::Circuit circ(20, 20);
    for (qplayer::QubitIndex q = 0; q < 20; ++q) {
        circ.h(q);  // S = 2^20, * 168 B ~= 176 MiB
    }

    const auto f =
        qplayer::detail::analyze_sparse_feasibility(circ, std::uint64_t{1024});

    EXPECT_FALSE(f.feasible);
    EXPECT_TRUE(f.estimated_bytes > f.budget_bytes);
}

// prepare() rejects an infeasible circuit before evolving when the pre-flight
// check is enabled (opt-in) and the budget is constrained via
// QPLAYER_MEMORY_BUDGET_GB (+).
TEST_F(SparseStateFixture, Prepare_InsufficientMemory_ThrowsSimulatorError) {
    backend->set_feasibility_check(true);  // is opt-in
    // ~1 KiB budget: a 20-qubit all-H circuit (S=2^20) cannot fit.
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.000001", 1);
    qplayer::Circuit circ(20, 20);
    for (qplayer::QubitIndex q = 0; q < 20; ++q) {
        circ.h(q);
    }

    EXPECT_THROW(backend->prepare(circ), SimulatorError);

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// With enabled, the same tiny budget still admits a low-entanglement GHZ
// circuit: S=2 needs only ~336 B, so prepare() must NOT reject it (no
// false-reject of sparse-friendly circuits).
TEST_F(SparseStateFixture, Prepare_GHZUnderTinyBudget_Succeeds) {
    backend->set_feasibility_check(true);  // is opt-in
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.000001", 1);  // ~1 KiB
    qplayer::Circuit circ(40, 40);
    circ.h(0);
    for (qplayer::QubitIndex q = 0; q < 39; ++q) {
        circ.cx(0, q + 1);
    }

    EXPECT_NO_THROW(backend->prepare(circ));

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// : pre-flight feasibility is OFF by default. The same infeasible
// circuit + tiny budget that throws when is enabled must NOT throw at
// prepare() by default — evolution proceeds, leaving overruns to the runtime
// guard / bad_alloc backstop. prepare() only inits + (skipped) check, so it
// completes without the predicted-count rejection.
TEST_F(SparseStateFixture, Prepare_FeasibilityOffByDefault_NoRejection) {
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.000001", 1);  // ~1 KiB
    qplayer::Circuit circ(20, 20);
    for (qplayer::QubitIndex q = 0; q < 20; ++q) {
        circ.h(q);  // (if on) would reject: S=2^20 ≫ 1 KiB
    }

    // Default backend (no set_feasibility_check) -> prepare must not reject.
    EXPECT_NO_THROW(backend->prepare(circ));

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// The toggle is reversible: enable then disable returns to the default
// no-rejection behaviour (set_feasibility_check parity with env).
TEST_F(SparseStateFixture, Prepare_FeasibilityToggleOffAgain_NoRejection) {
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.000001", 1);
    qplayer::Circuit circ(20, 20);
    for (qplayer::QubitIndex q = 0; q < 20; ++q) circ.h(q);

    backend->set_feasibility_check(true);
    EXPECT_THROW(backend->prepare(circ), SimulatorError);

    backend->set_feasibility_check(false);
    EXPECT_NO_THROW(backend->prepare(circ));

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// ── /4: runtime physical-memory guard ────────────────────

namespace {
qplayer::GateOp gate1q(qplayer::GateType t, qplayer::QubitIndex q) {
    qplayer::GateOp g;
    g.type   = t;
    g.qubits = {q};
    return g;
}
}  // namespace

// The runtime guard catches growth on the direct apply_gate() path (Interactive),
// which bypasses the prepare() pre-flight check. With a ~1 KiB budget,
// branching to 8 non-zero states (8 * 168 B = 1344 B) exceeds it; the periodic
// check (every kMemCheckGateInterval gates) then throws once it fires. Diagonal
// pad gates advance the counter to the interval while keeping the map small, so
// the test never allocates real memory.
TEST_F(SparseStateFixture, ApplyGate_RuntimeMemoryExceeded_ThrowsSimulatorError) {
    using qplayer::GateType;
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.000001", 1);  // ~1 KiB budget
    backend->init_state(10);  // captures budget_bytes_

    // Grow to 8 states, then pad with diagonal Z gates (S unchanged) so the
    // counter reaches kMemCheckGateInterval while the map holds 8 entries.
    backend->apply_gate(gate1q(GateType::H, 0));
    backend->apply_gate(gate1q(GateType::H, 1));
    backend->apply_gate(gate1q(GateType::H, 2));

    bool threw = false;
    try {
        for (int i = 0; i < static_cast<int>(qplayer::kMemCheckGateInterval) + 8; ++i) {
            backend->apply_gate(gate1q(GateType::Z, 0));
        }
    } catch (const SimulatorError&) {
        threw = true;
    }
    EXPECT_TRUE(threw);

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// Under an ample (default) budget, a long all-diagonal sequence over a tiny map
// must NOT trip the guard: the check is size-based, not gate-count-based.
TEST_F(SparseStateFixture, ApplyGate_LongCircuitAmpleBudget_NoFalseTrip) {
    using qplayer::GateType;
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");  // use real /proc budget (GiBs)
    backend->init_state(4);
    backend->apply_gate(gate1q(GateType::H, 0));  // S = 2, well within budget

    EXPECT_NO_THROW([&] {
        for (int i = 0; i < static_cast<int>(qplayer::kMemCheckGateInterval) * 3; ++i) {
            backend->apply_gate(gate1q(GateType::Z, 0));
        }
    }());
}

// cat state == GHZ (H + CX chain): exactly two non-zero amplitudes. The
// estimate must agree with the runtime accounting — both charge 168 B/state,
// so estimated_bytes == 2 * kSparseEntryBytes (consistency anchor).
TEST_F(SparseStateFixture, RuntimeGuard_CatStateEstimateMatchesGHZ) {
    qplayer::Circuit circ(30, 30);
    circ.h(0);
    for (qplayer::QubitIndex q = 0; q < 29; ++q) {
        circ.cx(0, q + 1);
    }

    const auto f = qplayer::detail::analyze_sparse_feasibility(circ, UINT64_MAX);
    EXPECT_EQ(static_cast<std::uint64_t>(f.estimated_max_states), std::uint64_t{2});
    EXPECT_EQ(f.estimated_bytes,
              std::uint64_t{2} * qplayer::detail::kSparseEntryBytes);
}

// Regression: a 100-qubit sparse GHZ (Bell-like, S=2) still executes correctly
// under the default budget with both guards active — neither nor blocks a
// genuinely sparse large circuit.
TEST_F(SparseStateFixture, Execute_LargeGHZ_GuardsDoNotBlockSparseCircuit) {
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
    constexpr QubitIndex N = 100;
    qplayer::Circuit circ(N, N);
    circ.h(0);
    for (QubitIndex q = 0; q < N - 1; ++q) {
        circ.cx(0, q + 1);
    }
    for (QubitIndex q = 0; q < N; ++q) {
        circ.measure(q, q);
    }
    backend->set_seed(7);

    EXPECT_NO_THROW(backend->execute(circ, 1000));
}

// ── T7H: partitioned parallel store ──────────────────────────────

namespace {
// Builds a moderately entangled 12-qubit state exercising both partition-local
// gates (low-qubit targets) and partition-qubit-target gates (high-qubit targets
// -> serial fallback): H on every qubit, a CX chain, and rotations.
qplayer::Circuit build_mixed_12q() {
    qplayer::Circuit c(12, 12);
    for (qplayer::QubitIndex q = 0; q < 12; ++q) c.h(q);
    for (qplayer::QubitIndex q = 0; q < 11; ++q) c.cx(q, q + 1);
    c.rz(0.3, 0).ry(0.7, 5).t(11).cx(11, 0);  // q11 is a high (partition) qubit
    return c;
}
}  // namespace

// Bit-exact thread-count invariance: the same circuit run
// with 1 vs 4 OpenMP threads over the fixed P-shard store must yield byte-identical
// amplitudes. Each shard transforms independently and every output is a fixed
// 2-term form, so IEEE commutativity makes the result thread-count-independent.
TEST_F(SparseStateFixture, ThreadCountInvariant_StatevectorBitExact) {
    const qplayer::Circuit c = build_mixed_12q();

#ifdef _OPENMP
    omp_set_num_threads(1);
#endif
    backend->prepare(c);
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            backend->apply_gate(std::get<qplayer::GateOp>(op));
        }
    }
    const StateVector sv1 = backend->get_statevector();

#ifdef _OPENMP
    omp_set_num_threads(4);
#endif
    backend->prepare(c);
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            backend->apply_gate(std::get<qplayer::GateOp>(op));
        }
    }
    const StateVector sv4 = backend->get_statevector();

    EXPECT_EQ(sv1.size(), sv4.size());
    const std::size_t n = std::min(sv1.size(), sv4.size());
    for (std::size_t i = 0; i < n; ++i) {
        // Tolerance 0.0 = bit-exact: |a-b| <= 0 holds only when a == b exactly.
        // The parallel path must reproduce the serial values exactly,
        // not merely closely.
        EXPECT_NEAR(sv1[i].real(), sv4[i].real(), 0.0);
        EXPECT_NEAR(sv1[i].imag(), sv4[i].imag(), 0.0);
    }
}

// Both gate paths (partition-local CX on low qubits + serial-fallback CX whose
// target is a high partition qubit) produce a correct 12-qubit GHZ state:
// (|0...0> + |1...1>)/sqrt(2), exactly two non-zero amplitudes at index 0 and
// 2^12-1. Exercises the serial fallback because the CX chain targets every qubit
// including the top partition qubits.
TEST_F(SparseStateFixture, GhzAcrossPartitions_TwoAmplitudesCorrect) {
    qplayer::Circuit c(12, 12);
    c.h(0);
    for (qplayer::QubitIndex q = 0; q < 11; ++q) c.cx(0, q + 1);

    backend->prepare(c);
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            backend->apply_gate(std::get<qplayer::GateOp>(op));
        }
    }

    const StateVector sv = backend->get_statevector();
    const std::size_t last = (std::size_t{1} << 12) - 1;
    const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
    // Exactly |0..0> and |1..1>, each amplitude 1/sqrt(2); everything else zero.
    EXPECT_NEAR(sv[0].real(), inv_sqrt2, 1e-12);
    EXPECT_NEAR(sv[last].real(), inv_sqrt2, 1e-12);
    std::size_t nonzero = 0;
    for (const auto& a : sv) if (std::abs(a) > 1e-12) ++nonzero;
    EXPECT_EQ(nonzero, std::size_t{2});
}

// Fixed seed reproducibility through execute(): two runs of the same
// circuit with the same seed produce identical counts, even though sampling now
// draws from the partitioned store via the sorted canonical order.
TEST_F(SparseStateFixture, FixedSeed_ExecuteCountsReproducible) {
    qplayer::Circuit circ(12, 12);
    for (qplayer::QubitIndex q = 0; q < 12; ++q) circ.h(q);
    for (qplayer::QubitIndex q = 0; q < 11; ++q) circ.cx(q, q + 1);
    circ.rz(0.3, 0).ry(0.7, 5).t(11).cx(11, 0);
    for (qplayer::QubitIndex q = 0; q < 12; ++q) circ.measure(q, q);

    backend->set_seed(424242);
    const auto counts_a = backend->execute(circ, 2000).counts();
    backend->set_seed(424242);
    const auto counts_b = backend->execute(circ, 2000).counts();

    EXPECT_TRUE(counts_a == counts_b);
}

// ── : entry-level parallel parity tests ───────────────────────────────
//
// Bit-exact parity across QPLAYER_SPARSE_PARTITIONS={1,2,8,64} and
// OMP_NUM_THREADS variations. The snapshot + thread-local bucket merge yields
// disjoint inserts, so every output is produced exactly once with the same
// fixed 2-term arithmetic → statevector is independent of P and thread count.

namespace {

// Runs a circuit through SparseStateBackend and returns the densified statevector.
// Uses a freshly constructed backend (no state leakage from fixture).
StateVector run_sv(const qplayer::Circuit& circ) {
    SparseStateBackend be;
    be.prepare(circ);
    for (const auto& op : circ.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            be.apply_gate(std::get<qplayer::GateOp>(op));
        }
    }
    return be.get_statevector();
}

// Build a 10-qubit circuit that exercises:
// (a) partition-qubit-target gates (H/CX on high qubits) — old serial path.
// (b) pass-through paths (control not satisfied on diagonal gates).
// (c) SWAP across potentially a partition qubit.
// (d) enough entanglement for a non-trivial statevector.
qplayer::Circuit build_partition_parity_circuit() {
    qplayer::Circuit c(10);
    // H on all qubits: creates superposition, branches from 1 to 1024 states.
    for (qplayer::QubitIndex q = 0; q < 10; ++q) c.h(q);
    // CX chain including high qubits (old serial fallback path):
    for (qplayer::QubitIndex q = 0; q < 9; ++q) c.cx(q, q + 1);
    // Gate on the highest qubit (definite partition qubit for any P>1):
    c.rz(0.5, 9);
    c.t(8);
    // SWAP involving high qubit:
    c.swap(8, 9);
    // Additional rotations for amplitude diversity:
    c.ry(0.3, 0).rz(1.2, 4).cx(9, 0);
    return c;
}

// Build a 8-qubit GHZ circuit: H(0) + CX chain. The CX chain includes
// targets q1..q7 which at P=8 includes partition qubits. Only two amplitudes
// survive. Exercises controlled-1Q across all partition boundaries.
qplayer::Circuit build_ghz_partition_circuit() {
    qplayer::Circuit c(8);
    c.h(0);
    for (qplayer::QubitIndex q = 0; q < 7; ++q) c.cx(0, q + 1);
    return c;
}

// Checks two statevectors are bit-exact.
// Tolerance 0.0: identical IEEE double representation required.
void expect_sv_bitexact(const StateVector& ref, const StateVector& got,
                        const char* label) {
    (void)label;  // used only in failure context
    if (ref.size() != got.size()) {
        FAIL("statevector size mismatch");
        return;
    }
    for (std::size_t i = 0; i < ref.size(); ++i) {
        // EXPECT_NEAR with tol=0.0 checks exact equality for doubles.
        EXPECT_NEAR(ref[i].real(), got[i].real(), 0.0);
        EXPECT_NEAR(ref[i].imag(), got[i].imag(), 0.0);
    }
}

}  // namespace

// Parity across QPLAYER_SPARSE_PARTITIONS = 1 (serial reference) vs 2, 8, 64.
// Circuit has high-qubit targets → exercises the unified entry-level path.
TEST(SparseStateParity, PartitionCount_BitExactAcrossP) {
    const qplayer::Circuit circ = build_partition_parity_circuit();

    // Reference: P=1 (single shard, effectively serial within OMP).
    ::setenv("QPLAYER_SPARSE_PARTITIONS", "1", 1);
    const StateVector sv1 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    // P=2.
    ::setenv("QPLAYER_SPARSE_PARTITIONS", "2", 1);
    const StateVector sv2 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");
    expect_sv_bitexact(sv1, sv2, "P=2 vs P=1");

    // P=8.
    ::setenv("QPLAYER_SPARSE_PARTITIONS", "8", 1);
    const StateVector sv8 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");
    expect_sv_bitexact(sv1, sv8, "P=8 vs P=1");

    // P=64.
    ::setenv("QPLAYER_SPARSE_PARTITIONS", "64", 1);
    const StateVector sv64 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");
    expect_sv_bitexact(sv1, sv64, "P=64 vs P=1");
}

// GHZ 8-qubit: CX chain targets hit all partition boundaries.
// Verify exactly two non-zero amplitudes, each 1/sqrt(2), for every P.
TEST(SparseStateParity, GHZ_PartitionQubitTargets_BitExactAcrossP) {
    const qplayer::Circuit circ = build_ghz_partition_circuit();

    ::setenv("QPLAYER_SPARSE_PARTITIONS", "1", 1);
    const StateVector sv1 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
    // Verify sv1 has exactly two non-zero entries (|000...0> and |111...1>).
    // Tolerance 1e-12: exact arithmetic (no sampling), testing-rules.
    const std::size_t last = (std::size_t{1} << 8) - 1;
    EXPECT_NEAR(sv1[0].real(), inv_sqrt2, 1e-12);
    EXPECT_NEAR(sv1[last].real(), inv_sqrt2, 1e-12);

    for (const char* pval : {"2", "8", "64"}) {
        ::setenv("QPLAYER_SPARSE_PARTITIONS", pval, 1);
        const StateVector svP = run_sv(circ);
        ::unsetenv("QPLAYER_SPARSE_PARTITIONS");
        expect_sv_bitexact(sv1, svP, pval);
    }
}

// Thread-count parity: same circuit run with OMP_NUM_THREADS=1 vs 4.
// Confirms entry-level parallel produces same amplitudes as single-threaded.
// Tolerance 0.0: bit-exact required.
TEST(SparseStateParity, ThreadCount_BitExactWith1And4) {
    const qplayer::Circuit circ = build_partition_parity_circuit();

#ifdef _OPENMP
    omp_set_num_threads(1);
#endif
    const StateVector sv1 = run_sv(circ);

#ifdef _OPENMP
    omp_set_num_threads(4);
#endif
    const StateVector sv4 = run_sv(circ);

#ifdef _OPENMP
    // Restore: let subsequent tests use max threads.
    omp_set_num_threads(omp_get_max_threads());
#endif

    expect_sv_bitexact(sv1, sv4, "threads=4 vs threads=1");
}

// SWAP across partition qubit: P=1 vs P=8 parity.
// Exercises apply_entry_gate_swap with high-qubit operands (old serial path).
TEST(SparseStateParity, Swap_HighQubit_BitExactAcrossP) {
    // Build a circuit where H then SWAP involves the top qubit.
    qplayer::Circuit circ(8);
    for (qplayer::QubitIndex q = 0; q < 8; ++q) circ.h(q);
    circ.swap(6, 7);   // both are partition qubits for P >= 4
    circ.swap(0, 7);   // straddles all partitions
    circ.cx(7, 0);

    ::setenv("QPLAYER_SPARSE_PARTITIONS", "1", 1);
    const StateVector sv1 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    ::setenv("QPLAYER_SPARSE_PARTITIONS", "8", 1);
    const StateVector sv8 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    expect_sv_bitexact(sv1, sv8, "swap high qubit P=8 vs P=1");
}

// CSwap (Fredkin) parity: P=1 vs P=8.
TEST(SparseStateParity, CSwap_HighQubit_BitExactAcrossP) {
    qplayer::Circuit circ(8);
    circ.h(0).h(6).h(7);  // superposition on ctrl and swap qubits
    // cswap: control=0, swap q6 and q7 (both potentially partition qubits).
    qplayer::GateOp cswap_op;
    cswap_op.type   = qplayer::GateType::CSwap;
    cswap_op.qubits = {0, 6, 7};

    auto run_cswap = [&]() -> StateVector {
        SparseStateBackend be;
        be.prepare(circ);
        for (const auto& op : circ.ops()) {
            if (std::holds_alternative<qplayer::GateOp>(op)) {
                be.apply_gate(std::get<qplayer::GateOp>(op));
            }
        }
        be.apply_gate(cswap_op);
        return be.get_statevector();
    };

    ::setenv("QPLAYER_SPARSE_PARTITIONS", "1", 1);
    const StateVector sv1 = run_cswap();
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    ::setenv("QPLAYER_SPARSE_PARTITIONS", "8", 1);
    const StateVector sv8 = run_cswap();
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    expect_sv_bitexact(sv1, sv8, "cswap high qubit P=8 vs P=1");
}

// Skew scenario: state is concentrated in one top-bit region.
// The GHZ circuit has only 2 non-zero states; with partition key on top bits,
// each may land in a different shard. Verify parity still holds for P=64.
TEST(SparseStateParity, SkewedAmplitudes_BitExactAcrossP) {
    // 12-qubit GHZ: (|0...0> + |1...1>)/sqrt(2). With P=64, states 0 and
    // 2^12-1=4095 are in shards 0 and 63 respectively (extreme skew).
    qplayer::Circuit circ(12);
    circ.h(0);
    for (qplayer::QubitIndex q = 0; q < 11; ++q) circ.cx(0, q + 1);

    ::setenv("QPLAYER_SPARSE_PARTITIONS", "1", 1);
    const StateVector sv1 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    ::setenv("QPLAYER_SPARSE_PARTITIONS", "64", 1);
    const StateVector sv64 = run_sv(circ);
    ::unsetenv("QPLAYER_SPARSE_PARTITIONS");

    expect_sv_bitexact(sv1, sv64, "skewed GHZ P=64 vs P=1");
}
