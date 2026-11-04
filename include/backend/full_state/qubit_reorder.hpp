/// @file qubit_reorder.hpp
/// @brief Physical qubit reordering kernels, retained but not wired into any execution path.
/// @ingroup backend
///
/// CPU qubit-permutation primitive for.
///
/// guarantees "transparent" qubit reordering: target qubits are placed
/// in contiguous memory positions before gate application, and the external
/// qubit-index numbering is unchanged (product-spec).
///
/// PHYSICAL REORDER IS NOT ADOPTED. Neither this CPU primitive nor
/// the GPU variant in qubit_reorder.cu is wired into an execute() path, and that
/// is the decided end state — not a gap.
///
/// Reason: every gate kernel (CPU and GPU alike) reconstructs its base index by
/// inserting a 0 bit at each gate-qubit position, so a warp's 32 * 2^k accessed
/// amplitudes always cover contiguous runs with no gaps — transfer efficiency is
/// 100% regardless of which qubits the gate targets. There is no coalescing
/// penalty for a reorder to remove, while a reorder costs a full out-of-place
/// scatter (2x the state's traffic) plus a scratch buffer. See for the
/// per-target warp footprint analysis and the re-adoption conditions.
///
/// It is KEPT (Phase 8-F audit #35 reviewed it reaffirmed)
/// because it still serves three roles:
/// 1. CPU reference for cross-validating the GPU reorder kernel.
/// 2. transparency proof: round-trip (perm then inverse) == identity.
/// 3. Starting point if re-adoption conditions are ever met.

#pragma once

#include <vector>

#include "error.hpp"
#include "types.hpp"

namespace qplayer {
namespace detail {

// Permute the SoA state vector so that logical qubit perm[i] occupies bit
// position i of the state-vector index.
//
// Convention (matching full_state_cpu.cpp and gate_fusion.cpp):
//   bit position q of index idx  =  (idx >> q) & 1.
//
// After reorder_qubits(re, im, dim, n, perm):
//   new_index bit i  =  old_index bit perm[i]   for all i in [0, n).
//
// The mapping is a bijection (perm is a permutation), so every output slot
// is written exactly once — #pragma omp parallel for has no write conflicts.
//
// Throws SimulatorError{"FullState", ...} when:
//   - perm.size() != n_qubits, or
//   - perm contains a duplicate or out-of-range value (not a valid permutation).
//
// Memory: allocates a temporary 64-byte-aligned output buffer (re/im each),
// copies the result back in-place, then frees the buffer.
// (— new/delete forbidden; std::aligned_alloc + std::free used)
void reorder_qubits(double* re, double* im, StateIndex dim,
                    QubitIndex n_qubits,
                    const std::vector<QubitIndex>& perm);

// Returns the inverse permutation such that inverse[perm[i]] == i for all i.
// Useful for restoring the original qubit order after a reorder pass.
// Throws SimulatorError{"FullState", ...} on an invalid (non-bijective) perm.
std::vector<QubitIndex> inverse_permutation(const std::vector<QubitIndex>& perm);

}  // namespace detail
}  // namespace qplayer
