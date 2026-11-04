// types.hpp — central index-type aliases for QPlayer.
// Index domains are kept as strong aliases so signatures self-document and bounds
// stay consistent across IR, backend kernels, and bindings.

#pragma once

#include <cstdint>

namespace qplayer {

// Qubit index in the user-facing IR (e.g. `h q[3]` -> 3). uint32_t covers up to
// 2^32 qubits; the practical cap (MAX_STATEVECTOR_QUBITS) is smaller.
using QubitIndex = std::uint32_t;

// Classical bit (cubit) index addressing a single bit inside ClassicalRegisters.
using ClassicalBitIndex = std::uint32_t;

// Index into Circuit::classical_registers vector. Used by IfOp condition and
// Circuit::creg(name) return type to identify *which register* (not which bit).
using ClassicalRegisterIndex = std::uint32_t;

// Quantum state-space index — addresses an element of the 2^n-dimensional state
// vector or a key in the SparseState amplitude map. unsigned __int128 supports
// up to 128 qubits (2^128 basis states); FullState backends are bound by VRAM
// long before this limit. __int128 is a GCC/Clang extension —
// suppress -Wpedantic locally; already restricts the toolchain to
// GCC >= 11 / Clang >= 13.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
using StateIndex = unsigned __int128;
#  pragma GCC diagnostic pop
#else
#  error "QPlayer requires unsigned __int128 (GCC >= 11 or Clang >= 13)"
#endif

// Read-only view onto a SoA-layout state vector (re/im pair). Used by
// QRegister::get_statevector_raw() to avoid AoS conversion cost.
struct RawStateView {
    const double* re;
    const double* im;
    StateIndex    size;
};

// NOT a cap on the number of simulatable qubits (SparseState/TN run to 128 and
// FullState's live limit comes from QRegister::max_qubits()); it bounds only how
// large a state get_statevector() will densify into a returned dense AoS vector.
// Densification ceiling for SparseState / TensorNetwork get_statevector(), where
// converting a sparse/MPS state to a dense AoS vector is only sane up to ~25
// qubits (2^25 * 16 bytes ≈ 512 MB). FullState CPU no longer uses this
// fixed cap: it derives its qubit limit dynamically from the no-swap memory
// budget (peak working set + cgroup) via Backend::prepare() / get_statevector()
// Query the live FullState limit with QRegister::max_qubits().
constexpr std::uint32_t MAX_STATEVECTOR_QUBITS = 25;

}  // namespace qplayer
