/// @file result.cpp
/// @brief Phase 0-S trivial accessors for the Result container
///
/// (+). Backends populate the private members through the
/// `friend class QRegister` / `friend class Interactive` channels (Phase 5+);
/// the const accessors below simply expose those members.
///
/// Phase 0-S notes:
/// - has_error()/error_message() stay trivially false / "" under the immediate-
/// throw model. Continuation mode (M4+) may flip them later.
/// - has_statevector() reflects whether the optional was populated by the
/// backend; statevector() throws CircuitError when called without one.

#include "result.hpp"

#include "error.hpp"

#include "log.hpp"      // qplayer::log — QPLAYER_PROFILE_GPU sampling breakdown

#include <algorithm>   // std::upper_bound, std::max, std::fill
#include <chrono>      // std::chrono — QPLAYER_PROFILE_GPU section timing
#include <cmath>       // std::fma
#include <cstdlib>     // std::getenv
#include <iterator>    // std::distance
#include <memory>      // std::allocator_traits — uninit_allocator
#include <numeric>     // std::partial_sum
#include <random>      // std::mt19937_64, std::uniform_real_distribution
#include <string>      // std::stoull, std::to_string — env threshold parse
#include <vector>      // std::vector — GPU uniform/basis buffers

namespace {

// uninit_allocator<T>: skips value-initialization for large prob arrays in the
// parallel CDF path. Every element is overwritten by the parallel
// prob-fill loop before it is read, so zero-init is pure waste. Avoid the ~480ms
// page-fault cost that std::vector::resize() induces on first access of a
// fresh 1-GiB allocation (2^27 doubles). Only used for the large-circuit path
// (dimi > kParallelSampleThreshold); the small-circuit serial path keeps the
// default allocator (resize zero-inits, harmless at small element counts).
template <typename T>
struct uninit_allocator : public std::allocator<T> {
    using base = std::allocator<T>;
    // Define these directly instead of inheriting them: std::allocator<T>::pointer
    // and ::size_type are deprecated in C++17 and REMOVED in C++20, so the
    // inherited form compiles today only because CMakeLists pins C++17 and would
    // break the moment the standard is raised (audit #24).
    using value_type = T;
    using pointer    = T*;
    using size_type  = std::size_t;

    template <typename U>
    struct rebind { using other = uninit_allocator<U>; };

    // Suppress default-construct (no zero-fill) — each element is written before read.
    template <typename U>
    void construct(U*) noexcept {}

    // Forward non-default constructs (copy/move) to the standard allocator.
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        std::allocator_traits<base>::construct(
            static_cast<base&>(*this), p, std::forward<Args>(args)...);
    }
};

}  // anonymous namespace

#if defined(QPLAYER_BUILD_GPU)
// GPU-resident sampling launchers (full_state_gpu.cu). Declared here
// so result.cpp draws on-device without including cuda_runtime.h (one-way
// include policy). cudaError_t returned as int (0 == success).
extern "C" {
int qplayer_cuda_sample_counts(const double* d_re, const double* d_im,
                               unsigned long long dim,
                               const double* h_uniforms,
                               unsigned long long nshots,
                               unsigned long long* h_out_basis);
int qplayer_cuda_download_state(double* h_re, double* h_im,
                                const double* d_re, const double* d_im,
                                unsigned long long dim);
}  // extern "C"
#endif

namespace qplayer {

const std::map<std::string, std::uint64_t>& Result::counts() const {
    // Lazy sampling: the FullState CPU fast path defers the nshots
    // draw to here. Draw once on first access, then serve the cached map.
    if (lazy_.has_value() && !sampled_) {
        perform_lazy_sampling();
        sampled_ = true;
    }
    return counts_;
}

// perform_lazy_sampling() — inverse-CDF draw deferred from a backend fast path.
// Mirrors the eager fast-path samplers exactly (same probability formula, RNG
// stream, and MSB-first bitstring) so a fixed seed reproduces identical counts
// (Golden invariance). Two modes (revised):
//   - sparse (LazySampling::sparse_probs non-empty): draw from the sparse CDF,
//     basis index taken from the sparse entry (SparseState; no densify).
//   - dense  (sparse_probs empty): build prob/CDF over statevector_ (FullState
//     CPU/GPU). std::fma matches full_state_cpu.cpp's eager kernel bit-for-bit.
void Result::perform_lazy_sampling() const {
    const LazySampling& ls = *lazy_;
    const ClassicalBitIndex total_c = ls.total_cubits;

    // sampling breakdown: time prob/CDF build vs the nshots
    // draw. Gated on QPLAYER_PROFILE_GPU so it is free in production.
    const bool prof = std::getenv("QPLAYER_PROFILE_GPU") != nullptr;
    auto prof_mark = std::chrono::steady_clock::now();
    const auto prof_lap = [&prof, &prof_mark](const char* section) {
        if (!prof) return;
        const auto now = std::chrono::steady_clock::now();
        qplayer::log::info(
            std::string{"[profile-gpu] sampling "} + section + " = " +
            std::to_string(
                std::chrono::duration<double, std::milli>(now - prof_mark).count()) +
            " ms");
        prof_mark = now;
    };

    // Empty classical space: single empty-string key with all shots.
    if (total_c == 0) {
        counts_[""] = ls.nshots;
        return;
    }

#if defined(QPLAYER_BUILD_GPU)
    // GPU-resident sampling: |psi> stays in VRAM. Generate the same
    // mt19937_64(seed) uniform stream as the CPU path (parity), draw
    // basis indices on-device (prob/CDF/inverse-CDF), and build bitstrings here.
    // Avoids the 2GiB D2H + AoS materialization for counts-only workloads.
    if (has_gpu_resident()) {
        std::vector<double> uniforms(ls.nshots);
        std::mt19937_64 rng(ls.seed);
        std::uniform_real_distribution<double> uniform01(0.0, 1.0);
        for (std::uint32_t s = 0; s < ls.nshots; ++s) uniforms[s] = uniform01(rng);

        std::vector<unsigned long long> basis(ls.nshots);
        const int rc = qplayer_cuda_sample_counts(
            gpu_re_, gpu_im_, static_cast<unsigned long long>(gpu_dim_),
            uniforms.data(), ls.nshots, basis.data());
        if (rc != 0) {
            throw SimulatorError{"FullStateGPU",
                "GPU-resident sampling failed (cuda error " + std::to_string(rc) + ")"};
        }
        prof_lap("gpu-device-sample");

        std::vector<int> cubit_vals(total_c, 0);
        for (std::uint32_t s = 0; s < ls.nshots; ++s) {
            const StateIndex basis_idx = static_cast<StateIndex>(basis[s]);
            std::fill(cubit_vals.begin(), cubit_vals.end(), 0);
            for (const auto& qc : ls.measure_map) {
                cubit_vals[qc.second] =
                    static_cast<int>((basis_idx >> qc.first) & StateIndex{1});
            }
            std::string bs(total_c, '0');
            for (ClassicalBitIndex k = 0; k < total_c; ++k) {
                bs[k] = static_cast<char>('0' + cubit_vals[total_c - 1u - k]);
            }
            ++counts_[bs];
        }
        return;
    }
#endif

    // CDF + a basis-index lookup, populated for whichever mode is active. For the
    // sparse mode `basis_of[k]` maps the k-th CDF slot to its StateIndex; for the
    // dense mode the slot index *is* the basis index.
    //
    // cdf_scale: multiplier applied to each uniform draw u before upper_bound.
    //   - sparse path and small dense path: normalised CDF -> scale = 1.0.
    // - large dense path: unnormalised CDF -> scale = total probability sum.
    // u * 1.0 == u in IEEE 754, so the sparse/small paths are bit-exact.
    //
    // uninit_allocator: skips zero-init for the large-circuit dense path so that
    // the parallel fill loop handles first-touch page faults in parallel.
    // std::upper_bound, std::partial_sum, and assign() all work through iterators
    // and are allocator-agnostic.
    using CdfVec = std::vector<double, uninit_allocator<double>>;
    CdfVec                  cdf;
    double                  cdf_scale = 1.0;
    const bool sparse = !ls.sparse_probs.empty();

    // Large dense path 3-level draw (supersedes): we DROP the
    // 2^n local-CDF array entirely. Pass 1 keeps only `block_total[b]` (2^16 blocks,
    // bit-exact vs) plus `checkpoints` — the running CDF value at the start
    // of each 4096-element sub-block. The draw locates the block (`cumboundary`),
    // then the sub-block (`checkpoints`, <=16/block), then RE-SCANS that <=4096
    // window from re/im to find the exact index. Re-scan is bounded to nshots*4096
    // elements (distribution-independent), so the 1T CDF write becomes a ~T/512
    // checkpoint write + negligible re-read -> ~3T -> ~2T traffic. Bit-exact:
    // checkpoints are intermediate values of the same sequential block scan, and the
    // window re-scan continues from that exact running acc (no fresh-from-0 sum), so
    // each global CDF value matches bit-for-bit (IEEE add order preserved).
    bool                     large_subblock  = false;
    std::size_t              draw_block      = 0;     // kScanBlock (2^16) for the large path
    std::vector<double>      cumboundary;             // cumboundary[b] = global CDF at block b end
    std::vector<double>      block_off_draw;          // block_off[b] = global CDF at block b start
    std::vector<double>      checkpoints;             // ck[b*subs+s] = running acc at sub-block s start
    constexpr std::size_t    kSubBlock = std::size_t{1} << 12;  // 4096 — re-scan window

    if (sparse) {
        // Sparse: probs are already normalised (SparseState eager parity).
        cdf.resize(ls.sparse_probs.size());
        double acc = 0.0;
        for (std::size_t k = 0; k < ls.sparse_probs.size(); ++k) {
            acc += ls.sparse_probs[k].second;
            cdf[k] = acc;
        }
    } else {
        // Dense CDF build. Two sub-paths (+):
        //
        // CPU-resident (P2): read prob[i] directly from the SoA pointers
        // cpu_re_/cpu_im_ without first building an AoS copy. Same double values,
        // same index order as the AoS path -> bit-exact parity (Golden invariance).
        //
        // AoS fallback (statevector_ populated, or GPU-resident after materialize):
        // uses the existing statevector_ complex array path.
        //
        // Size-gated parallel CDF (revised 2026-06-16):
        // - dimi <= kParallelSampleThreshold: serial path (bit-exact Golden).
        //   - dimi >  kParallelSampleThreshold: deterministic block inclusive scan
        // (3-phase OpenMP) — single logical pass, no prob_sum/normalize.
        //
        // DEFAULT IS PARALLEL for dimi > 2^18 (revised 2026-06-16). The value
        // of the parallel block-scan is CONTENTION RESILIENCE, not raw throughput:
        // the serial CDF is single-threaded, so on a busy/shared host a 1-GiB+ scan
        // gets starved by preemption (measured q27 serial 148ms quiet -> 1357ms under
        // load ~9), whereas the 144-thread parallel scan stays ~145ms regardless of
        // load. On a quiet host serial ≈ parallel (~150ms at 2^27); under contention
        // parallel is ~9x faster. So parallel is never worse and far better when busy
        // -> it is the robust default for large circuits. (The earlier "default OFF"
        // revision was measured on a quiet host only and missed this resilience.)
        // The env=0 force-parallel pitfall: 0 <= 1024 is rejected by the guard below;
        // force parallel with a value in (1024, dim), e.g. 2048.
        //
        // kParallelSampleThreshold: state count above which the parallel block-scan
        // CDF path is used. Default = 2^18 (262144) -> circuits >= ~18 qubits go
        // parallel (where serial-under-load starvation matters); smaller circuits stay
        // serial (cheap regardless). Tunable via QPLAYER_PARALLEL_SAMPLE_THRESHOLD
        // (decimal state count; raise to force serial, e.g. 2^40). Must stay > 1024 to
        // keep Golden circuits (max Clifford 1024 states) on the serial bit-exact path
        //
        //
        // Block size kScanBlock = 2^16 is a fixed compile-time constant so the block
        // boundaries depend only on dimi, not on OMP thread count -> deterministic
        // across all thread counts (reproducibility).
        static const StateIndex kParallelSampleThreshold = []() -> StateIndex {
            // Resolve once; subsequent calls read the static value.
            constexpr StateIndex kDefaultThreshold = StateIndex{1} << 18;  // parallel for >= ~18 qubits (contention resilience); env-tunable
            constexpr StateIndex kMinThreshold     = StateIndex{1} << 10;  // 1024 — must stay > Golden max
            constexpr StateIndex kMaxThreshold     = StateIndex{1} << 40;  // upper bound for an env override
            const char* env = std::getenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD");
            if (env == nullptr) return kDefaultThreshold;
            // Parse as unsigned long long; fall back to default on error.
            try {
                const unsigned long long v = std::stoull(std::string{env});
                const StateIndex sv = static_cast<StateIndex>(v);
                if (sv <= kMinThreshold || sv > kMaxThreshold) {
                    qplayer::log::warn(
                        "QPLAYER_PARALLEL_SAMPLE_THRESHOLD value out of range "
                        "[1025, 2^40]; using default " +
                        std::to_string(static_cast<unsigned long long>(kDefaultThreshold)));
                    return kDefaultThreshold;
                }
                return sv;
            } catch (...) {
                qplayer::log::warn(
                    "QPLAYER_PARALLEL_SAMPLE_THRESHOLD parse error; "
                    "using default " +
                    std::to_string(static_cast<unsigned long long>(kDefaultThreshold)));
                return kDefaultThreshold;
            }
        }();
        constexpr StateIndex kScanBlock = StateIndex{1} << 16;  // fixed block -> deterministic

        std::size_t dimi = 0;

        // For the large-circuit parallel path, we need prob without zero-init
        // (uninit_allocator<double>) so that the parallel fill loop handles the
        // first-touch page-fault in parallel, avoiding the ~480ms serial zero-init
        // cost of std::vector::resize() on a fresh 1-GiB allocation. The small-
        // circuit serial path uses an ordinary std::vector<double>. We determine
        // dimi first, then branch on the threshold to allocate accordingly.

        if (has_cpu_resident()) {
            dimi = static_cast<std::size_t>(cpu_dim_);
        } else {
            // A SparseState run whose amplitudes were all pruned (S == 0, reachable
            // with QPLAYER_SPARSE_THRESHOLD near 1.0) leaves sparse_probs empty, so
            // `sparse` above is false and we land on this dense branch — but for
            // n > MAX_DENSE_EXPORT_QUBITS the backend never attached a statevector,
            // so this used to dereference an empty optional (audit #19).
            if (!statevector_.has_value()) {
                throw CircuitError{
                    "counts(): no sampling source available — the backend attached "
                    "neither a state vector nor a sparse probability snapshot "
                    "(all amplitudes pruned? see QPLAYER_SPARSE_THRESHOLD)"};
            }
            dimi = statevector_->size();
        }

        if (static_cast<StateIndex>(dimi) <= kParallelSampleThreshold) {
            // ── Small circuit: serial path ─────────────────────────────
            // Bit-exact for Golden 4 types (Bell/GHZ/QFT/Clifford, all <= 1024 states).
            // resize() zero-init is negligible at <= 65536 elements.
            std::vector<double> prob(dimi);

            if (has_cpu_resident()) {
                const double* src_re = cpu_re_;
                const double* src_im = cpu_im_;
                const std::int64_t n = static_cast<std::int64_t>(dimi);
#pragma omp parallel for schedule(static)
                for (std::int64_t i = 0; i < n; ++i) {
                    const double re = src_re[i];
                    const double im = src_im[i];
                    prob[static_cast<std::size_t>(i)] = std::fma(re, re, im * im);
                }
            } else {
                const StateVector& sv = *statevector_;
                const std::complex<double>* svp = sv.data();
                const std::int64_t n = static_cast<std::int64_t>(dimi);
#pragma omp parallel for schedule(static)
                for (std::int64_t i = 0; i < n; ++i) {
                    const double re = svp[i].real();
                    const double im = svp[i].imag();
                    prob[static_cast<std::size_t>(i)] = std::fma(re, re, im * im);
                }
            }
            prof_lap("dense-prob");

            double prob_sum = 0.0;
            for (std::size_t i = 0; i < dimi; ++i) { prob_sum += prob[i]; }
            prof_lap("dense-prob_sum");
            if (prob_sum > 0.0) {
                // Divide (not multiply by reciprocal) to match the original /= bit-exactly.
                const std::int64_t ndimi = static_cast<std::int64_t>(dimi);
#pragma omp parallel for schedule(static)
                for (std::int64_t i = 0; i < ndimi; ++i) { prob[static_cast<std::size_t>(i)] /= prob_sum; }
            }
            prof_lap("dense-normalize");
            // In-place prefix scan -> normalised CDF. Serial — bit-exact.
            std::partial_sum(prob.begin(), prob.end(), prob.begin());
            // cdf uses uninit_allocator; assign() copies the result (negligible for
            // <= 65536 elements) — std::move requires identical allocator types.
            cdf.assign(prob.begin(), prob.end());
            prof_lap("dense-partial_sum");
            // cdf_scale stays 1.0: upper_bound(cdf, u*1.0) == upper_bound(cdf, u).
        } else {
            // ── Large circuit (dimi > 65536): fused prob+scan, two-level draw ────
            // (supersedes large path) The old path swept the 2^n
            // prob array three times: a fill pass + Phase-1 local scan + Phase-3
            // global-offset add. We fuse fill+scan into ONE pass and DROP Phase 3:
            //   - Fused pass: prob[i]=re^2+im^2 AND per-block local inclusive scan
            //     in a single sweep over the source (re+im read, prob write).
            //   - No Phase 3: `prob` keeps the per-block LOCAL scan; the small
            //     block_off/cumboundary arrays (nblocks entries, L1-resident) carry
            //     the block prefix so the draw never needs the global CDF.
            // The local CDF array is retained so the draw can BINARY-search within a
            // block (O(log) per shot) — recomputing the block on the fly would be
            // O(block) per shot and pathological for concentrated distributions.
            // Bit-identical to the old large path (IEEE add commutes); block
            // boundaries are (dimi, kScanBlock)-only -> thread-count-independent
            // determinism. No prob_sum/normalize pass (P5-b).
            //
            // uninit_allocator skips zero-init: the fused loop writes every element
            // before it is read, avoiding the ~480ms page-fault cost of a fresh
            // 1-GiB std::vector::resize(); first-touch is parallel (NUMA-friendly).
            // : NO 2^n CDF array. Pass 1 keeps block_total[b] (bit-exact vs
            // : same 2^16 blocks, same fma order) + checkpoints — the running
            // acc at each 4096 sub-block start. The draw re-scans the located <=4096
            // window instead of binary-searching a materialised 2^n CDF. Traffic
            // 3T -> ~2T (the 1T CDF write is gone; checkpoints are ~T/512).
            const std::size_t block   = static_cast<std::size_t>(kScanBlock);   // 2^16
            const std::size_t nblocks = (dimi + block - 1u) / block;
            const std::size_t subs    = block / kSubBlock;                      // 16
            std::vector<double> block_total(nblocks);
            std::vector<double> ck(nblocks * subs);
            const std::int64_t nb = static_cast<std::int64_t>(nblocks);

            // Per block: sequential scan in sub-block steps; record running acc at
            // each sub-block start (ck), accumulate within the sub-block. The acc
            // sequence is identical to 's per-element scan, so ck[] holds the
            // exact intermediate CDF values -> window re-scan reproduces them bit-for-bit.
            const bool cpu_res = has_cpu_resident();
            const double* src_re = cpu_res ? cpu_re_ : nullptr;
            const double* src_im = cpu_res ? cpu_im_ : nullptr;
            const std::complex<double>* svp = cpu_res ? nullptr : statevector_->data();
#pragma omp parallel for schedule(static)
            for (std::int64_t b = 0; b < nb; ++b) {
                const std::size_t lo     = static_cast<std::size_t>(b) * block;
                const std::size_t blk_hi = (lo + block < dimi) ? lo + block : dimi;
                const std::size_t base   = static_cast<std::size_t>(b) * subs;
                double acc = 0.0;
                for (std::size_t s = 0; s < subs; ++s) {
                    ck[base + s] = acc;  // running acc at this sub-block's first index
                    const std::size_t slo = lo + s * kSubBlock;
                    if (slo >= blk_hi) continue;  // tail sub-block past the state end
                    const std::size_t shi = (slo + kSubBlock < blk_hi) ? slo + kSubBlock : blk_hi;
                    for (std::size_t i = slo; i < shi; ++i) {
                        const double re = cpu_res ? src_re[i] : svp[i].real();
                        const double im = cpu_res ? src_im[i] : svp[i].imag();
                        acc += std::fma(re, re, im * im);
                    }
                }
                block_total[static_cast<std::size_t>(b)] = acc;
            }
            prof_lap("dense-blocktotal-checkpoints");

            // Serial exclusive prefix of block totals (nblocks entries — small).
            // block_off[b] = global CDF at block b start; cumboundary[b] = at end.
            std::vector<double> block_off(nblocks);
            std::vector<double> cumb(nblocks);
            block_off[0] = 0.0;
            cumb[0] = block_total[0];
            for (std::size_t b = 1; b < nblocks; ++b) {
                block_off[b] = block_off[b - 1u] + block_total[b - 1u];
                cumb[b] = block_off[b] + block_total[b];
            }

            // Total = global CDF at the last block end (== sum of all probs).
            cdf_scale      = cumb[nblocks - 1u];   // draw target = u * total
            large_subblock = true;
            draw_block     = block;
            cumboundary    = std::move(cumb);
            block_off_draw = std::move(block_off);
            checkpoints    = std::move(ck);
            prof_lap("dense-blockprefix");
        }
    }
    if (sparse) prof_lap("sparse-cdf");

    if (large_subblock) {
        // ── Large-circuit 3-level draw ───────────────────────────────
        // Draw all u in shot order from the same RNG stream -> identical multiset ->
        // bit-exact counts vs (counts are order-invariant, so no sort is
        // needed). Per shot: ① block via cumboundary, ② sub-block via checkpoints
        // (<=16/block), ③ re-scan the located <=4096 window from re/im seeded with
        // the exact running acc. Each shot's window is independently bounded, so the
        // index resolution is OMP-parallel; the std::map histogram below stays serial.
        std::mt19937_64 rng(ls.seed);
        std::uniform_real_distribution<double> uniform01(0.0, 1.0);
        const std::size_t nshots = static_cast<std::size_t>(ls.nshots);
        std::vector<double> targets(nshots);
        for (std::size_t s = 0; s < nshots; ++s) targets[s] = uniform01(rng) * cdf_scale;

        const bool cpu_res = has_cpu_resident();
        const double* src_re = cpu_res ? cpu_re_ : nullptr;
        const double* src_im = cpu_res ? cpu_im_ : nullptr;
        const std::complex<double>* svp = cpu_res ? nullptr : statevector_->data();
        const std::size_t subs = draw_block / kSubBlock;
        const std::size_t dimi = cpu_res ? static_cast<std::size_t>(cpu_dim_)
                                         : statevector_->size();

        std::vector<StateIndex> basis(nshots);
        const std::int64_t nshots_i = static_cast<std::int64_t>(nshots);
#pragma omp parallel for schedule(static)
        for (std::int64_t t = 0; t < nshots_i; ++t) {
            const double target = targets[static_cast<std::size_t>(t)];
            // ① block: first whose end-boundary exceeds target.
            const auto bit = std::upper_bound(cumboundary.begin(), cumboundary.end(), target);
            std::size_t b = static_cast<std::size_t>(std::distance(cumboundary.begin(), bit));
            if (b >= cumboundary.size()) b = cumboundary.size() - 1u;  // clamp (u<1 -> target<total)
            const double tib = target - block_off_draw[b];             // local target within block
            // ② sub-block: ck[base+s] = running acc at sub-block s start. Advance while
            //    the END cumulative of sub-block s (= ck[base+s+1]) <= tib. The guard
            //    s+1<subs keeps us inside this block (the last sub-block needs no ck).
            const std::size_t base = b * subs;
            std::size_t s = 0;
            while (s + 1u < subs && checkpoints[base + s + 1u] <= tib) ++s;
            // ③ window re-scan: seed acc with the exact running value at sub-block start
            // so the per-element acc matches 's CDF bit-for-bit.
            const std::size_t blk_lo = b * draw_block;
            const std::size_t blk_hi = (blk_lo + draw_block < dimi) ? blk_lo + draw_block : dimi;
            const std::size_t wlo = blk_lo + s * kSubBlock;
            std::size_t whi = wlo + kSubBlock;
            if (whi > blk_hi) whi = blk_hi;
            double acc = checkpoints[base + s];
            std::size_t found = (whi > wlo) ? whi - 1u : wlo;  // default last (target≈total)
            for (std::size_t i = wlo; i < whi; ++i) {
                const double re = cpu_res ? src_re[i] : svp[i].real();
                const double im = cpu_res ? src_im[i] : svp[i].imag();
                acc += std::fma(re, re, im * im);
                if (acc > tib) { found = i; break; }
            }
            basis[static_cast<std::size_t>(t)] = static_cast<StateIndex>(found);
        }

        // Serial histogram (std::map not thread-safe; nshots is small).
        std::vector<int> cubit_vals(total_c, 0);
        for (std::size_t s = 0; s < nshots; ++s) {
            const StateIndex basis_idx = basis[s];
            std::fill(cubit_vals.begin(), cubit_vals.end(), 0);
            for (const auto& qc : ls.measure_map) {
                cubit_vals[qc.second] =
                    static_cast<int>((basis_idx >> qc.first) & StateIndex{1});
            }
            std::string bs(total_c, '0');
            for (ClassicalBitIndex k = 0; k < total_c; ++k) {
                bs[k] = static_cast<char>('0' + cubit_vals[total_c - 1u - k]);
            }
            ++counts_[bs];
        }
        prof_lap("draw");
        return;
    }

    // ── Small-dense + sparse per-shot draw (unchanged) ──────
    std::mt19937_64 rng(ls.seed);
    std::uniform_real_distribution<double> uniform01(0.0, 1.0);

    std::vector<int> cubit_vals(total_c, 0);
    for (std::uint32_t shot = 0; shot < ls.nshots; ++shot) {
        const double u      = uniform01(rng);
        const double target = u * cdf_scale;  // scale=1.0 for small/sparse normalised CDF
        const auto it = std::upper_bound(cdf.begin(), cdf.end(), target);
        const std::size_t ki = static_cast<std::size_t>(
            std::max(std::ptrdiff_t{0}, std::distance(cdf.begin(), it)));
        StateIndex basis_idx;
        if (sparse) {
            basis_idx = (ki < ls.sparse_probs.size())
                            ? ls.sparse_probs[ki].first
                            : ls.sparse_probs.back().first;
        } else {
            basis_idx = static_cast<StateIndex>(ki);
        }

        std::fill(cubit_vals.begin(), cubit_vals.end(), 0);
        for (const auto& qc : ls.measure_map) {
            cubit_vals[qc.second] =
                static_cast<int>((basis_idx >> qc.first) & StateIndex{1});
        }

        std::string bs(total_c, '0');
        for (ClassicalBitIndex k = 0; k < total_c; ++k) {
            bs[k] = static_cast<char>('0' + cubit_vals[total_c - 1u - k]);
        }
        ++counts_[bs];
    }
    prof_lap("draw");
}

std::uint64_t Result::shots() const noexcept {
    return shots_;
}

bool Result::has_statevector() const noexcept {
    // GPU-resident and CPU-resident states count as available — they materialise
    // on first statevector() access (deferred AoS).
    return statevector_.has_value() || has_gpu_resident() || has_cpu_resident();
}

const StateVector& Result::statevector() const {
    if (!statevector_.has_value()) {
        // GPU-resident path: deferred D2H on first access.
        if (has_gpu_resident()) {
            materialize_gpu_statevector();
        // CPU-resident path: deferred SoA->AoS conversion.
        } else if (has_cpu_resident()) {
            materialize_cpu_statevector();
        } else {
            throw CircuitError{"Result::statevector() called but no state vector is attached"};
        }
    }
    return *statevector_;
}

// materialize_gpu_statevector() — deferred D2H + AoS for the GPU-resident path
// Downloads the device SoA state held by gpu_owner_ into a host AoS
// statevector_ on first statevector()/get_statevector_raw() access. The device
// buffer is kept alive by gpu_owner_ (shared with the backend), so this is safe
// even after the backend's next run() allocated a fresh buffer.
void Result::materialize_gpu_statevector() const {
#if defined(QPLAYER_BUILD_GPU)
    const std::size_t dimi = static_cast<std::size_t>(gpu_dim_);
    std::vector<double> h_re(dimi), h_im(dimi);
    const int rc = qplayer_cuda_download_state(
        h_re.data(), h_im.data(), gpu_re_, gpu_im_,
        static_cast<unsigned long long>(gpu_dim_));
    if (rc != 0) {
        throw SimulatorError{"FullStateGPU",
            "deferred GPU state download failed (cuda error " + std::to_string(rc) + ")"};
    }
    StateVector sv(dimi);
    for (std::size_t i = 0; i < dimi; ++i) {
        sv[i] = std::complex<double>(h_re[i], h_im[i]);
    }
    statevector_ = std::move(sv);
#else
    throw CircuitError{"Result::statevector(): GPU-resident state but GPU build disabled"};
#endif
}

// materialize_cpu_statevector() — deferred SoA->AoS conversion for the
// CPU-resident path (mirrors materialize_gpu_statevector). Called
// lazily on first statevector()/get_statevector_raw() access when the FullState
// CPU fast path hand-off used result_set_cpu_resident() instead of an eager
// get_statevector() call. Converts the host SoA (cpu_re_/cpu_im_) into the AoS
// statevector_ cache; subsequent calls return the cached value.
void Result::materialize_cpu_statevector() const {
    if (!has_cpu_resident()) {
        throw CircuitError{"Result: materialize_cpu_statevector called without CPU-resident state"};
    }
    const std::size_t dimi = static_cast<std::size_t>(cpu_dim_);
    StateVector sv(dimi);
    for (std::size_t i = 0; i < dimi; ++i) {
        sv[i] = std::complex<double>(cpu_re_[i], cpu_im_[i]);
    }
    statevector_ = std::move(sv);
}

bool Result::has_expectation(const std::string& pauli) const {
    return expectations_.find(pauli) != expectations_.end();
}

double Result::expectation(const std::string& pauli) const {
    auto it = expectations_.find(pauli);
    if (it == expectations_.end()) {
        throw CircuitError{"Result::expectation: Pauli string not registered"};
    }
    return it->second;
}

const std::map<std::string, double>& Result::all_expectations() const noexcept {
    return expectations_;
}

bool Result::has_error() const noexcept {
    // Under immediate-throw model the result is never reached with an
    // error flag set. Kept for the Python binding's stable ABI shape.
    return !error_message_.empty();
}

const std::string& Result::error_message() const noexcept {
    return error_message_;
}

bool Result::has_truncation_errors() const noexcept {
    return !truncation_errors_.empty();
}

const std::vector<double>& Result::truncation_errors() const noexcept {
    return truncation_errors_;
}

bool Result::has_trajectory_diagnostics() const noexcept {
    return !bond_dimensions_.empty();
}

const std::vector<std::uint32_t>& Result::bond_dimensions() const noexcept {
    return bond_dimensions_;
}

const std::vector<double>& Result::truncation_totals() const noexcept {
    return truncation_totals_;
}

const std::vector<double>& Result::norm_drifts() const noexcept {
    return norm_drifts_;
}

const std::vector<std::vector<NoiseDrawEvent>>& Result::noise_draw_ledger() const noexcept {
    return noise_draw_ledger_;
}

const std::vector<std::uint32_t>& Result::noise_interventions() const noexcept {
    return noise_interventions_;
}

}  // namespace qplayer
