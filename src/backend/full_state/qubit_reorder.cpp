// qubit_reorder.cpp — CPU bit-permutation primitive for.
//
// Provides reorder_qubits() and inverse_permutation() in qplayer::detail.
// The GPU coalesced-access variant lives in qubit_reorder.cu.
// On CPU, full_state_cpu.cpp gate kernels already handle arbitrary target
// qubits via index arithmetic, so this file is NOT wired into the CPU execute()
// hot path.  Its purpose is to supply a transparent, round-trip-correct
// bit-permutation primitive that satisfies the DoD requirement:
// "round-trip reorder == identity".

#include "backend/full_state/qubit_reorder.hpp"

#include <algorithm>   // std::sort (for duplicate detection)
#include <cstdlib>     // std::aligned_alloc, std::free
#include <cstring>     // std::memcpy
#include <vector>

#include "error.hpp"
#include "types.hpp"

namespace qplayer {
namespace detail {

namespace {

// Validate that perm is a permutation of [0, n).
// Throws SimulatorError on size mismatch or duplicate/out-of-range element.
void validate_permutation(QubitIndex n_qubits,
                          const std::vector<QubitIndex>& perm) {
    if (static_cast<QubitIndex>(perm.size()) != n_qubits) {
        throw SimulatorError{
            "FullState",
            "reorder_qubits: invalid permutation — size mismatch"};
    }
    // Check bijectivity: copy, sort, and verify it equals [0,1,...,n-1].
    std::vector<QubitIndex> sorted(perm.begin(), perm.end());
    std::sort(sorted.begin(), sorted.end());
    for (QubitIndex i = 0; i < n_qubits; ++i) {
        if (sorted[i] != i) {
            throw SimulatorError{
                "FullState",
                "reorder_qubits: invalid permutation — not a bijection"};
        }
    }
}

// Compute new_index from old_index given permutation perm.
// Convention: result bit i = old_index bit perm[i], matching
// full_state_cpu.cpp bit-addressing (bit q of idx = (idx >> q) & 1).
inline StateIndex permute_index(StateIndex old_idx,
                                const std::vector<QubitIndex>& perm) {
    StateIndex new_idx = 0;
    const QubitIndex n = static_cast<QubitIndex>(perm.size());
    for (QubitIndex i = 0; i < n; ++i) {
        // Extract bit perm[i] from old_idx and place it at bit i of new_idx.
        const StateIndex bit = (old_idx >> perm[i]) & StateIndex{1};
        new_idx |= (bit << i);
    }
    return new_idx;
}

}  // anonymous namespace

// ── reorder_qubits ───────────────────────────────────────────────────────────

void reorder_qubits(double* re, double* im, StateIndex dim,
                    QubitIndex n_qubits,
                    const std::vector<QubitIndex>& perm) {
    validate_permutation(n_qubits, perm);

    // Allocate 64-byte-aligned output buffers (AVX-512 requires 64B).
    const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
    const std::size_t align = 64;
    const std::size_t padded = (bytes + align - 1) & ~(align - 1);

    // new/delete forbidden; use std::aligned_alloc+free.
    double* out_re = static_cast<double*>(std::aligned_alloc(align, padded));
    double* out_im = static_cast<double*>(std::aligned_alloc(align, padded));
    if (!out_re || !out_im) {
        std::free(out_re);
        std::free(out_im);
        throw SimulatorError{"FullState",
                             "reorder_qubits: aligned_alloc failed"};
    }

    // Scatter: out[new_index] = in[old_index].
    // The mapping is a bijection so every output slot is written exactly once
    // — no write conflict across iterations (safe for OpenMP parallel for).
    // parallel for over independent indices.
#pragma omp parallel for schedule(static)
    for (StateIndex i = 0; i < dim; ++i) {
        const StateIndex j = permute_index(i, perm);
        out_re[j] = re[i];
        out_im[j] = im[i];
    }

    // Copy result back in-place, then release temporary buffers.
    std::memcpy(re, out_re, bytes);
    std::memcpy(im, out_im, bytes);
    std::free(out_re);
    std::free(out_im);
}

// ── inverse_permutation ──────────────────────────────────────────────────────

std::vector<QubitIndex> inverse_permutation(
    const std::vector<QubitIndex>& perm) {
    const QubitIndex n = static_cast<QubitIndex>(perm.size());
    validate_permutation(n, perm);

    std::vector<QubitIndex> inv(n);
    for (QubitIndex i = 0; i < n; ++i) {
        // inverse[perm[i]] = i by definition of the inverse permutation.
        inv[perm[i]] = i;
    }
    return inv;
}

}  // namespace detail
}  // namespace qplayer
