/// @file memory_check.hpp
/// @brief Pre-allocation memory budget guard used by the dense backends.
/// @ingroup backend
///
/// Pre-allocation memory budget guard for backends.
/// FullState CPU/GPU backends call check_memory_budget() inside prepare() before
/// allocating the 2^n state vector. SparseState / TensorNetwork skip the check
/// because their footprint cannot be bounded ahead of time.

#pragma once

#include <cstdint>
#include <string_view>

#include "types.hpp"

namespace qplayer::detail {

// Returns the available memory budget in bytes (-1).
// Priority: QPLAYER_MEMORY_BUDGET_GB env (exact) > min(/proc/meminfo
// MemAvailable, cgroup v1/v2 limit) × 0.8. The cgroup term keeps the budget
// honest inside containers, where /proc/meminfo reports the host and not the
// cgroup cap. uint64_t (not size_t) so the signature is identical on 32-bit
// hosts — relevant for Python bindings on remote inspectors.
std::uint64_t available_memory_bytes();

// Throws SimulatorError when required_bytes exceeds available_memory_bytes().
// Message format: "[<backend_name>] insufficient memory: requires X.XX GiB,
// available Y.YY GiB (n_qubits = <N>)".
void check_memory_budget(std::uint64_t    required_bytes,
                         std::string_view backend_name,
                         QubitIndex       num_qubits);

/// Selects which allocation profile check_memory_budget() validates against.
/// @ingroup backend
///
/// Working-set kind for the FullState CPU peak-memory model.
/// The bare state vector (re + im SoA) is 2^n * 16 bytes; the two kinds capture
/// the transient buffers layered on top of it.
enum class WorkingSet {
    Execute,      // base + one transient (reorder scratch OR sampling prob/cdf,
                  // which never overlap) -> 2 * 2^n * 16
    Materialize,  // the additional AoS copy on top of the already-resident SoA;
                  // the live budget already reflects the base -> 1 * 2^n * 16
};

// Peak working-set bytes over the bare state vector for the given kind
// Returns UINT64_MAX when 2^n * 16 would overflow uint64
// (num_qubits >= 60) so the caller treats it as "exceeds any budget" rather
// than silently wrapping (safety).
std::uint64_t peak_state_bytes(QubitIndex num_qubits, WorkingSet ws);

// Largest qubit count whose peak_state_bytes(n, ws) fits within
// available_memory_bytes(). Backs QRegister::max_qubits().
std::uint32_t max_qubits_for_budget(WorkingSet ws);

}  // namespace qplayer::detail
