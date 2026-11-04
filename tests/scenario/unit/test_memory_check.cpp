// test_memory_check.cpp — unit tests for the memory budget guard
// Covers:
//   1. available_memory_bytes() honours QPLAYER_MEMORY_BUDGET_GB exactly.
//   2. check_memory_budget() passes a 25-qubit state vector (0.5 GiB) and
//      rejects a 26-qubit one (1.0 GiB) under a 0.75 GiB budget — the canonical
// "25 qubits OK / 26 qubits pre-rejected" scenario.
//   3. An oversized override relaxes the check; an invalid override falls back
//      to /proc/meminfo without throwing on a trivial request.
//
// The QPLAYER_MEMORY_BUDGET_GB env var is process-global state, so a fixture
// clears it in TearDown() (invoked even when the body throws) to keep tests
// independent (— no leaked global state).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <limits>
#include <cstdlib>

#include "../../../include/backend/memory_check.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/qregister.hpp"
#include "../../../include/error.hpp"

using qplayer::SimulatorError;
using qplayer::detail::available_memory_bytes;
using qplayer::detail::check_memory_budget;
using qplayer::detail::max_qubits_for_budget;
using qplayer::detail::peak_state_bytes;
using qplayer::detail::WorkingSet;

namespace {

constexpr std::uint64_t kBytesPerGiB = std::uint64_t{1} << 30;

// FullState CPU footprint: 2^n complex<double> in SoA = 2^n × 16 bytes
// (0909).
std::uint64_t state_vector_bytes(std::uint32_t num_qubits) {
    return (std::uint64_t{1} << num_qubits) * 16ULL;
}

// Clears the memory-budget env var before and after each test so neither the
// host environment nor a previous test leaks in.
class MemoryCheckFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override    { ::unsetenv("QPLAYER_MEMORY_BUDGET_GB"); }
    void TearDown() override  { ::unsetenv("QPLAYER_MEMORY_BUDGET_GB"); }
};

}  // namespace

// available_memory_bytes() returns the env budget verbatim (× 1 GiB), with the
// 0.8 margin applied only on the /proc/meminfo path — not on explicit override.
TEST_F(MemoryCheckFixture, AvailableBytes_BudgetEnvSet_ReturnsExactBudget) {
    // Arrange
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "4.0", 1);

    // Act
    const std::uint64_t avail = available_memory_bytes();

    // Assert: exactly 4 GiB, no extra 0.8 scaling on the override path.
    EXPECT_EQ(avail, 4ULL * kBytesPerGiB);
}

// 25 qubits = 0.5 GiB < 0.75 GiB budget → within budget, no throw.
TEST_F(MemoryCheckFixture, CheckBudget_25Qubits_WithinBudget_NoThrow) {
    // Arrange
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.75", 1);

    // Act / Assert
    EXPECT_NO_THROW(
        check_memory_budget(state_vector_bytes(25), "FullState", 25));
}

// 26 qubits = 1.0 GiB > 0.75 GiB budget → pre-rejected with SimulatorError.
TEST_F(MemoryCheckFixture, CheckBudget_26Qubits_ExceedsBudget_Throws) {
    // Arrange
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.75", 1);

    // Act / Assert
    EXPECT_THROW(
        check_memory_budget(state_vector_bytes(26), "FullState", 26),
        SimulatorError);
}

// An over-large override relaxes the guard so a 30-qubit (16 GiB) request that
// would fail on a small host still passes (override policy).
TEST_F(MemoryCheckFixture, CheckBudget_OverrideRelaxes_NoThrow) {
    // Arrange
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "100000", 1);

    // Act / Assert
    EXPECT_NO_THROW(
        check_memory_budget(state_vector_bytes(30), "FullState", 30));
}

// A non-numeric override is ignored (warn) and the check falls back to
// /proc/meminfo, which comfortably admits a 1-qubit (32-byte) request.
TEST_F(MemoryCheckFixture, CheckBudget_InvalidEnv_FallsBackToMeminfo) {
    // Arrange
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "not-a-number", 1);

    // Act / Assert
    EXPECT_NO_THROW(
        check_memory_budget(state_vector_bytes(1), "FullState", 1));
}

// Without an override the budget comes from /proc/meminfo (or the kNoLimit
// fallback); either way it must be strictly positive.
TEST_F(MemoryCheckFixture, AvailableBytes_NoEnv_ReturnsPositive) {
    // Act
    const std::uint64_t avail = available_memory_bytes();

    // Assert
    EXPECT_TRUE(avail > 0);
}

// ── : peak working-set model ────────────────────────────────────────

// Execute layers one transient buffer on the base SoA -> 2 × 2^n × 16.
TEST_F(MemoryCheckFixture, PeakStateBytes_Execute_IsTwiceBase) {
    // 2^10 × 16 = 16384; Execute doubles it.
    EXPECT_EQ(peak_state_bytes(10, WorkingSet::Execute),
              2ULL * state_vector_bytes(10));
}

// Materialize is the lone extra AoS copy -> 1 × 2^n × 16 (the resident base is
// already reflected in the live budget at get_statevector() time).
TEST_F(MemoryCheckFixture, PeakStateBytes_Materialize_IsBase) {
    EXPECT_EQ(peak_state_bytes(10, WorkingSet::Materialize),
              state_vector_bytes(10));
}

// 2^n × 16 overflows uint64 past n=58; the guard returns the kNoLimit sentinel
// (UINT64_MAX) so callers treat it as "exceeds any budget".
TEST_F(MemoryCheckFixture, PeakStateBytes_OverflowGuard_ReturnsMax) {
    EXPECT_EQ(peak_state_bytes(59, WorkingSet::Execute), UINT64_MAX);
    EXPECT_EQ(peak_state_bytes(64, WorkingSet::Materialize), UINT64_MAX);
}

// ── : dynamic max-qubits derivation ─────────────────────────────────

// With a 16 GiB budget (= 2^34 bytes): Execute peak = 2^(n+5) ≤ 2^34 -> n ≤ 29.
TEST_F(MemoryCheckFixture, MaxQubits_Execute_16GiBBudget_Is29) {
    // Arrange
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "16.0", 1);

    // Act / Assert: 2^29 × 32 = 16 GiB fits exactly; 2^30 × 32 = 32 GiB does not.
    EXPECT_EQ(max_qubits_for_budget(WorkingSet::Execute),
              static_cast<std::uint32_t>(29));
}

// Materialize peak = 2^(n+4) ≤ 2^34 -> n ≤ 30, one qubit looser than Execute.
TEST_F(MemoryCheckFixture, MaxQubits_Materialize_16GiBBudget_Is30) {
    // Arrange
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "16.0", 1);

    // Act / Assert
    EXPECT_EQ(max_qubits_for_budget(WorkingSet::Materialize),
              static_cast<std::uint32_t>(30));
}

// ── Unrepresentable sizes must be refused, not tautologically passed (#17) ────
//
// peak_state_bytes() returns kNoLimit (UINT64_MAX) for n >= 59 because 2^(n+5)
// overflows uint64. check_memory_budget() then compared required <= available,
// and when neither /proc/meminfo nor the cgroup limit is readable `available` is
// also kNoLimit — so the guard degenerated into kNoLimit <= kNoLimit and even
// n = 100 passed. The backend then truncated the byte count to 0 and wrote into a
// zero-byte allocation. A kNoLimit *requirement* now fails unconditionally.

TEST_F(MemoryCheckFixture, CheckBudget_UnrepresentableSize_ThrowsEvenWithHugeBudget) {
    // A huge but representable budget: the refusal must not depend on the budget.
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "1000000", 1);

    const std::uint64_t required =
        peak_state_bytes(100, WorkingSet::Execute);  // kNoLimit
    EXPECT_EQ(required, std::numeric_limits<std::uint64_t>::max());
    EXPECT_THROW(check_memory_budget(required, "FullState", 100),
                 qplayer::SimulatorError);
}

TEST_F(MemoryCheckFixture, PeakStateBytes_58Representable_59Refused) {
    EXPECT_TRUE(peak_state_bytes(58, WorkingSet::Execute) !=
                std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(peak_state_bytes(59, WorkingSet::Execute),
              std::numeric_limits<std::uint64_t>::max());
}

// ── Budget env overflow must be rejected, not converted (audit #18) ──────────
//
// The old bound allowed up to 1e12 GiB; 1e12 * 2^30 is ~1.07e21 against a uint64
// max of ~1.84e19, and converting an out-of-range double to an integer type is UB
// (not a wrap). Out-of-range values now fall back to /proc/meminfo.

TEST_F(MemoryCheckFixture, AvailableBytes_BudgetOverflowsUint64_FallsBackToMeminfo) {
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "1e12", 1);
    const std::uint64_t with_absurd = available_memory_bytes();

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
    const std::uint64_t without = available_memory_bytes();

    // The absurd override is ignored: the result is the host figure, not 1e12 GiB.
    // Comparing the two reads for equality was flaky -- MemAvailable moves
    // between them whenever another process allocates (2 MB apart under a
    // parallel ctest), so the fallback is checked against a 1% band instead.
    EXPECT_TRUE(with_absurd < static_cast<std::uint64_t>(1e6 * static_cast<double>(kBytesPerGiB)));
    EXPECT_NEAR(static_cast<double>(with_absurd), static_cast<double>(without),
                0.01 * static_cast<double>(without));  // host free-memory churn between reads
}

TEST_F(MemoryCheckFixture, AvailableBytes_LargeButRepresentableBudget_Honoured) {
    // 1e9 GiB = ~1.07e18 bytes, comfortably inside uint64 — must be accepted.
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "1e9", 1);
    const std::uint64_t avail = available_memory_bytes();
    EXPECT_EQ(avail, static_cast<std::uint64_t>(1e9 * static_cast<double>(kBytesPerGiB)));
}

// ── Noise trajectory path: memory-adaptive thread count ───────────
//
// prepare() — and with it check_memory_budget() — is only called on the noiseless
// fast path, so allocated unchecked. Every live trajectory holds its own
// 2 x 2^n doubles, so the requirement scales with the team size. The guard now
// derives that team size from the budget instead of reading omp_get_max_threads()
// and refusing: shrinking the team is what lets a tight-budget run still finish.

TEST_F(MemoryCheckFixture, NoisePath_BudgetBelowTeamSize_ClampsThreadsAndRuns) {
    // 2^14 x 16 B = 256 KiB per trajectory. A ~1.07 MB budget holds only four of
    // them, which is far below omp_get_max_threads() on any real host. The old
    // fixed-team guard costed the run at the full team and refused; the clamp
    // must let it complete instead.
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.001", 1);   // ~1.07 MB

    qplayer::QRegister qr(qplayer::SimType::FullState, qplayer::ProcType::CPU);
    qr.set_seed(1);
    qplayer::NoiseModel nm;
    nm.add_depolarizing("h", 0.05);
    qr.set_noise_model(nm);

    qplayer::Circuit c(14, 1);
    for (qplayer::QubitIndex q = 0; q < 14; ++q) c.h(q);
    c.measure(0, 0);

    EXPECT_NO_THROW(qr.run(c, 4));
}

TEST_F(MemoryCheckFixture, NoisePath_SingleTrajectoryDoesNotFit_Throws) {
    // Below one trajectory's 256 KiB no team size rescues the run, so the guard
    // must still refuse rather than clamp to zero threads.
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "0.0002", 1);  // ~215 KB < 256 KiB

    qplayer::QRegister qr(qplayer::SimType::FullState, qplayer::ProcType::CPU);
    qr.set_seed(1);
    qplayer::NoiseModel nm;
    nm.add_depolarizing("h", 0.05);
    qr.set_noise_model(nm);

    qplayer::Circuit c(14, 1);
    for (qplayer::QubitIndex q = 0; q < 14; ++q) c.h(q);
    c.measure(0, 0);

    EXPECT_THROW(qr.run(c, 1), SimulatorError);
}

TEST_F(MemoryCheckFixture, NoisePath_ClampedThreads_ReproduceFullTeamCounts) {
    // The clamp must not perturb results: trajectory seeds derive from the
    // trajectory index, not the thread id, so the same master seed
    // yields the same per-trajectory stream at any team size. Without that the
    // clamp would silently make noisy runs budget-dependent.
    const auto counts_under = [](const char* budget_gb) {
        ::setenv("QPLAYER_MEMORY_BUDGET_GB", budget_gb, 1);
        qplayer::QRegister qr(qplayer::SimType::FullState, qplayer::ProcType::CPU);
        qr.set_seed(20260813);
        qplayer::NoiseModel nm;
        nm.add_depolarizing("h", 0.2);
        qr.set_noise_model(nm);

        qplayer::Circuit c(10, 1);
        for (qplayer::QubitIndex q = 0; q < 10; ++q) c.h(q);
        c.measure(0, 0);
        return qr.run(c, 64).counts();
    };

    // 2^10 x 16 B = 16 KiB per trajectory: ~21 KB holds exactly one (team of 1),
    // 8 GiB holds the whole team.
    const auto clamped = counts_under("0.00002");
    const auto full    = counts_under("8");

    EXPECT_EQ(clamped.size(), full.size());
    for (const auto& kv : full) {
        EXPECT_EQ(clamped.count(kv.first), std::size_t{1});
        EXPECT_EQ(clamped.at(kv.first), kv.second);
    }
}

// The same circuit must run when the budget covers the full team.
TEST_F(MemoryCheckFixture, NoisePath_SufficientBudget_Runs) {
    ::setenv("QPLAYER_MEMORY_BUDGET_GB", "8", 1);

    qplayer::QRegister qr(qplayer::SimType::FullState, qplayer::ProcType::CPU);
    qr.set_seed(1);
    qplayer::NoiseModel nm;
    nm.add_depolarizing("h", 0.05);
    qr.set_noise_model(nm);

    qplayer::Circuit c(8, 1);
    for (qplayer::QubitIndex q = 0; q < 8; ++q) c.h(q);
    c.measure(0, 0);

    EXPECT_NO_THROW(qr.run(c, 4));
}
