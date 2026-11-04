/// @file sparse_state_backend.cpp
/// @brief SparseState backend full implementation.
///
/// unordered_map amplitude storage, threshold truncation,
/// init_state(), get_statevector(), get_statevector_raw() permanent error.
/// apply_gate() sparse unitary, apply_measure/reset (Born collapse),
/// prepare(), execute() with fast-path and per-shot path,
/// set_seed() reproducible PRNG.
/// Noise model unsupported. IdleOp rejected.
/// expectation_value(): O(nnz) walk over stored non-zero entries, no qubit cap.

#include "backend/sparse_state/sparse_state_backend.hpp"

#include <algorithm>    // std::upper_bound, std::max
#include <cmath>        // std::abs, std::sqrt
#include <cstdlib>      // std::getenv
#include <complex>
#include <iomanip>      // std::setprecision (GiB formatting)
#include <map>
#include <numeric>      // std::partial_sum
#include <random>       // std::mt19937_64, std::random_device
#include <sstream>      // std::ostringstream (GiB formatting)
#include <string>
#include <string_view>  // QPLAYER_SPARSE_DIAG env compare
#include <utility>      // std::pair, std::move
#include <variant>
#include <vector>

#ifdef _OPENMP
#include <omp.h>        // omp_get_max_threads (partition count)
#endif

#include "backend/memory_check.hpp"             // available_memory_bytes()
#include "backend/sparse_state/feasibility.hpp"  // analyze_sparse_feasibility()
#include "circuit/circuit.hpp"
#include "detail/rng.hpp"   // thread_seed, splitmix64
#include "error.hpp"
#include "log.hpp"
#include "result.hpp"
#include "types.hpp"

namespace qplayer {

namespace {

// ── In-place lock-free gate kernels (Phase 8-E) ─────────────────────
//
// The persistent store is mutated in place — no per-gate rebuild or flatten. Each
// gate runs two barrier-separated phases, LOCK-FREE:
//   STEP1  #pragma omp parallel over partitions p, READ-ONLY on the store: for
//          each entry, apply the "lower representative" rule so every pair is
//          processed exactly once, compute the fixed 2-term outputs, and append
//          them to the thread's per-TARGET-partition scratch (.set / .del).
//   STEP2  merge_scratch(): #pragma omp parallel over target partition q, each
//          q written by exactly one thread — apply the staged deletes then sets
//          into shard[q] in place. Updates to existing keys reuse nodes (no
//          alloc); only genuinely new keys allocate, only removed keys free.
//
// Why lock-free (Decision 4, revised after the perf finding): the
// earlier mutex+stage claim serialised cross-partition pairs and collapsed under
// 144-thread futex contention on large dense states (surface_d5 regressed to
// 1491s). Here STEP1 never writes the store, so reads need no locks; STEP2 owns
// each shard, so writes need no locks. Each output is a fixed formula produced
// once (lower-rep), and the merge is a disjoint per-key apply, so the result is
// bit-identical for any P / thread count (reductions via sorted_entries()).

using cd    = std::complex<double>;
using Entry = PersistentAmplitudeStore::Entry;

// Applies the store's per-thread scratch to its shards, one target partition per
// thread (lock-free: each q written by exactly one thread; each output key was
// produced exactly once, so deletes and sets are disjoint). Updates to existing
// keys reuse nodes; only genuinely new keys allocate, only removed keys free.
void merge_scratch(PersistentAmplitudeStore& store) {
    auto& scratch = store.scratch();
    const std::size_t P = store.num_partitions();
    const int         T = static_cast<int>(scratch.size());
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t q = 0; q < P; ++q) {
        auto& sq = store.shard(q);
        for (int t = 0; t < T; ++t) {
            for (StateIndex idx : scratch[static_cast<std::size_t>(t)].del[q]) {
                sq.erase(idx);
            }
        }
        for (int t = 0; t < T; ++t) {
            for (const Entry& e : scratch[static_cast<std::size_t>(t)].set[q]) {
                sq[e.first] = e.second;
            }
        }
    }
}

// Deterministic parallel reduction over a canonical (sorted) entry vector: sums
// |amp|^2 over all entries (`total`) and over entries whose index has any `mask`
// bit set (`masked`). Fixed block size makes the result independent of thread
// count; the sorted input makes it independent of P/machine — so measurement and
// sampling stay bit-reproducible while running in parallel.
std::pair<double, double> masked_norm_sum(const std::vector<Entry>& sorted,
                                          StateIndex mask) {
    const std::size_t S = sorted.size();
    constexpr std::size_t kBlk = std::size_t{1} << 16;
    const std::size_t nblk = (S + kBlk - 1) / kBlk;
    if (nblk <= 1) {  // small: serial (avoids parallel-region overhead)
        double total = 0.0, masked = 0.0;
        for (const auto& e : sorted) {
            const double a2 = std::norm(e.second);
            total += a2;
            if ((e.first & mask) != StateIndex{0}) masked += a2;
        }
        return {total, masked};
    }
    std::vector<double> bt(nblk, 0.0), bm(nblk, 0.0);
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t b = 0; b < nblk; ++b) {
        double total = 0.0, masked = 0.0;
        const std::size_t lo = b * kBlk;
        const std::size_t hi = std::min(S, lo + kBlk);
        for (std::size_t k = lo; k < hi; ++k) {
            const double a2 = std::norm(sorted[k].second);
            total += a2;
            if ((sorted[k].first & mask) != StateIndex{0}) masked += a2;
        }
        bt[b] = total;
        bm[b] = masked;
    }
    double total = 0.0, masked = 0.0;  // combine in fixed block order (deterministic)
    for (std::size_t b = 0; b < nblk; ++b) { total += bt[b]; masked += bm[b]; }
    return {total, masked};
}

// Population count over the full 128-bit StateIndex. The FullState kernels get
// away with a single popcountll because n <= ~35 there, but SparseState runs to
// 128 qubits (kMaxSparseQubits) so the high half must be counted too.
int popcount_state_index(StateIndex v) {
    return __builtin_popcountll(static_cast<std::uint64_t>(v)) +
           __builtin_popcountll(static_cast<std::uint64_t>(v >> 64));
}

// Deterministic parallel reduction for <psi|P|psi> over the canonical (sorted)
// entry vector, mirroring masked_norm_sum's fixed-block scheme so the result is
// independent of partition and thread count.
//
// P|j> = coeff(j) |j ^ flip_mask| with coeff(j) = i^num_y * (-1)^popcount(j & sign_mask),
// so <psi|P|psi> = sum_j conj(psi[j ^ flip]) * coeff(j) * psi[j]. Only stored
// entries carry psi[j] != 0, so iterating them covers every non-vanishing term —
// this is what makes the sparse cost O(nnz) instead of O(2^n)
// psi[j ^ flip] comes from store.get(), which yields 0 for
// absent indices; concurrent const lookups on the shard maps are read-only and
// never rehash, so the parallel block loop is safe.
double pauli_expectation_sum(const std::vector<Entry>&        sorted,
                             const PersistentAmplitudeStore&  store,
                             StateIndex                       flip_mask,
                             StateIndex                       sign_mask,
                             cd                               phase) {
    const std::size_t S = sorted.size();
    constexpr std::size_t kBlk = std::size_t{1} << 16;
    const std::size_t nblk = (S + kBlk - 1) / kBlk;

    // P is Hermitian, so the imaginary part is numerical noise; only Re is kept
    // (same contract as FullStateCpuBackend::expectation_value).
    const auto block_sum = [&](std::size_t lo, std::size_t hi) {
        double acc = 0.0;
        for (std::size_t k = lo; k < hi; ++k) {
            const StateIndex j = sorted[k].first;
            const int parity = popcount_state_index(j & sign_mask) & 1;
            const cd coeff = parity ? -phase : phase;
            acc += (std::conj(store.get(j ^ flip_mask)) * coeff * sorted[k].second)
                       .real();
        }
        return acc;
    };

    if (nblk <= 1) {  // small: serial (avoids parallel-region overhead)
        return block_sum(0, S);
    }
    std::vector<double> ba(nblk, 0.0);
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t b = 0; b < nblk; ++b) {
        const std::size_t lo = b * kBlk;
        ba[b] = block_sum(lo, std::min(S, lo + kBlk));
    }
    double acc = 0.0;  // combine in fixed block order (deterministic)
    for (std::size_t b = 0; b < nblk; ++b) acc += ba[b];
    return acc;
}

// inplace_controlled1q — in-place controlled-1Q gate: 1Q gates (control_mask==0),
// CX/CY/CZ/CH/CRZ/CU1/CU3 (one control bit), and CCX (two control bits). The pair
// is (i0 = idx & ~tbit, i1 = idx | tbit); tbit == (1 << target).
void inplace_controlled1q(PersistentAmplitudeStore& store,
                          const detail::Mat2x2& U,
                          StateIndex control_mask,
                          StateIndex tbit,
                          double threshold) {
    const cd u00{U.u00r, U.u00i}, u01{U.u01r, U.u01i};
    const cd u10{U.u10r, U.u10i}, u11{U.u11r, U.u11i};
    const std::size_t P = store.num_partitions();
    store.clear_scratch();
    auto& scratch = store.scratch();

    #pragma omp parallel
    {
        const int tid = 0
#ifdef _OPENMP
            + omp_get_thread_num()
#endif
            ;
        GateBuckets& sc = scratch[static_cast<std::size_t>(tid)];
        #pragma omp for schedule(dynamic)
        for (std::size_t p = 0; p < P; ++p) {
            for (const auto& kv : store.shard(p)) {
                const StateIndex idx = kv.first;
                // Pass-through: control not satisfied → amplitude unchanged (left in store).
                if (control_mask != StateIndex{0} &&
                    (idx & control_mask) != control_mask) {
                    continue;
                }
                if ((idx & tbit) == StateIndex{0}) {
                    // idx is the lower i0 — always process the pair here.
                    const StateIndex i0 = idx;
                    const StateIndex i1 = idx | tbit;
                    const cd a0 = kv.second;
                    const cd a1 = store.get(i1);
                    const cd n0 = u00 * a0 + u01 * a1;
                    const cd n1 = u10 * a0 + u11 * a1;
                    if (std::abs(n0) >= threshold) sc.set[store.partition_of(i0)].emplace_back(i0, n0);
                    else                           sc.del[store.partition_of(i0)].push_back(i0);
                    if (std::abs(n1) >= threshold) sc.set[store.partition_of(i1)].emplace_back(i1, n1);
                    else                           sc.del[store.partition_of(i1)].push_back(i1);
                } else {
                    // idx is the upper i1 — process only when i0 is absent (else the
                    // lower's iteration handles the pair). i0 shares idx's control bits.
                    const StateIndex i1 = idx;
                    const StateIndex i0 = idx & ~tbit;
                    if (std::abs(store.get(i0)) > 0.0) continue;
                    const cd a1 = kv.second;      // a0 == 0
                    const cd n0 = u01 * a1;
                    const cd n1 = u11 * a1;
                    if (std::abs(n0) >= threshold) sc.set[store.partition_of(i0)].emplace_back(i0, n0);
                    if (std::abs(n1) >= threshold) sc.set[store.partition_of(i1)].emplace_back(i1, n1);
                    else                           sc.del[store.partition_of(i1)].push_back(i1);
                }
            }
        }
    }
    merge_scratch(store);
}

// exchange_emit — shared body for SWAP / CSwap. The pair (idx, partner) exchanges
// amplitudes; processed once from the lower representative (or from the upper when
// the lower is absent). Pure permutation, so magnitudes are preserved.
inline void exchange_emit(const PersistentAmplitudeStore& store, GateBuckets& sc,
                          StateIndex idx, cd idx_amp, StateIndex partner,
                          double threshold) {
    if (partner < idx) {
        // idx is the upper member: defer to the lower unless the lower is absent.
        if (std::abs(store.get(partner)) > 0.0) return;
    }
    const cd vi = idx_amp;             // amplitude at idx (the iterated entry)
    const cd vp = store.get(partner);  // amplitude at partner (read-only)
    // idx receives partner's amplitude; partner receives idx's.
    if (std::abs(vp) >= threshold) sc.set[store.partition_of(idx)].emplace_back(idx, vp);
    else                           sc.del[store.partition_of(idx)].push_back(idx);
    if (std::abs(vi) >= threshold) sc.set[store.partition_of(partner)].emplace_back(partner, vi);
    else                           sc.del[store.partition_of(partner)].push_back(partner);
}

// inplace_swap — in-place SWAP(q0, q1).
void inplace_swap(PersistentAmplitudeStore& store,
                  QubitIndex q0, QubitIndex q1, double threshold) {
    const StateIndex  b0 = StateIndex{1} << q0;
    const StateIndex  b1 = StateIndex{1} << q1;
    const std::size_t P  = store.num_partitions();
    store.clear_scratch();
    auto& scratch = store.scratch();

    #pragma omp parallel
    {
        const int tid = 0
#ifdef _OPENMP
            + omp_get_thread_num()
#endif
            ;
        GateBuckets& sc = scratch[static_cast<std::size_t>(tid)];
        #pragma omp for schedule(dynamic)
        for (std::size_t p = 0; p < P; ++p) {
            for (const auto& kv : store.shard(p)) {
                const StateIndex idx  = kv.first;
                const bool       bit0 = (idx & b0) != StateIndex{0};
                const bool       bit1 = (idx & b1) != StateIndex{0};
                if (bit0 == bit1) continue;  // fixed point (no exchange)
                exchange_emit(store, sc, idx, kv.second, idx ^ b0 ^ b1, threshold);
            }
        }
    }
    merge_scratch(store);
}

// inplace_cswap — in-place controlled-SWAP (Fredkin): SWAP(q0,q1) only where the
// control bit is set; control==0 entries pass through. The partner keeps the
// control bit (ctrl differs from q0/q1), so it also satisfies the control.
void inplace_cswap(PersistentAmplitudeStore& store,
                   QubitIndex ctrl, QubitIndex q0, QubitIndex q1, double threshold) {
    const StateIndex  cbit = StateIndex{1} << ctrl;
    const StateIndex  b0   = StateIndex{1} << q0;
    const StateIndex  b1   = StateIndex{1} << q1;
    const std::size_t P    = store.num_partitions();
    store.clear_scratch();
    auto& scratch = store.scratch();

    #pragma omp parallel
    {
        const int tid = 0
#ifdef _OPENMP
            + omp_get_thread_num()
#endif
            ;
        GateBuckets& sc = scratch[static_cast<std::size_t>(tid)];
        #pragma omp for schedule(dynamic)
        for (std::size_t p = 0; p < P; ++p) {
            for (const auto& kv : store.shard(p)) {
                const StateIndex idx = kv.first;
                if ((idx & cbit) == StateIndex{0}) continue;  // control |0>: pass through
                const bool bit0 = (idx & b0) != StateIndex{0};
                const bool bit1 = (idx & b1) != StateIndex{0};
                if (bit0 == bit1) continue;  // fixed point
                exchange_emit(store, sc, idx, kv.second, idx ^ b0 ^ b1, threshold);
            }
        }
    }
    merge_scratch(store);
}

}  // anonymous namespace (in-place gate kernels)

namespace {

// Parses QPLAYER_SPARSE_THRESHOLD from env once at construction time.
// Returns kSparseDefaultThreshold when the var is absent, non-numeric,
// negative, or non-finite (same robustness pattern as budget_from_env).
double parse_threshold_from_env() {
    const char* raw = std::getenv("QPLAYER_SPARSE_THRESHOLD");
    if (raw == nullptr) {
        return kSparseDefaultThreshold;
    }
    try {
        const double val = std::stod(raw);
        if (!(val >= 0.0) || val > 1.0) {  // reject NaN, negative, absurd values
            log::warn("QPLAYER_SPARSE_THRESHOLD is out of range; "
                      "using default 1e-12.");
            return kSparseDefaultThreshold;
        }
        return val;
    } catch (...) {
        log::warn("QPLAYER_SPARSE_THRESHOLD is not a number; "
                  "using default 1e-12.");
        return kSparseDefaultThreshold;
    }
}

// Reads QPLAYER_SPARSE_DIAG once at construction. Any non-empty value other
// than "0" enables the baseline diagnostics.
bool diag_enabled_from_env() {
    const char* raw = std::getenv("QPLAYER_SPARSE_DIAG");
    return raw != nullptr && raw[0] != '\0' && std::string_view{raw} != "0";
}

// Reads QPLAYER_SPARSE_FEASIBILITY once at construction. Default OFF:
// the pre-flight feasibility rejection runs only when this is set to a
// non-empty value other than "0". runtime guard is unaffected (always on).
bool feasibility_enabled_from_env() {
    const char* raw = std::getenv("QPLAYER_SPARSE_FEASIBILITY");
    return raw != nullptr && raw[0] != '\0' && std::string_view{raw} != "0";
}

// Formats a byte count as a GiB string with two decimals (unit).
// Local copy: memory_check.cpp's to_gib() is file-static and the feasibility
// message needs the same human-facing form.
std::string to_gib(std::uint64_t bytes) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2)
       << static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return os.str();
}

// Pre-flight feasibility gate. Estimates the worst-case non-zero
// count and throws SimulatorError when it cannot fit the memory budget. Called
// from both the fast path (via prepare()) and the per-shot path so neither can
// start an unbounded evolution. The estimate is a conservative upper bound and
// independent of the dense fast-path.
void check_sparse_feasibility(const Circuit& circuit) {
    const std::uint64_t budget = detail::available_memory_bytes();
    const detail::SparseFeasibility f =
        detail::analyze_sparse_feasibility(circuit, budget);
    if (!f.feasible) {
        // estimated_max_states is capped at 2^56, so it fits uint64 for display.
        const auto states = static_cast<std::uint64_t>(f.estimated_max_states);
        throw SimulatorError{"SparseState",
            "insufficient memory: estimated " + std::to_string(states) +
            " non-zero states require " + to_gib(f.estimated_bytes) +
            " GiB, available " + to_gib(budget) +
            " GiB; override with QPLAYER_MEMORY_BUDGET_GB"};
    }
}

}  // namespace (env-parse helpers only — gate kernels in first anonymous namespace)

// Partition count resolution for the persistent store lives inline in
// partitioned_store.hpp (detail::resolve_prime_partition_count). The
// old pow2 resolve_partition_count() is removed with the double-buffer store.

// ── Construction ──────────────────────────────────────────────────────────────

SparseStateBackend::SparseStateBackend()
    : threshold_(parse_threshold_from_env()),
      feasibility_check_enabled_(feasibility_enabled_from_env()),
      diag_enabled_(diag_enabled_from_env()) {}

SparseStateBackend::~SparseStateBackend() = default;

// set_feasibility_check() — programmatic toggle for the pre-flight analysis
// peer to the QPLAYER_SPARSE_FEASIBILITY env override.
void SparseStateBackend::set_feasibility_check(bool enable) {
    feasibility_check_enabled_ = enable;
}

// ── Threshold helpers ───────────────────────────────────────

// Inserts or updates amplitudes_[idx] = amp when |amp| >= threshold_.
// Erases the entry when |amp| < threshold_ to maintain numerical hygiene.
void SparseStateBackend::set_amplitude(StateIndex idx,
                                       std::complex<double> amp) {
    if (std::abs(amp) < threshold_) {
        amplitudes_.erase(idx);
    } else {
        amplitudes_.put(idx, amp);
    }
}

// Removes all entries whose |amp| has fallen below threshold_ from every shard.
// Called after in-place mutation of amplitude values (gate paths).
void SparseStateBackend::prune_below_threshold() {
    const double thr = threshold_;
    // Each shard is independent (erase is order-agnostic and value-only), so this
    // parallelises over shards, one shard per thread.
    auto& shards = amplitudes_.shards();
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t p = 0; p < shards.size(); ++p) {
        auto& shard = shards[p];
        for (auto it = shard.begin(); it != shard.end(); ) {
            if (std::abs(it->second) < thr) {
                it = shard.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ── Interactive: init_state ────────────────────────────────────────────

// Initialises the state to |0...0> with num_qubits qubits.
// Clears any previous state and sets amplitude[0] = 1.0 + 0i.
void SparseStateBackend::init_state(QubitIndex num_qubits) {
    // 128-qubit structural cap. Checked before any mutation so a
    // rejected request leaves the previous state intact. All entry points
    // (prepare, per-shot execute, Interactive) funnel through init_state(), so
    // this single guard covers them. n=128 is allowed; n>128 overflows StateIndex.
    if (num_qubits > kMaxSparseQubits) {
        throw SimulatorError{"SparseState",
            "exceeds 128-qubit limit (StateIndex is 128-bit): requested "
            + std::to_string(num_qubits)};
    }

    // Size the shards for n qubits and seed the ground state |0...0>
    // (index 0, amplitude 1+0i — always shard 0). configure() clears all shards.
    amplitudes_.configure(num_qubits);
    amplitudes_.set_ground();
    num_qubits_ = num_qubits;
    peak_nonzero_ = 0;  // diagnostics: reset peak per fresh state.

    // Capture the memory budget for the runtime guard. Read here so
    // every path (batch, per-shot, Interactive) is covered uniformly without a
    // per-gate syscall. kNoLimit (unreadable budget) disables the guard.
    budget_bytes_ = detail::available_memory_bytes();
    gate_counter_ = 0;
}

// ── note_peak — diagnostics peak tracker ─────────────────

void SparseStateBackend::note_peak() {
    if (diag_enabled_ && amplitudes_.size() > peak_nonzero_) {
        peak_nonzero_ = amplitudes_.size();
    }
}

// ── apply_gate — sparse unitary dispatch ───────────────────────────────

void SparseStateBackend::apply_gate(const GateOp& op) {
    // Guard: init_state must have been called first.
    if (num_qubits_ == 0 && amplitudes_.empty()) {
        throw SimulatorError{"SparseState",
            "apply_gate called before init_state"};
    }

    // Runtime memory guard. Every kMemCheckGateInterval gates,
    // verify the live non-zero count still fits the budget captured at
    // init_state(). This is the in-flight safety net behind the prepare()
    // pre-flight estimate: same 168 B accounting basis, but driven by the
    // actual map size so it catches growth the conservative estimate could not
    // have foreseen. Checked before the early-return dispatch below so it covers
    // every gate kind. kNoLimit budget never trips (used <= UINT64_MAX).
    if ((gate_counter_++ % kMemCheckGateInterval) == 0) {
        const std::uint64_t used = amplitudes_.size() * detail::kSparseEntryBytes;
        if (used > budget_bytes_) {
            throw SimulatorError{"SparseState",
                "runtime memory exceeded: " + std::to_string(amplitudes_.size()) +
                " states use " + to_gib(used) + " GiB > budget " +
                to_gib(budget_bytes_) + " GiB"};
        }
    }

    const GateType type = op.type;

    // Reset: delegate to apply_reset public interface (measure + conditional X).
    if (type == GateType::Reset) {
        apply_reset(op.qubits[0]);
        return;
    }

    // Opaque is refused by policy, not pending implementation.
    // An arbitrary k-qubit dense unitary fans each stored entry into up to 2^k
    // entries (k=8 -> 256x), which defeats the sparsity assumption the whole
    // backend rests on. Same class of constraint as noise.
    if (type == GateType::Opaque) {
        throw SimulatorError{"SparseState",
            "Opaque gate not supported. "
            "Use FullState backend for opaque gates."};
    }

    // All gates mutate the persistent store in place: partition-parallel
    // STEP1 + claim-once + deferred commit. No per-gate rebuild, flatten, or move.

    // SWAP: exchange qubit bit positions in every basis state index (bijection).
    if (type == GateType::SWAP) {
        inplace_swap(amplitudes_, op.qubits[0], op.qubits[1], threshold_);
        return;
    }

    // CSwap (Fredkin): control=qubits[0], swap qubits[1]<->qubits[2] when set.
    if (type == GateType::CSwap) {
        inplace_cswap(amplitudes_, op.qubits[0], op.qubits[1], op.qubits[2], threshold_);
        return;
    }

    // ECR is a genuine 4x4 (not control+1Q) and has no 2x2 sub-unitary, so it
    // cannot flow through the shared gate_matrix() path below. SparseState is not
    // a device-noise / native-basis target (the basis-aware transpile path targets
    // FullState — /1006), so reject it explicitly rather than silently
    // applying identity via the gate_matrix() fallback.
    if (type == GateType::ECR) {
        throw SimulatorError{"SparseState",
            "ECR gate not supported (native 2Q basis gate targets FullState)"};
    }
    // Google native 2Q gates are genuine 4x4 and not supported by SparseState
    // for the same reason as ECR above.
    if (type == GateType::ISWAP) {
        throw SimulatorError{"SparseState",
            "iSWAP gate not supported (Google native 2Q gate targets FullState)"};
    }
    if (type == GateType::SQRT_ISWAP) {
        throw SimulatorError{"SparseState",
            "sqrt_iSWAP gate not supported (Google native 2Q gate targets FullState)"};
    }
    if (type == GateType::FSIM) {
        throw SimulatorError{"SparseState",
            "fSim gate not supported (Google native 2Q gate targets FullState)"};
    }

    // All remaining gate types reduce to a controlled-1Q (2x2 unitary) application.
    // Derive the target-qubit sub-unitary and control mask — same convention as
    // FullState apply_gate() (full_state_cpu.cpp:890-913).
    const detail::Mat2x2 U = detail::gate_matrix(type, op.params);

    StateIndex control_mask = StateIndex{0};
    QubitIndex target       = op.qubits[0];

    switch (type) {
        // 2-qubit single-control gates: qubits[0]=control, qubits[1]=target.
        case GateType::CX:
        case GateType::CY:
        case GateType::CZ:
        case GateType::CH:
        case GateType::CRZ:
        case GateType::CU1:
        case GateType::CU3:
            control_mask = StateIndex{1} << op.qubits[0];
            target       = op.qubits[1];
            break;

        // CCX (Toffoli): qubits[0]+qubits[1] control, qubits[2]=target.
        case GateType::CCX:
            control_mask = (StateIndex{1} << op.qubits[0]) |
                           (StateIndex{1} << op.qubits[1]);
            target       = op.qubits[2];
            break;

        default:
            // 1Q gates: control_mask=0, target=qubits[0] (already set above).
            break;
    }

    const StateIndex tbit = StateIndex{1} << target;

    // In-place controlled-1Q: partition-parallel, claim-once, mutate
    // amplitudes_ directly. Single path regardless of which qubits are targeted.
    inplace_controlled1q(amplitudes_, U, control_mask, tbit, threshold_);
}

// ── apply_measure_rng — Born-rule collapse kernel ────────────

int SparseStateBackend::apply_measure_rng(QubitIndex qubit,
                                           std::mt19937_64& rng) {
    if (qubit >= num_qubits_) {
        throw SimulatorError{"SparseState",
            "apply_measure: qubit index out of range"};
    }

    const StateIndex tbit = StateIndex{1} << qubit;

    // Accumulate p1 = Pr(qubit==1) and p_total = sum |amp|^2 (normalisation guard)
    // via a deterministic parallel reduction over canonical StateIndex order — the
    // result is independent of partition count and thread count, required for
    // bit-exact reproducibility of the sampled outcome.
    const auto [p_total, p1] = masked_norm_sum(amplitudes_.sorted_entries(), tbit);

    // Guard against floating-point drift from threshold truncation.
    if (p_total < 1e-15) {
        throw SimulatorError{"SparseState",
            "apply_measure: state norm near zero (numerical underflow)"};
    }

    // Sample measurement outcome from caller's RNG stream.
    std::uniform_real_distribution<double> uniform01(0.0, 1.0);
    const int outcome = (uniform01(rng) < p1 / p_total) ? 1 : 0;

    // Probability of the sampled outcome (for renormalisation).
    const double p_out = (outcome == 1) ? p1 : (p_total - p1);
    if (p_out < 1e-15) {
        throw SimulatorError{"SparseState",
            "apply_measure: p_outcome near zero (numerical underflow)"};
    }

    // Collapse: per shard, remove entries that disagree with the outcome and
    // rescale survivors by norm_inv. Each entry (and each shard) is independent
    // and norm_inv is precomputed, so this parallelises over shards, one shard
    // per thread — result is order-independent.
    const double norm_inv = 1.0 / std::sqrt(p_out / p_total);
    auto& shards = amplitudes_.shards();
    #pragma omp parallel for schedule(dynamic)
    for (std::size_t p = 0; p < shards.size(); ++p) {
        auto& shard = shards[p];
        for (auto it = shard.begin(); it != shard.end(); ) {
            const int bit = static_cast<int>((it->first & tbit) != StateIndex{0});
            if (bit != outcome) {
                it = shard.erase(it);
            } else {
                it->second *= norm_inv;
                ++it;
            }
        }
    }
    // Prune any entries that fell below threshold after rescaling.
    prune_below_threshold();

    return outcome;
}

// ── apply_reset_rng — project qubit to |0> ────────────────────────────

void SparseStateBackend::apply_reset_rng(QubitIndex qubit,
                                          std::mt19937_64& rng) {
    // Measure the qubit; if outcome is 1, apply X to flip it back to |0>.
    const int b = apply_measure_rng(qubit, rng);
    if (b == 1) {
        GateOp x_op;
        x_op.type   = GateType::X;
        x_op.qubits = {qubit};
        apply_gate(x_op);
    }
}

// ── Public apply_measure / apply_reset — standalone / interactive interface ───

// apply_measure() creates a local RNG seeded from master_seed_ so standalone
// (interactive / unit-test) calls are reproducible when set_seed() is used,
// and random otherwise (FullState line 995 pattern).
int SparseStateBackend::apply_measure(QubitIndex qubit) {
    const std::uint64_t seed =
        has_seed_
            ? qplayer::detail::splitmix64(master_seed_ ^
                  (static_cast<std::uint64_t>(qubit) << 16))
            : static_cast<std::uint64_t>(std::random_device{}());
    std::mt19937_64 local_rng(seed);
    return apply_measure_rng(qubit, local_rng);
}

// apply_reset() — delegates to apply_reset_rng with a local RNG.
void SparseStateBackend::apply_reset(QubitIndex qubit) {
    if (num_qubits_ == 0 && amplitudes_.empty()) {
        throw SimulatorError{"SparseState",
            "apply_reset called before init_state"};
    }
    const std::uint64_t seed =
        has_seed_
            ? qplayer::detail::splitmix64(master_seed_ ^
                  (static_cast<std::uint64_t>(qubit) << 24))
            : static_cast<std::uint64_t>(std::random_device{}());
    std::mt19937_64 local_rng(seed);
    apply_reset_rng(qubit, local_rng);
}

// ── prepare — initialise state for batch execution ────────────────────

// init_state() applies the 128-qubit structural guard first, then a
// pre-flight feasibility analysis rejects circuits whose estimated
// worst-case non-zero count cannot fit the memory budget — replacing the
// post-hoc std::bad_alloc reliance of for SparseState. The estimate
// is a conservative upper bound and stays independent of the get_statevector()
// dense fast-path: false-rejects relax via QPLAYER_MEMORY_BUDGET_GB.
void SparseStateBackend::prepare(const Circuit& circuit) {
    init_state(circuit.num_qubits());
    // pre-flight feasibility prediction is opt-in (default OFF). When
    // off, evolution proceeds and the runtime guard + bad_alloc backstop catch
    // any overrun. When on, the predicted non-zero count rejects before running.
    if (feasibility_check_enabled_) {
        check_sparse_feasibility(circuit);
    }
}

// ── execute — fast path + per-shot path ──────────────────

Result SparseStateBackend::execute(const Circuit&     circuit,
                                   std::uint32_t      nshots,
                                   const NoiseModel*  noise) {
    // Noise model not supported — SparseState is noiseless.
    if (noise != nullptr) {
        throw SimulatorError{"SparseState",
            "Noise model not supported. "
            "Use FullState backend for noisy simulation."};
    }

    // Seed the PRNG: use master_seed_ when set_seed() has been called, otherwise a
    // random device. set-ness is the flag, not the value — 0 is valid.
    const bool          seeded = has_seed_;
    const std::uint64_t seed   =
        seeded ? master_seed_
               : static_cast<std::uint64_t>(std::random_device{}());

    Result r;
    std::map<std::string, std::uint64_t> counts;
    const ClassicalBitIndex total_c = circuit.total_cubits();

    if (!circuit.requires_per_shot()) {
        // ── fast path — no IfOp, no mid-circuit measurement ──────────
        // Apply all gate ops once, then sample the final distribution nshots times.
        // Dense 2^n probability vector is NOT built — sparse inverse-CDF sampling
        // directly over amplitudes_ enables 100+ qubit simulation.

        prepare(circuit);
        note_peak();  // capture initial S=1 before evolution.

        for (const Op& op : circuit.ops()) {
            std::visit([this](auto&& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, GateOp>) {
                    if (node.type == GateType::Reset) {
                        apply_reset(node.qubits[0]);
                    } else if (node.type == GateType::Opaque) {
                        throw SimulatorError{"SparseState",
                            "Opaque gate not supported. "
                            "Use FullState backend for opaque gates."};
                    } else {
                        apply_gate(node);
                    }
                } else if constexpr (std::is_same_v<T, IdleOp>) {
                    // IdleOp signals noise pipeline; SparseState rejects noise
                    // (arch).
                    throw SimulatorError{"SparseState",
                        "IdleOp not supported "
                        "(noise unsupported on SparseState)"};
                } else {
                    // MeasureOp: deferred to sampling below.
                    // BarrierOp: no state change.
                    (void)node;
                }
            }, op);
            note_peak();  // track peak non-zero count per op.
        }

        // Collect MeasureOps for bitstring construction.
        std::vector<MeasureOp> measure_ops;
        for (const Op& op : circuit.ops()) {
            if (std::holds_alternative<MeasureOp>(op)) {
                measure_ops.push_back(std::get<MeasureOp>(op));
            }
        }

        // Build sparse (StateIndex, prob) pairs for inverse-CDF sampling.
        // Avoids a dense 2^n vector — critical for 100-qubit circuits.
        // Canonical StateIndex order (sorted_entries) makes the (basis, prob)
        // vector — and therefore the sampling CDF (result.cpp builds it in vector
        // order) — identical for any partition/thread count/machine, the
        // reproducibility anchor for the sharded store.
        // The prob vector is filled in parallel and prob_sum reduced deterministically.
        const std::vector<std::pair<StateIndex, std::complex<double>>> sorted =
            amplitudes_.sorted_entries();
        const std::size_t S = sorted.size();
        std::vector<std::pair<StateIndex, double>> probs(S);
        #pragma omp parallel for schedule(static)
        for (std::size_t k = 0; k < S; ++k) {
            probs[k] = {sorted[k].first, std::norm(sorted[k].second)};
        }
        const double prob_sum = masked_norm_sum(sorted, StateIndex{0}).first;

        // Normalise probabilities to guard against threshold-induced drift.
        if (prob_sum > 0.0) {
            #pragma omp parallel for schedule(static)
            for (std::size_t k = 0; k < S; ++k) { probs[k].second /= prob_sum; }
        }

        // Expose statevector only when densification is feasible.
        if (num_qubits_ <= MAX_DENSE_EXPORT_QUBITS) {
            result_set_statevector(r, get_statevector());
        }

        // Lazy sampling (sparse mode): defer the nshots draw
        // Result::counts(). The normalised sparse (basis_index, prob) snapshot is
        // O(nnz) — sampling needs no dense statevector, so it works beyond
        // MAX_DENSE_EXPORT_QUBITS (e.g. 100-qubit sparse circuits). counts()
        // rebuilds the sparse CDF identically, so a fixed seed reproduces the
        // eager result bit-for-bit.
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map;
        measure_map.reserve(measure_ops.size());
        for (const MeasureOp& m : measure_ops) {
            measure_map.emplace_back(m.qubit, m.cubit);
        }
        result_set_lazy_sampling_sparse(r, nshots, seed, total_c,
                                        std::move(measure_map), std::move(probs));
        result_set_shots(r, nshots);

        // baseline diagnostics: one machine-parseable line.
        // peak_nonzero is the logical S; baseline.py pairs it with /proc VmHWM.
        if (diag_enabled_) {
            std::size_t ng = 0;
            for (const Op& dop : circuit.ops()) {
                if (std::holds_alternative<GateOp>(dop)) { ++ng; }
            }
            log::info("sparse_diag path=fast peak_nonzero=" +
                      std::to_string(peak_nonzero_) + " final_nonzero=" +
                      std::to_string(amplitudes_.size()) + " gates=" +
                      std::to_string(ng) + " qubits=" +
                      std::to_string(static_cast<unsigned>(num_qubits_)));
        }
        return r;

    } else {
        // ── per-shot path — IfOp or mid-circuit measurement present ───
        // Each shot: reset to |0…0>, replay the full op sequence with collapse.

        // 128-qubit structural guard + opt-in pre-flight feasibility
        // (default OFF): this path calls init_state() per shot, so the
        // prepare() chokepoint is bypassed — guard + (optional) check here instead.
        init_state(circuit.num_qubits());
        if (feasibility_check_enabled_) {
            check_sparse_feasibility(circuit);
        }

        const QubitIndex num_q = circuit.num_qubits();
        const auto& cregs      = circuit.classical_registers();

        std::vector<int> cubit_vals(total_c, 0);

        for (std::uint32_t shot = 0; shot < nshots; ++shot) {
            init_state(num_q);
            std::fill(cubit_vals.begin(), cubit_vals.end(), 0);

            // Per-shot RNG derived from master seed and shot index for reproducibility
            // ( — thread_seed() uses splitmix64 decorrelation).
            const std::uint64_t shot_seed =
                seeded ? qplayer::detail::thread_seed(seed,
                             static_cast<std::uint32_t>(shot))
                       : static_cast<std::uint64_t>(std::random_device{}());
            std::mt19937_64 shot_rng(shot_seed);

            for (const Op& op : circuit.ops()) {
                std::visit([&](auto&& node) {
                    using T = std::decay_t<decltype(node)>;

                    if constexpr (std::is_same_v<T, GateOp>) {
                        if (node.type == GateType::Reset) {
                            apply_reset_rng(node.qubits[0], shot_rng);
                        } else if (node.type == GateType::Opaque) {
                            throw SimulatorError{"SparseState",
                                "Opaque gate not supported. "
                                "Use FullState backend for opaque gates."};
                        } else {
                            apply_gate(node);
                        }

                    } else if constexpr (std::is_same_v<T, MeasureOp>) {
                        // Born-rule collapse with shot-scoped RNG.
                        cubit_vals[node.cubit] =
                            apply_measure_rng(node.qubit, shot_rng);

                    } else if constexpr (std::is_same_v<T, IfOp>) {
                        // Evaluate creg value and branch (same as
                        // FullState full_state_cpu.cpp:748-765 pattern).
                        const ClassicalRegister& reg = cregs[node.creg];
                        std::uint64_t regval = 0;
                        for (ClassicalBitIndex b = 0; b < reg.width; ++b) {
                            if (cubit_vals[reg.offset + b]) {
                                regval |= (std::uint64_t{1} << b);
                            }
                        }
                        if (regval == node.value) {
                            if (node.body.type == GateType::Reset) {
                                apply_reset_rng(node.body.qubits[0], shot_rng);
                            } else if (node.body.type == GateType::Opaque) {
                                throw SimulatorError{"SparseState",
                                    "Opaque gate in IfOp not supported. "
                                    "Use FullState backend for opaque gates."};
                            } else {
                                apply_gate(node.body);
                            }
                        }

                    } else if constexpr (std::is_same_v<T, IdleOp>) {
                        // IdleOp signals noise pipeline.
                        throw SimulatorError{"SparseState",
                            "IdleOp not supported "
                            "(noise unsupported on SparseState)"};

                    } else {
                        // BarrierOp: no state change.
                        (void)node;
                    }
                }, op);
                note_peak();  // track peak within this shot.
            }  // end op loop

            // Build bitstring from cubit_vals (MSB-first convention).
            std::string bs(total_c, '0');
            for (ClassicalBitIndex k = 0; k < total_c; ++k) {
                bs[k] = static_cast<char>('0' + cubit_vals[total_c - 1u - k]);
            }
            ++counts[bs];
        }  // end shot loop

        // Expose statevector from last shot when feasible.
        if (num_qubits_ > 0 && num_qubits_ <= MAX_DENSE_EXPORT_QUBITS) {
            result_set_statevector(r, get_statevector());
        }
    }

    // Empty circuit or zero cubits: ensure empty string key with nshots count.
    if (total_c == 0) {
        counts[""] = nshots;
    }

    result_set_counts(r, std::move(counts));
    result_set_shots(r, nshots);

    // baseline diagnostics for the per-shot path. peak_nonzero
    // reflects the last shot's evolution (init_state resets it each shot); the
    // target circuits take the fast path above, so per-shot is informational.
    if (diag_enabled_) {
        std::size_t ng = 0;
        for (const Op& dop : circuit.ops()) {
            if (std::holds_alternative<GateOp>(dop)) { ++ng; }
        }
        log::info("sparse_diag path=per_shot peak_nonzero=" +
                  std::to_string(peak_nonzero_) + " final_nonzero=" +
                  std::to_string(amplitudes_.size()) + " gates=" +
                  std::to_string(ng) + " qubits=" +
                  std::to_string(static_cast<unsigned>(num_qubits_)));
    }
    return r;
}

// ── set_seed — master PRNG seed for reproducible sampling ─────────

void SparseStateBackend::set_seed(std::uint64_t seed) {
    master_seed_ = seed;
    has_seed_    = true;  // any value, 0 included
}

// ── State access ───────────────────────────────────────────────────────

// Densifies the sparse amplitude map into an AoS StateVector.
// Only allowed when num_qubits_ <= MAX_DENSE_EXPORT_QUBITS to bound memory use.
StateVector SparseStateBackend::get_statevector() {
    if (num_qubits_ > MAX_DENSE_EXPORT_QUBITS) {
        throw SimulatorError{"SparseState",
            "get_statevector exceeds MAX_DENSE_EXPORT_QUBITS"};
    }
    // 2^num_qubits_ entries; StateIndex{1} << n uses the alias constructor form
    // (— no __uint128_t literal).
    const StateIndex dim = StateIndex{1} << num_qubits_;
    StateVector sv(static_cast<std::size_t>(dim), {0.0, 0.0});
    for (const auto& shard : amplitudes_.shards()) {
        for (const auto& [idx, amp] : shard) {
            sv[static_cast<std::size_t>(idx)] = amp;
        }
    }
    return sv;
}

// SparseState has no SoA (Structure of Arrays) buffer — permanently unavailable
// Not a stub: this is the final permanent behaviour.
RawStateView SparseStateBackend::get_statevector_raw() {
    throw SimulatorError{"SparseState",
        "raw SoA view not available (SparseState has no SoA buffer)"};
}

// expectation_value() — <psi|P|psi> for a Pauli string, computed by walking the
// stored non-zero entries only.
// Supported at every qubit count: unlike get_statevector() there is no
// MAX_DENSE_EXPORT_QUBITS densify cap, so a 100-qubit sparse state is fine.
// Semantics match FullStateCpuBackend::expectation_value() exactly — empty
// string means all-Z, Qiskit qubit order, I/X/Y/Z only.
double SparseStateBackend::expectation_value(std::string_view pauli) {
    if (num_qubits_ == 0 && amplitudes_.empty()) {
        throw SimulatorError{"SparseState",
            "expectation_value called before init_state"};
    }

    // An empty Pauli string defaults to all-Z over every qubit: <Z...Z>.
    // The local string outlives the view for the rest of this call.
    std::string all_z;
    if (pauli.empty()) {
        all_z.assign(static_cast<std::size_t>(num_qubits_), 'Z');
        pauli = all_z;
    }
    if (pauli.size() != static_cast<std::size_t>(num_qubits_)) {
        throw CircuitError{"expectation_value: Pauli string length (" +
                           std::to_string(pauli.size()) + ") != num_qubits (" +
                           std::to_string(num_qubits_) + ")"};
    }

    // flip_mask = X|Y qubits (P maps |j> to |j ^ flip_mask>); sign_mask = Y|Z
    // qubits (each contributes (-1)^bit); num_y feeds the global i^num_y phase.
    StateIndex flip_mask = StateIndex{0};
    StateIndex sign_mask = StateIndex{0};
    int        num_y     = 0;
    for (std::size_t k = 0; k < pauli.size(); ++k) {
        const QubitIndex q = num_qubits_ - 1 - static_cast<QubitIndex>(k);  // Qiskit order
        switch (pauli[k]) {
            case 'I': break;
            case 'X': flip_mask |= (StateIndex{1} << q); break;
            case 'Y': flip_mask |= (StateIndex{1} << q);
                      sign_mask |= (StateIndex{1} << q); ++num_y; break;
            case 'Z': sign_mask |= (StateIndex{1} << q); break;
            default:
                throw CircuitError{std::string("expectation_value: invalid Pauli char '") +
                                   pauli[k] + "' (only I/X/Y/Z allowed)"};
        }
    }

    // Global phase i^num_y from the Y factors (bit dependence folded into sign).
    cd phase{1.0, 0.0};
    switch (num_y & 3) {
        case 0: phase = cd{ 1.0,  0.0}; break;
        case 1: phase = cd{ 0.0,  1.0}; break;
        case 2: phase = cd{-1.0,  0.0}; break;
        case 3: phase = cd{ 0.0, -1.0}; break;
    }

    // Canonical StateIndex order keeps the floating-point sum bit-reproducible
    // across partition and thread counts, same basis as measurement.
    return pauli_expectation_sum(amplitudes_.sorted_entries(), amplitudes_,
                                 flip_mask, sign_mask, phase);
}

// nonzero_amplitudes() — return sorted_entries() directly.
// No densify, no MAX_DENSE_EXPORT_QUBITS cap — works up to 128 qubits.
// Only entries above threshold_ are stored, so every entry is nonzero by
// invariant (set_amplitude() prunes sub-threshold entries on insertion).
// Returns an empty vector before init_state() (amplitudes_ is empty).
std::vector<std::pair<StateIndex, std::complex<double>>>
SparseStateBackend::nonzero_amplitudes() const {
    return amplitudes_.sorted_entries();
}

// Single amplitude. A miss is zero by definition: entries
// below the truncation threshold were dropped, so "absent" and
// "negligible" are the same statement here.
std::complex<double> SparseStateBackend::amplitude(StateIndex basis) const {
    if (basis >= (StateIndex{1} << num_qubits())) {
        throw SimulatorError{"SparseState", "amplitude basis index out of range"};
    }
    return amplitudes_.get(basis);
}

// TensorNetwork-only.
std::vector<std::complex<double>>
SparseStateBackend::reduced_density_matrix(std::vector<QubitIndex>) const {
    throw SimulatorError{"SparseState",
        "reduced_density_matrix is TensorNetwork-only; partial-trace get_statevector() instead"};
}

// num_qubits() — active qubit count (0 before init_state). Used by
// dump_statevector() to format bitstring width without densifying.
QubitIndex SparseStateBackend::num_qubits() const {
    return num_qubits_;
}

// ── Metadata ──────────────────────────────────────────────────────────────────

SimType          SparseStateBackend::sim_type()       const { return SimType::SparseState; }
bool             SparseStateBackend::supports_noise() const { return false; }
bool             SparseStateBackend::supports_gpu()   const { return false; }
std::string_view SparseStateBackend::name()           const { return "SparseState"; }

// max_qubits() — report the structural 128-qubit ceiling. Sparse
// storage is bounded by non-zero count, not qubit count, so the dense-equivalent
// host bound is irrelevant here; whether a specific circuit fits memory is the
// feasibility analysis's job. The densify cap MAX_DENSE_EXPORT_QUBITS=25
// is a separate get_statevector() policy. ws is unused.
std::uint32_t SparseStateBackend::max_qubits(detail::WorkingSet /*ws*/) const noexcept {
    return kMaxSparseQubits;
}

}  // namespace qplayer
