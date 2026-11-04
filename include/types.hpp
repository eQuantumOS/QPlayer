/// @file types.hpp
/// @brief Central index-type aliases for QPlayer.
/// @ingroup core
///
/// Index domains are kept as strong aliases so signatures self-document and bounds
/// stay consistent across IR, backend kernels, and bindings.

#pragma once

#include <cstdint>

namespace qplayer {

/// Qubit index in the user-facing IR (e.g. `h q[3]` -> 3). uint32_t covers up to
/// 2^32 qubits; the practical cap (MAX_DENSE_EXPORT_QUBITS) is smaller.
using QubitIndex = std::uint32_t;

/// Classical bit (cubit) index addressing a single bit inside ClassicalRegisters.
using ClassicalBitIndex = std::uint32_t;

/// Index into Circuit::classical_registers vector. Used by IfOp condition and
/// Circuit::creg(name) return type to identify *which register* (not which bit).
using ClassicalRegisterIndex = std::uint32_t;

// __int128 is a GCC/Clang extension — suppress -Wpedantic locally;
// already restricts the toolchain to GCC >= 11 / Clang >= 13.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
/// Quantum state-space index — addresses an element of the 2^n-dimensional state
/// vector or a key in the SparseState amplitude map. unsigned __int128 supports
/// up to 128 qubits (2^128 basis states); FullState backends are bound by VRAM
/// long before this limit.
using StateIndex = unsigned __int128;
#  pragma GCC diagnostic pop
#else
#  error "QPlayer requires unsigned __int128 (GCC >= 11 or Clang >= 13)"
#endif

/// Read-only view onto a SoA-layout state vector (re/im pair). Used by
/// QRegister::get_statevector_raw() to avoid AoS conversion cost.
struct RawStateView {
    const double* re;   ///< Real parts, `size` elements.
    const double* im;   ///< Imaginary parts, `size` elements.
    StateIndex    size; ///< Number of amplitudes (2^num_qubits).
};

/// Ceiling on EXPORTING a state as a dense AoS vector — the qubit count above
/// which SparseState / TensorNetwork get_statevector() refuses to materialize
/// one, since 2^25 * 16 bytes ≈ 512 MB and each further qubit doubles it
///
///
/// @warning This is NOT a cap on the number of simulatable qubits — SparseState
/// and TensorNetwork run to 128, and FullState's live limit comes from
/// QRegister::max_qubits(). FullState CPU no longer uses this fixed cap: it
/// derives its qubit limit dynamically from the no-swap memory budget (peak
/// working set + cgroup) via Backend::prepare() / get_statevector()
/// Named MAX_STATEVECTOR_QUBITS until 2026-08-11; the old name read
/// as a global qubit cap, which it never was.
constexpr std::uint32_t MAX_DENSE_EXPORT_QUBITS = 25;

}  // namespace qplayer
