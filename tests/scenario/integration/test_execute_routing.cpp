// test_execute_routing.cpp — Integration tests for FullStateCpuBackend::execute()
// fast path (no classical control) and per-shot path (IfOp
// present), apply_measure collapse, apply_reset, determinism via set_seed.
// Also covers bitstring MSB-first convention and Σcounts == nshots invariant.
// (— Gate)

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <complex>
#include <map>
#include <string>

#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/result.hpp"
#include "../../../include/types.hpp"

using qplayer::FullStateCpuBackend;
using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::MeasureOp;
using qplayer::NotImplementedError;
using qplayer::QubitIndex;
using qplayer::ClassicalBitIndex;
using qplayer::SimulatorError;
using qplayer::StateVector;
using qplayer::Result;

namespace {

// Fixture: fresh backend per test, clears QPLAYER_MEMORY_BUDGET_GB env.
class ExecuteFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override {
        ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
        backend = std::make_unique<FullStateCpuBackend>();
    }
    void TearDown() override {
        ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
        backend.reset();
    }
    std::unique_ptr<FullStateCpuBackend> backend;
};

// Sum all counts values; must equal nshots.
static std::uint64_t sum_counts(const std::map<std::string, std::uint64_t>& c) {
    std::uint64_t total = 0;
    for (const auto& kv : c) total += kv.second;
    return total;
}

}  // namespace

// ── fast path: Bell state — H(0); CX(0,1); measure(0,0); measure(1,1) ──

TEST_F(ExecuteFixture, Bell_FastPath_Distribution_00and11) {
    // Arrange: 2-qubit Bell circuit, no classical control.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // has_classical_control must be false for the fast path.
    EXPECT_FALSE(circ.has_classical_control());

    // Act: run 10000 shots with a fixed seed for mild stability.
    backend->set_seed(42);
    const Result result = backend->execute(circ, 10000);

    // Assert: distribution over "00" and "11", approximately 50/50.
    // Statistical tolerance ±0.03 for nshots=10000.
    const auto& counts = result.counts();
    const double p00 = static_cast<double>(counts.count("00") ? counts.at("00") : 0) / 10000.0;
    const double p11 = static_cast<double>(counts.count("11") ? counts.at("11") : 0) / 10000.0;
    const double p01 = static_cast<double>(counts.count("01") ? counts.at("01") : 0) / 10000.0;
    const double p10 = static_cast<double>(counts.count("10") ? counts.at("10") : 0) / 10000.0;

    // Theoretical: p00 = p11 = 0.5; p01 = p10 = 0.
    // Tolerance 0.03: statistical error for nshots=10000 (3-sigma ~ 0.015).
    EXPECT_NEAR(p00, 0.5, 0.03);
    EXPECT_NEAR(p11, 0.5, 0.03);
    EXPECT_NEAR(p01, 0.0, 0.01);  // should be exactly 0
    EXPECT_NEAR(p10, 0.0, 0.01);

    // Σcounts == nshots invariant.
    EXPECT_EQ(sum_counts(counts), std::uint64_t{10000});
    EXPECT_EQ(result.shots(), std::uint64_t{10000});
}

// ── fast path: GHZ — H(0); CX(0,1); CX(1,2); measure_all ──────────────

TEST_F(ExecuteFixture, GHZ_FastPath_Distribution_000and111) {
    // Arrange: 3-qubit GHZ circuit.
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).measure_all();

    EXPECT_FALSE(circ.has_classical_control());

    // Act
    backend->set_seed(123);
    const Result result = backend->execute(circ, 10000);

    // Assert: only "000" and "111" with ~50% each.
    const auto& counts = result.counts();
    const double p000 = static_cast<double>(counts.count("000") ? counts.at("000") : 0) / 10000.0;
    const double p111 = static_cast<double>(counts.count("111") ? counts.at("111") : 0) / 10000.0;

    // Tolerance 0.03: statistical (nshots=10000).
    EXPECT_NEAR(p000, 0.5, 0.03);
    EXPECT_NEAR(p111, 0.5, 0.03);
    EXPECT_EQ(sum_counts(counts), std::uint64_t{10000});
}

// ── per-shot path: conditional circuit ─────────────────────────────────
// Circuit: H(0); measure(0->c[0]); if(c==1) X(1); measure(1->c[1]).
// Expected: c0 and c1 are perfectly correlated — "00" ≈ 0.5, "11" ≈ 0.5.

TEST_F(ExecuteFixture, Conditional_PerShotPath_CorrelatedBits) {
    // Arrange: 2-qubit, 2-cubit circuit with IfOp.
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    // if classical register 0 (creg index 0) == 1, apply X to qubit 1.
    GateOp x_gate;
    x_gate.type   = GateType::X;
    x_gate.qubits = {1};
    circ.if_then(0, 1, x_gate);
    circ.measure(1, 1);

    // The circuit has classical control (mid-circuit measurement + IfOp).
    EXPECT_TRUE(circ.has_classical_control());

    // Act: 10000 shots.
    backend->set_seed(7);
    const Result result = backend->execute(circ, 10000);

    // Assert: c0 == c1 in all shots.
    // Bitstring convention: MSB-first. total_cubits=2; cubit[1] is MSB char, cubit[0] is LSB.
    // "00" means cubit[1]=0, cubit[0]=0 -> both 0.
    // "11" means cubit[1]=1, cubit[0]=1 -> both 1.
    // "01" and "10" must be 0.
    const auto& counts = result.counts();
    const double p00 = static_cast<double>(counts.count("00") ? counts.at("00") : 0) / 10000.0;
    const double p11 = static_cast<double>(counts.count("11") ? counts.at("11") : 0) / 10000.0;
    const double p01 = static_cast<double>(counts.count("01") ? counts.at("01") : 0) / 10000.0;
    const double p10 = static_cast<double>(counts.count("10") ? counts.at("10") : 0) / 10000.0;

    // Tolerance 0.03 statistical; 0 outcomes must be exactly 0.
    EXPECT_NEAR(p00, 0.5, 0.03);
    EXPECT_NEAR(p11, 0.5, 0.03);
    EXPECT_NEAR(p01, 0.0, 0.005);
    EXPECT_NEAR(p10, 0.0, 0.005);

    EXPECT_EQ(sum_counts(counts), std::uint64_t{10000});
    EXPECT_EQ(result.shots(), std::uint64_t{10000});
}

// ── Measurement collapse: single |+⟩ qubit collapses to a basis state ─────────

TEST_F(ExecuteFixture, ApplyMeasure_PlusState_CollapsesToBasis) {
    // Arrange: |+⟩ = H|0⟩.
    backend->init_state(1);
    GateOp h;
    h.type   = GateType::H;
    h.qubits = {0};
    backend->apply_gate(h);

    // Act: measure qubit 0 with a fixed seed.
    backend->set_seed(99);
    const int outcome = backend->apply_measure(0);

    // Assert: outcome is 0 or 1.
    EXPECT_TRUE(outcome == 0 || outcome == 1);

    // After collapse the statevector has exactly one non-zero amplitude.
    const StateVector sv = backend->get_statevector();
    EXPECT_EQ(sv.size(), std::size_t{2});

    const double a0 = std::abs(sv[0]);
    const double a1 = std::abs(sv[1]);
    // One amplitude is 1, the other is 0 (collapsed state is a basis vector).
    // Tolerance 1e-10: algebraic renormalisation, not statistical.
    if (outcome == 0) {
        EXPECT_NEAR(a0, 1.0, 1e-10);
        EXPECT_NEAR(a1, 0.0, 1e-10);
    } else {
        EXPECT_NEAR(a0, 0.0, 1e-10);
        EXPECT_NEAR(a1, 1.0, 1e-10);
    }
}

// ── apply_reset: |1⟩ after reset becomes |0⟩ ─────────────────────────────────

TEST_F(ExecuteFixture, ApplyReset_ExcitedState_BecomeGround) {
    // Arrange: |1⟩ via X on |0⟩.
    backend->init_state(1);
    GateOp x;
    x.type   = GateType::X;
    x.qubits = {0};
    backend->apply_gate(x);

    // Verify we are in |1⟩.
    {
        const StateVector sv = backend->get_statevector();
        EXPECT_NEAR(sv[0].real(), 0.0, 1e-12);
        EXPECT_NEAR(sv[1].real(), 1.0, 1e-12);
    }

    // Act: reset qubit 0.
    backend->set_seed(11);
    backend->apply_reset(0);

    // Assert: back in |0⟩.
    const StateVector sv = backend->get_statevector();
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-10);
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-10);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-10);
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-10);
}

// ── apply_reset on |0⟩ leaves |0⟩ unchanged ──────────────────────────────────

TEST_F(ExecuteFixture, ApplyReset_GroundState_RemainsGround) {
    backend->init_state(1);
    backend->set_seed(55);
    backend->apply_reset(0);

    const StateVector sv = backend->get_statevector();
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-10);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-10);
}

// ── Determinism: two execute() runs with same seed give identical counts ──────

TEST_F(ExecuteFixture, Execute_SameSeed_GivesDeterministicCounts) {
    // fast path: Bell circuit.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // First run.
    backend->set_seed(42);
    const Result r1 = backend->execute(circ, 1000);

    // Second run with the same seed.
    backend->set_seed(42);
    const Result r2 = backend->execute(circ, 1000);

    // Assert: counts maps are identical. Compare entry by entry to avoid
    // EXPECT_EQ on std::map<> which lacks operator<< for the error formatter.
    EXPECT_EQ(r1.shots(), r2.shots());
    EXPECT_EQ(r1.counts().size(), r2.counts().size());
    for (const auto& kv : r1.counts()) {
        const auto it = r2.counts().find(kv.first);
        EXPECT_TRUE(it != r2.counts().end());
        if (it != r2.counts().end()) {
            EXPECT_EQ(kv.second, it->second);
        }
    }
}

// ── shots() accessor matches nshots ──────────────────────────────────────────

TEST_F(ExecuteFixture, Execute_ShotsAccessor_MatchesNshots) {
    Circuit circ(1, 1);
    circ.h(0).measure(0, 0);

    backend->set_seed(1);
    const Result r = backend->execute(circ, 512);

    EXPECT_EQ(r.shots(), std::uint64_t{512});
    EXPECT_EQ(sum_counts(r.counts()), std::uint64_t{512});
}

// ── has_statevector is true after execute (both paths) ───────────────────────

TEST_F(ExecuteFixture, Execute_FastPath_HasStatevector) {
    Circuit circ(1, 1);
    circ.h(0).measure(0, 0);

    backend->set_seed(2);
    const Result r = backend->execute(circ, 10);

    EXPECT_TRUE(r.has_statevector());
}

TEST_F(ExecuteFixture, Execute_PerShotPath_HasStatevector) {
    // Per-shot path: needs IfOp.
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    GateOp x;
    x.type = GateType::X; x.qubits = {1};
    circ.if_then(0, 1, x);
    circ.measure(1, 1);

    backend->set_seed(3);
    const Result r = backend->execute(circ, 10);

    EXPECT_TRUE(r.has_statevector());
}
