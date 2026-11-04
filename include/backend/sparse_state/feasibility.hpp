// feasibility.hpp — SparseState pre-flight memory feasibility analysis
// (Phase 7-F). Statically estimates an upper bound Ŝ on the
// non-zero amplitude count a circuit can reach, so prepare() can reject circuits
// that cannot fit the memory budget *before* evolving them (instead of relying
// on the post-hoc std::bad_alloc path of). The estimate is a safe
// upper bound and stays independent of the get_statevector() dense fast-path
// — false-rejects of low-qubit high-branch circuits are relaxed
// via QPLAYER_MEMORY_BUDGET_GB, never by under-estimating.

#pragma once

#include <cstdint>

#include "types.hpp"  // StateIndex, QubitIndex

namespace qplayer {

class Circuit;  // forward decl — feasibility only reads ops()/num_qubits()

namespace detail {

// Bytes charged per stored non-zero amplitude (measured).
// 168 B = unordered_map node ~64 B (key 16 + complex 16 + bucket/hash overhead)
// × 2.78 swap double-buffer transient: apply_gate() holds amplitudes_ and the
// rebuilt next map live simultaneously at swap time, so the real peak RSS is
// ~2.78× the logical node size (linear-fit slope 168 B/entry, R²=0.9999,
// 1326.md). Shared
// with the runtime guard so pre-flight and in-flight use one accounting
// basis.
constexpr std::uint64_t kSparseEntryBytes = 168;

// Result of analyze_sparse_feasibility(): the worst-case non-zero count and the
// memory it would need, compared against the supplied budget.
struct SparseFeasibility {
    StateIndex    estimated_max_states;  // Ŝ — upper bound, saturated (see .cpp)
    std::uint64_t estimated_bytes;       // Ŝ × kSparseEntryBytes
    std::uint64_t budget_bytes;          // budget the estimate was compared to
    bool          feasible;              // estimated_bytes <= budget_bytes
};

// Abstract-interprets the circuit gate-by-gate, tracking an upper bound S on the
// non-zero amplitude count (start S=1 for |0...0>). Branching gates double S
// (saturating), diagonal/permutation/no-op/measure gates leave it unchanged
// (direct-count algorithm —). Permutations preserve S exactly
// (index relabel), so GHZ/cat states stay at S=2 rather than over-estimating to
// 2^n. IfOp bodies are assumed to always execute (conservative upper bound).
SparseFeasibility analyze_sparse_feasibility(const Circuit& circuit,
                                             std::uint64_t  budget_bytes);

}  // namespace detail
}  // namespace qplayer
