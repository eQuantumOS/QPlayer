// qubit_reorder.hpp — CPU qubit-permutation primitive for.
//
// guarantees "transparent" qubit reordering: target qubits are placed
// in contiguous memory positions before gate application, and the external
// qubit-index numbering is unchanged (product-spec).
//
// The *GPU* coalesced-access variant lives in qubit_reorder.cu.
// On CPU, gate kernels already use index arithmetic to handle arbitrary target
// qubits without physical movement, so reorder_qubits() is not wired into the
// CPU execute() hot path.  Instead it provides a tested, correctness-verified
// bit-permutation primitive that:
//   1. Serves the GPU backend as a CPU reference for cross-validation.
//   2. Enables future CPU coalesced optimisations if profiling reveals benefit.
// 3. Satisfies "reorder transparency" DoD: round-trip == identity.

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
