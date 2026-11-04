/// @file partitioned_store.hpp
/// @brief Persistent in-place prime-modulo amplitude store for SparseState.
/// @ingroup backend
///
/// persistent, in-place, prime-modulo amplitude store for
/// the SparseState backend (Phase 8-E). Replaces the /0923
/// double-buffer store, which rebuilt a fresh store every gate (flatten snapshot
/// + T×P bucket matrix + P fresh maps + 2·S node churn + 4× peak memory —
/// verify-sparse 2026-07-23: cat_n35 22×, surface_d5 65× slower than the
/// ETRI-2021 generation).
///
/// Design:
/// Partition key = idx % P (prime modulo, default P=1171). A prime modulus
/// spreads the 2^k regularity of basis indices across shards, avoiding the
/// top-bit skew. QPLAYER_SPARSE_PARTITIONS overrides P.
/// The store is sized ONCE (from init_state) and kept across gates; gates
/// mutate amplitudes in place — no per-gate rebuild, no flatten.
/// Gate application is LOCK-FREE (4): STEP1 reads the store
/// read-only (lower-representative rule → each pair once) and appends outputs
/// to per-thread, per-TARGET-partition buckets (GateBuckets, sized once and
/// reused); STEP2 merges each target partition from a single thread. No mutex
/// (the earlier mutex+stage claim collapsed under 144-thread futex contention
/// — surface_d5 1491s). Kernels live in sparse_state_backend.cpp.
///
/// Reproducibility (retained): each output amplitude is a
/// fixed 2-term form produced exactly once, and the merge is a disjoint per-key
/// apply, so the state is bit-identical regardless of P, thread count, and
/// machine. All reductions feeding sampling/measurement go through
/// sorted_entries() (ascending StateIndex, deterministic parallel sort).

#pragma once

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>       // std::getenv, std::stol (prime partition count)
#include <string>        // std::stol
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "types.hpp"

namespace qplayer {

/// Hash functor for 128-bit state indices.
/// @ingroup backend
///
/// std::hash is not specialised for unsigned __int128 in GCC libstdc++.
/// noexcept folding of the two 64-bit halves via golden-ratio multiplicative
/// mixing; noexcept lets the libstdc++ hashtable skip hash-code caching.
struct StateIndexHash {
    [[nodiscard]] std::size_t operator()(StateIndex idx) const noexcept {
        const auto lo = static_cast<std::uint64_t>(idx);
        const auto hi = static_cast<std::uint64_t>(idx >> 64);
        constexpr std::uint64_t kMul = 0x9e3779b97f4a7c15ULL;
        const std::uint64_t h_lo = lo * kMul;
        const std::uint64_t h_hi = hi * kMul;
        return static_cast<std::size_t>(h_lo ^ (h_hi << 1));
    }
};

namespace detail {

// Prime partition count for the persistent store (1·2). Resolved
// once per process: QPLAYER_SPARSE_PARTITIONS overrides; otherwise the prime
// default 1171 (the ETRI-2021 QSTORE_PARTITION value). A prime modulus spreads
// the 2^k regularity of basis indices across shards, avoiding the top-bit skew
// that 's partitioning suffered. Inline (header-only) so the persistent
// store needs no .cpp; the function-local static is shared across TUs.
inline std::size_t resolve_prime_partition_count() {
    static const std::size_t cached = []() -> std::size_t {
        if (const char* env = std::getenv("QPLAYER_SPARSE_PARTITIONS")) {
            try {
                const long v = std::stol(env);
                if (v >= 1) { return static_cast<std::size_t>(v); }
            } catch (...) { /* fall through to prime default */ }
        }
        return 1171;
    }();
    return cached;
}

// Deterministic parallel sort of (StateIndex, amp) entries by ascending
// StateIndex. Keys are unique, so the fully-sorted output is unique —
// identical regardless of thread count or block split, preserving the canonical
// reduction order for reproducible sampling/measurement. This
// replaces the single serial std::sort that dominated large-state measurement and
// final sampling (surface_d5 final sort 8.2s → parallel). Bottom-up merge sort:
// sort T contiguous blocks in parallel, then merge with doubling width.
template <class Entry>
inline void parallel_sort_by_index(std::vector<Entry>& v) {
    const std::size_t n = v.size();
    if (n < 2) return;
    const auto cmp = [](const Entry& a, const Entry& b) { return a.first < b.first; };
#ifdef _OPENMP
    const std::size_t threads = static_cast<std::size_t>(std::max(1, omp_get_max_threads()));
#else
    const std::size_t threads = 1;
#endif
    // Small inputs: serial sort avoids parallel-region overhead.
    if (threads == 1 || n < (std::size_t{1} << 16)) {
        std::sort(v.begin(), v.end(), cmp);
        return;
    }
    const std::size_t nblocks = std::min(threads, n);
    const std::size_t block   = (n + nblocks - 1) / nblocks;
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t b = 0; b < nblocks; ++b) {
        const std::size_t lo = b * block;
        const std::size_t hi = std::min(n, lo + block);
        if (lo < hi) std::sort(v.begin() + lo, v.begin() + hi, cmp);
    }
    // Double-buffered bottom-up merge. src/dst alternate between v and tmp.
    std::vector<Entry> tmp(n);
    Entry* src = v.data();
    Entry* dst = tmp.data();
    for (std::size_t width = block; width < n; width *= 2) {
        #pragma omp parallel for schedule(dynamic)
        for (std::size_t lo = 0; lo < n; lo += 2 * width) {
            const std::size_t mid = std::min(n, lo + width);
            const std::size_t hi  = std::min(n, lo + 2 * width);
            std::merge(src + lo, src + mid, src + mid, src + hi, dst + lo, cmp);
        }
        std::swap(src, dst);
    }
    if (src != v.data()) {
        std::copy(src, src + n, v.begin());
    }
}

}  // namespace detail

/// Per-partition scratch used while applying one gate.
/// @ingroup backend
///
/// Per-thread gate output buckets, keyed by TARGET partition (4).
/// The lock-free gate kernel fills scratch[tid].set[q] / .del[q] during STEP1
/// (read-only on the store); merge then applies each target partition q from a
/// single thread. Persistent (sized once in configure, reused per gate) to avoid
/// per-gate allocation of T×P vectors.
struct GateBuckets {
    std::vector<std::vector<std::pair<StateIndex, std::complex<double>>>> set;  // [target partition]
    std::vector<std::vector<StateIndex>>                                  del;  // [target partition]
};

/// Partitioned amplitude map mutated in place across gates, replacing the double-buffer store that rebuilt itself every gate.
/// @ingroup backend
class PersistentAmplitudeStore {
public:
    /// One partition's amplitude map, keyed by state index.
    using Shard = std::unordered_map<StateIndex, std::complex<double>, StateIndexHash>;
    /// A single state index paired with its amplitude.
    using Entry = std::pair<StateIndex, std::complex<double>>;

    PersistentAmplitudeStore() : shards_(1) {}

    // Sizes the store to P prime partitions and clears all amplitudes. Called
    // ONCE per state (from init_state) — NOT per gate (: persistent
    // store, no per-gate rebuild). num_qubits is kept for bit-width queries;
    // partition_of uses idx % P independent of n. Also sizes the persistent
    // lock-free gate scratch (one bucket set per OpenMP thread).
    void configure(QubitIndex num_qubits) {
        num_qubits_ = num_qubits;
        P_          = detail::resolve_prime_partition_count();
        shards_     = std::vector<Shard>(P_);
#ifdef _OPENMP
        const std::size_t T = static_cast<std::size_t>(std::max(1, omp_get_max_threads()));
#else
        const std::size_t T = 1;
#endif
        scratch_ = std::vector<GateBuckets>(T);
        for (auto& gb : scratch_) {
            gb.set = std::vector<std::vector<Entry>>(P_);
            gb.del = std::vector<std::vector<StateIndex>>(P_);
        }
    }

    void clear() {
        for (auto& s : shards_) s.clear();
    }

    // Resets to the ground state |0...0>: amplitude 1+0i at index 0 (partition 0).
    void set_ground() {
        clear();
        shards_[partition_of(StateIndex{0})][StateIndex{0}] = {1.0, 0.0};
    }

    [[nodiscard]] std::size_t num_partitions() const noexcept { return P_; }
    [[nodiscard]] QubitIndex  num_qubits()     const noexcept { return num_qubits_; }

    // Partition index = idx mod P (prime modulo 1). StateIndex is
    // __uint128_t — constructed form, no raw literal (coding-rules).
    [[nodiscard]] std::size_t partition_of(StateIndex idx) const noexcept {
        return static_cast<std::size_t>(idx % StateIndex{P_});
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t n = 0;
        for (const auto& s : shards_) n += s.size();
        return n;
    }

    [[nodiscard]] bool empty() const noexcept {
        for (const auto& s : shards_) if (!s.empty()) return false;
        return true;
    }

    // Amplitude lookup; returns 0 when absent (sparse semantics).
    [[nodiscard]] std::complex<double> get(StateIndex idx) const {
        const Shard& s = shards_[partition_of(idx)];
        auto it = s.find(idx);
        return it == s.end() ? std::complex<double>{0.0, 0.0} : it->second;
    }

    // Insert or overwrite amplitude (no threshold check — caller's responsibility).
    void put(StateIndex idx, std::complex<double> amp) {
        shards_[partition_of(idx)][idx] = amp;
    }

    void erase(StateIndex idx) {
        shards_[partition_of(idx)].erase(idx);
    }

    Shard&       shard(std::size_t p)       noexcept { return shards_[p]; }
    const Shard& shard(std::size_t p) const noexcept { return shards_[p]; }
    std::vector<Shard>&       shards()       noexcept { return shards_; }
    const std::vector<Shard>& shards() const noexcept { return shards_; }

    // Persistent lock-free gate scratch. scratch()[tid].set[q]/.del[q]
    // accumulate this gate's outputs keyed by target partition; clear_scratch()
    // empties them (keeping capacity) at the start of each gate.
    std::vector<GateBuckets>& scratch() noexcept { return scratch_; }
    void clear_scratch() noexcept {
        for (auto& gb : scratch_) {
            for (auto& v : gb.set) v.clear();
            for (auto& v : gb.del) v.clear();
        }
    }

    // Entries flattened and sorted by ascending StateIndex — the canonical order
    // for every reduction feeding sampling/measurement, so results are
    // independent of P, shard order, and thread count. The
    // flatten copies each shard into its pre-computed offset slice in parallel
    // (disjoint slices), then a deterministic parallel sort orders the whole.
    [[nodiscard]] std::vector<Entry> sorted_entries() const {
        const std::size_t P = shards_.size();
        std::vector<std::size_t> offsets(P + 1, 0);
        for (std::size_t p = 0; p < P; ++p) {
            offsets[p + 1] = offsets[p] + shards_[p].size();
        }
        std::vector<Entry> out(offsets[P]);
        #pragma omp parallel for schedule(dynamic)
        for (std::size_t p = 0; p < P; ++p) {
            std::size_t pos = offsets[p];
            for (const auto& kv : shards_[p]) {
                out[pos++] = Entry{kv.first, kv.second};
            }
        }
        detail::parallel_sort_by_index(out);
        return out;
    }

private:
    std::vector<Shard>       shards_;
    std::vector<GateBuckets> scratch_;  // persistent lock-free gate scratch
    QubitIndex               num_qubits_ = 0;
    std::size_t              P_          = 1;
};

}  // namespace qplayer
