// test_memory_check.cpp — unit tests for the memory budget guard
// Covers:
//   1. available_memory_bytes() honours QPLAYER_MEMORY_BUDGET_GB exactly.
//   2. check_memory_budget() passes a 25-qubit state vector (0.5 GiB) and
//      rejects a 26-qubit one (1.0 GiB) under a 0.75 GiB budget — the canonical
// "25 qubits OK / 26 qubits pre-rejected" scenario in.
//   3. An oversized override relaxes the check; an invalid override falls back
//      to /proc/meminfo without throwing on a trivial request.
//
// The QPLAYER_MEMORY_BUDGET_GB env var is process-global state, so a fixture
// clears it in TearDown() (invoked even when the body throws) to keep tests
// independent (— no leaked global state).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <cstdlib>

#include "../../../include/backend/memory_check.hpp"
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
