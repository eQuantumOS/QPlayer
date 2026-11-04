// full_state_cpu.cpp — FullState CPU backend: SoA state vector + gate kernels.
// : aligned alloc, init_state, get_statevector (AoS conversion),
//       get_statevector_raw, prepare (memory budget guard), apply_op dispatcher.
// : gate matrix derivation, apply_gate with 1Q/2Q/3Q unified kernel,
//       AVX2+AVX-512 SIMD paths via __attribute__((target(...))),
// NaN/Inf + norm validation helper.
// : expectation_value (Pauli string, Qiskit qubit order).
// : apply_measure (Born-rule collapse), apply_reset, execute with
// fast path (no classical control) and per-shot path (IfOp present).
// set_seed() for reproducible sampling.
// : L2-tiled cache blocking on 1Q/2Q gate kernels. Blocked path chosen
//       when cache_blocking_enabled_==true (default). Toggle via
// set_cache_blocking() or env QPLAYER_CACHE_BLOCKING=0 (benchmark).
//       Blocked path and unblocked path are numerically identical (same FMA3
//       body); the only difference is the traversal order of state indices.
// NOTE: runtime Gate Fusion produces 3Q/4Q fused unitaries that
// bypass the 1Q/2Q cache-blocked path — trade-off,.

#include "backend/full_state/full_state_backend.hpp"
#include "backend/full_state/cpu_dispatcher.hpp"
#include "backend/full_state/gate_fusion.hpp"
#include "backend/full_state/gate_matrix.hpp"
#include "backend/full_state/kraus_apply_2q.hpp"   // detail::apply_kraus_2q

#include <algorithm>    // std::lower_bound
#include <atomic>       // std::atomic<bool> for OMP error capture
#include <cmath>        // std::sin, std::cos, std::fma, std::isfinite, M_PI, M_SQRT1_2, std::sqrt
#include <cstdlib>      // std::aligned_alloc, std::free, std::getenv
#include <cstring>      // std::memset, std::strcmp
#include <complex>
#include <map>
#include <mutex>        // std::mutex for thread-safe counts merge
#include <numeric>      // std::partial_sum
#include <random>       // std::mt19937_64, std::uniform_real_distribution, std::random_device
#include <string>
#include <utility>      // std::pair, std::move
#include <variant>
#include <vector>

#include "backend/memory_check.hpp"
#include "circuit/circuit.hpp"
#include "detail/rng.hpp"
#include "error.hpp"
#include "noise/noise_model.hpp"

// Compile-time cache block size (detect_cache.cmake).
// Fallback guard: conservative 4096 doubles per block (= 32 KiB re+im, fits
// inside a 256 KiB L2 with headroom for control structures).
#ifndef QPLAYER_CACHE_BLOCK_SIZE
#define QPLAYER_CACHE_BLOCK_SIZE 4096
#endif

#ifndef QPLAYER_L3_THRESHOLD_QUBITS
#define QPLAYER_L3_THRESHOLD_QUBITS 24
#endif

namespace qplayer {

namespace {

// ── Aligned allocator ────────────────────────────────────────────────────────

// Allocate a 64-byte-aligned array of `count` doubles WITHOUT zero-initialising.
// 64B alignment satisfies both AVX2 (32B) and AVX-512 (64B) requirements
//
// Zero-initialisation is deferred to init_state()'s OpenMP parallel-for (P1,
//) which also touches padding bytes to prevent uninitialized reads by
// SIMD kernels that over-touch the trailing alignment pad.
// padded_count is returned via the out-parameter so init_state() can zero the
// full padded region including alignment tail without duplicating the calc.
std::unique_ptr<double[], CpuStateBuffers::AlignedDeleter>
alloc_aligned(StateIndex count, std::size_t* out_padded_count = nullptr) {
    const std::size_t align        = 64;
    const std::size_t raw_bytes    = static_cast<std::size_t>(count) * sizeof(double);
    const std::size_t padded_bytes = (raw_bytes + align - 1) & ~(align - 1);
    const std::size_t padded_count = padded_bytes / sizeof(double);

    void* ptr = std::aligned_alloc(align, padded_bytes);
    if (!ptr) {
        throw SimulatorError{"FullState", "aligned_alloc failed (out of memory)"};
    }
    if (out_padded_count) { *out_padded_count = padded_count; }
    return std::unique_ptr<double[], CpuStateBuffers::AlignedDeleter>{
        static_cast<double*>(ptr)};
}

// Import Mat2x2 and gate_matrix() from the shared header (extraction).
// Using-declarations keep all existing call sites unchanged: gate_matrix(...)
// and Mat2x2{...} continue to compile without modification.
using qplayer::detail::Mat2x2;
using qplayer::detail::gate_matrix;
using qplayer::detail::ecr_matrix_data;
using qplayer::detail::iswap_matrix_data;
using qplayer::detail::sqrt_iswap_matrix_data;
using qplayer::detail::fsim_matrix_data;

// ── Unified controlled-1Q kernel (AVX2 path) ─────────────────────────────────
// Applies 2x2 unitary U to the `target` qubit; in the subspace where every
// bit in `control_mask` is 1.  control_mask==0 means unconditional (pure 1Q).
// For control_mask==0, a block-stride scan enables contiguous inner-j access
// (AVX2/omp-simd vectorisable).  For non-zero masks a flat parallel-for is used
// because the scatter-gather pattern defeats SIMD auto-vectorisation.
// re/im must be 64-byte aligned (alloc_aligned).
__attribute__((target("avx2,fma")))
void apply_controlled_1q_avx2(
        double* __restrict__ re, double* __restrict__ im,
        StateIndex dim,
        QubitIndex target,
        StateIndex control_mask,
        const Mat2x2& U)
{
    const StateIndex tbit   = StateIndex{1} << target;
    const StateIndex stride = tbit;  // half-period in the block-stride pattern

    if (control_mask == 0) {
        // Pure 1-qubit: parallelise over independent 2*stride-sized groups; the
        // inner contiguous run of `stride` amplitude pairs is the SIMD lane.
        // One OMP parallel region covers all groups (no per-block fork/join),
        // eliminating the O(dim/stride) fork overhead of the old per-block layout
        //
        const std::int64_t ngroups = static_cast<std::int64_t>(dim >> (target + 1));
#pragma omp parallel for schedule(static)
        for (std::int64_t g = 0; g < ngroups; ++g) {
            const StateIndex base = static_cast<StateIndex>(g) << (target + 1);
            // aligned-clause omitted: re+base offset is not guaranteed 32/64B aligned
            // for small target values; aligned-clause tuning deferred to perf pass
            // (base offset not guaranteed 32/64B aligned for small target).
#pragma omp simd
            for (StateIndex j = 0; j < stride; ++j) {
                const StateIndex i0 = base + j;
                const StateIndex i1 = i0 + stride;
                const double a0r = re[i0], a0i = im[i0];
                const double a1r = re[i1], a1i = im[i1];
                // Complex multiply-add via FMA3.
                re[i0] = std::fma(U.u00r, a0r, std::fma(-U.u00i, a0i, std::fma(U.u01r, a1r, -U.u01i * a1i)));
                im[i0] = std::fma(U.u00r, a0i, std::fma( U.u00i, a0r, std::fma(U.u01r, a1i,  U.u01i * a1r)));
                re[i1] = std::fma(U.u10r, a0r, std::fma(-U.u10i, a0i, std::fma(U.u11r, a1r, -U.u11i * a1i)));
                im[i1] = std::fma(U.u10r, a0i, std::fma( U.u10i, a0r, std::fma(U.u11r, a1i,  U.u11i * a1r)));
            }
        }
    } else {
        // Controlled: flat index scan with mask check.
        // SIMD not applied here because the conditional break defeats vectorisation.
        const std::int64_t half = static_cast<std::int64_t>(dim >> 1);
#pragma omp parallel for schedule(static)
        for (std::int64_t k = 0; k < half; ++k) {
            // Reconstruct i0 (target=0 bit) from flat half-index k.
            // Insert a 0 at position `target` in k's binary representation.
            StateIndex kk = static_cast<StateIndex>(k);
            StateIndex i0 = ((kk >> target) << (target + 1)) | (kk & (tbit - 1));
            if ((i0 & control_mask) != control_mask) continue;
            const StateIndex i1 = i0 | tbit;
            const double a0r = re[i0], a0i = im[i0];
            const double a1r = re[i1], a1i = im[i1];
            re[i0] = std::fma(U.u00r, a0r, std::fma(-U.u00i, a0i, std::fma(U.u01r, a1r, -U.u01i * a1i)));
            im[i0] = std::fma(U.u00r, a0i, std::fma( U.u00i, a0r, std::fma(U.u01r, a1i,  U.u01i * a1r)));
            re[i1] = std::fma(U.u10r, a0r, std::fma(-U.u10i, a0i, std::fma(U.u11r, a1r, -U.u11i * a1i)));
            im[i1] = std::fma(U.u10r, a0i, std::fma( U.u10i, a0r, std::fma(U.u11r, a1i,  U.u11i * a1r)));
        }
    }
}

// AVX-512 path: same body, different target attribute.
// Both paths share the same algorithm so results are numerically identical;
// the compiler generates wider vector instructions for the AVX-512 variant
// (single binary policy).
__attribute__((target("avx512f")))
void apply_controlled_1q_avx512(
        double* __restrict__ re, double* __restrict__ im,
        StateIndex dim,
        QubitIndex target,
        StateIndex control_mask,
        const Mat2x2& U)
{
    const StateIndex tbit   = StateIndex{1} << target;
    const StateIndex stride = tbit;

    if (control_mask == 0) {
        // Pure 1-qubit: parallelise over independent 2*stride-sized groups; the
        // inner contiguous run of `stride` amplitude pairs is the SIMD lane.
        // One OMP parallel region covers all groups (no per-block fork/join),
        // eliminating the O(dim/stride) fork overhead of the old per-block layout
        //
        const std::int64_t ngroups = static_cast<std::int64_t>(dim >> (target + 1));
#pragma omp parallel for schedule(static)
        for (std::int64_t g = 0; g < ngroups; ++g) {
            const StateIndex base = static_cast<StateIndex>(g) << (target + 1);
            // aligned-clause omitted: re+base offset is not guaranteed 32/64B aligned
            // for small target values; aligned-clause tuning deferred to perf pass
            // (base offset not guaranteed 32/64B aligned for small target).
#pragma omp simd
            for (StateIndex j = 0; j < stride; ++j) {
                const StateIndex i0 = base + j;
                const StateIndex i1 = i0 + stride;
                const double a0r = re[i0], a0i = im[i0];
                const double a1r = re[i1], a1i = im[i1];
                // Complex multiply-add via FMA3.
                re[i0] = std::fma(U.u00r, a0r, std::fma(-U.u00i, a0i, std::fma(U.u01r, a1r, -U.u01i * a1i)));
                im[i0] = std::fma(U.u00r, a0i, std::fma( U.u00i, a0r, std::fma(U.u01r, a1i,  U.u01i * a1r)));
                re[i1] = std::fma(U.u10r, a0r, std::fma(-U.u10i, a0i, std::fma(U.u11r, a1r, -U.u11i * a1i)));
                im[i1] = std::fma(U.u10r, a0i, std::fma( U.u10i, a0r, std::fma(U.u11r, a1i,  U.u11i * a1r)));
            }
        }
    } else {
        const std::int64_t half = static_cast<std::int64_t>(dim >> 1);
#pragma omp parallel for schedule(static)
        for (std::int64_t k = 0; k < half; ++k) {
            StateIndex kk = static_cast<StateIndex>(k);
            StateIndex i0 = ((kk >> target) << (target + 1)) | (kk & (tbit - 1));
            if ((i0 & control_mask) != control_mask) continue;
            const StateIndex i1 = i0 | tbit;
            const double a0r = re[i0], a0i = im[i0];
            const double a1r = re[i1], a1i = im[i1];
            re[i0] = std::fma(U.u00r, a0r, std::fma(-U.u00i, a0i, std::fma(U.u01r, a1r, -U.u01i * a1i)));
            im[i0] = std::fma(U.u00r, a0i, std::fma( U.u00i, a0r, std::fma(U.u01r, a1i,  U.u01i * a1r)));
            re[i1] = std::fma(U.u10r, a0r, std::fma(-U.u10i, a0i, std::fma(U.u11r, a1r, -U.u11i * a1i)));
            im[i1] = std::fma(U.u10r, a0i, std::fma( U.u10i, a0r, std::fma(U.u11r, a1i,  U.u11i * a1r)));
        }
    }
}

// ── Swap kernel (SIMD not applied; active-pair direct enumeration) ────────────
// Swaps amplitudes for all index pairs where bit(i,q_a)==0 and bit(i,q_b)==1
// (and all control bits set).  control_mask==0 means unconditional SWAP;
// non-zero means controlled SWAP (Fredkin gate).
//
// Rather than scan the full 2^n state and branch-filter (7/8 of iterations are
// empty for CSwap, and each idle iteration still pays 128-bit StateIndex
// arithmetic —), we enumerate ONLY the active pairs directly: the
// "free" qubits (all but q_a, q_b, and the control qubits) parametrise the
// 2^(n-np) active indices.  A counter is expanded into a full index by inserting
// a 0 bit at each fixed position (O(1) per fixed bit, np<=3), then OR-ing the
// forced-1 bits.  This removes the branch and cuts iterations 4x (SWAP) / 8x
// (CSwap), measured -43% on the cswap-heavy q25 pattern. Pure
// amplitude exchange (no FP arithmetic) -> bit-identical to the old full scan.
// SIMD is not applied: this is data movement, not an arithmetic kernel, so the
// dual AVX2/AVX-512 path rule does not apply (coding-rules).
void apply_swap_kernel(
        double* __restrict__ re, double* __restrict__ im,
        StateIndex dim,
        QubitIndex q_a, QubitIndex q_b,
        StateIndex control_mask)
{
    const StateIndex bit_a = StateIndex{1} << q_a;
    const StateIndex bit_b = StateIndex{1} << q_b;

    // Collect sorted positions of the fixed qubits (q_a, q_b, control bits).
    // bit_a/bit_b/control_mask are de-duplicated via the OR into fixed_mask.
    const StateIndex fixed_mask = bit_a | bit_b | control_mask;
    const StateIndex fixed_or   = bit_b | control_mask;  // forced-1 bits (q_a=0)
    QubitIndex fixed_pos[128];
    int        n_fixed = 0;
    for (QubitIndex q = 0; (StateIndex{1} << q) <= fixed_mask && q < 128; ++q) {
        if ((fixed_mask >> q) & StateIndex{1}) fixed_pos[n_fixed++] = q;
    }

    // Active pairs: one per setting of the (n - n_fixed) free qubits.
    const std::int64_t n_pairs = static_cast<std::int64_t>(dim >> n_fixed);
#pragma omp parallel for schedule(static)
    for (std::int64_t c = 0; c < n_pairs; ++c) {
        // Insert a 0 bit at each fixed position (ascending), then set forced-1
        // bits -> the active index i with q_a=0, q_b=1, controls=1.
        StateIndex i = static_cast<StateIndex>(c);
        for (int k = 0; k < n_fixed; ++k) {
            const StateIndex lowmask = (StateIndex{1} << fixed_pos[k]) - 1;
            i = ((i & ~lowmask) << 1) | (i & lowmask);
        }
        i |= fixed_or;
        const StateIndex j = (i | bit_a) & ~bit_b;  // partner: q_a=1, q_b=0
        const double tr = re[i], ti = im[i];
        re[i] = re[j]; im[i] = im[j];
        re[j] = tr;    im[j] = ti;
    }
}

// apply_dense_2q_kernel — apply a full 4x4 unitary to two qubits (ECR).
// The matrix is row-major, operand-order local bits: local index x = 2*b1 + b0
// where b0 = bit at q0, b1 = bit at q1 (matches ecr_matrix_data()). Enumerates
// only the dim/4 active quads via the same 0-bit insertion trick as
// apply_swap_kernel, then does a 4x4 GEMV per quad. Plain OpenMP scalar loop: the
// 4x4 matmul is tiny and this is a rare post-transpile native-gate path, not a hot
// kernel, so no AVX2/AVX-512 split is warranted (coding-rules, same class as
// apply_swap_kernel). Bit-identical to the fused GEMV path for the same ECR.
void apply_dense_2q_kernel(double* __restrict__ re, double* __restrict__ im,
                           StateIndex dim, QubitIndex q0, QubitIndex q1,
                           const double* m)
{
    const QubitIndex lo = q0 < q1 ? q0 : q1;
    const QubitIndex hi = q0 < q1 ? q1 : q0;
    const StateIndex lo_mask  = (StateIndex{1} << lo) - 1;
    const StateIndex mid_mask = ((StateIndex{1} << (hi - 1)) - 1) & ~lo_mask;
    const StateIndex bit0 = StateIndex{1} << q0;  // operand-0 bit
    const StateIndex bit1 = StateIndex{1} << q1;  // operand-1 bit

    const std::int64_t quads = static_cast<std::int64_t>(dim >> 2);
#pragma omp parallel for schedule(static)
    for (std::int64_t qq = 0; qq < quads; ++qq) {
        const StateIndex quad = static_cast<StateIndex>(qq);
        // Insert two 0 bits at positions lo and hi to form the base index.
        const StateIndex base = (quad & lo_mask)
            | ((quad & mid_mask) << 1)
            | ((quad >> (hi - 1)) << (hi + 1));
        // Gather the 4 amplitudes at operand-order local index x = 2*b1 + b0.
        const StateIndex sidx[4] = {
            base, base | bit0, base | bit1, base | bit0 | bit1
        };
        double ar[4], ai[4];
        for (int k = 0; k < 4; ++k) { ar[k] = re[sidx[k]]; ai[k] = im[sidx[k]]; }
        for (int r = 0; r < 4; ++r) {
            double nr = 0.0, ni = 0.0;
            for (int c = 0; c < 4; ++c) {
                const double mr = m[8 * r + 2 * c];
                const double mi = m[8 * r + 2 * c + 1];
                nr += mr * ar[c] - mi * ai[c];
                ni += mr * ai[c] + mi * ar[c];
            }
            re[sidx[r]] = nr; im[sidx[r]] = ni;
        }
    }
}

// ── Kernel function pointer types ─────────────────────────────────────────────

using CtrlKernelFn = void(*)(double*, double*, StateIndex, QubitIndex,
                              StateIndex, const Mat2x2&);

// Select the appropriate controlled-1Q kernel once per process start.
// Compile-time #ifdef branching is forbidden (single-binary policy).
CtrlKernelFn get_ctrl_kernel() {
    return qplayer::detail::select_for_host(
        &apply_controlled_1q_avx2,
        &apply_controlled_1q_avx512);
}

// Process-lifetime kernel pointer; initialised at first apply_gate call via
// function-local static (C++11 thread-safe, same pattern as detect_simd_tier).
CtrlKernelFn ctrl_kernel() {
    static CtrlKernelFn fn = get_ctrl_kernel();
    return fn;
}

// ── Cache-blocked 1Q/controlled-2Q kernel (AVX2 path) ────────────────────────
// L2-tiled traversal of state-vector amplitude pairs for the 2x2 unitary U.
// The blocking ensures the working set (2 * CACHE_BLOCK_SIZE doubles) fits in
// L2 cache, reducing cache-miss penalty for large state vectors.
//
// Tiling strategy: the outer loop over 'blocks' maps to L2-sized tiles of
// flat state-vector indices.  Within each tile we identify the valid i0 indices
// (target=0 bit, control bits satisfied) and apply the 2x2 update.
//
// Correctness: every amplitude pair (i0, i1=i0|tbit) is visited exactly once
// regardless of target qubit position.  For the pure-1Q path (control_mask==0)
// every k-th index with bit(k,target)=0 is an i0.  The flat index scan used
// here is equivalent to the group-parallel scan in apply_controlled_1q_avx2
// and produces identical numerical results (same FMA3 body).
//
// Alignment: re+i0 offset is not guaranteed 32B-aligned for small target values
// (e.g., target=0 gives odd-address strides), so the aligned() clause is
// omitted to avoid misaligned SIMD faults (lesson).
//
// NOTE: runtime Gate Fusion may produce 3Q/4Q fused unitaries that
// bypass this 1Q/2Q blocked path — trade-off, unresolved.
__attribute__((target("avx2,fma")))
void apply_controlled_1q_blocked_avx2(
        double* __restrict__ re, double* __restrict__ im,
        StateIndex dim,
        QubitIndex target,
        StateIndex control_mask,
        const Mat2x2& U)
{
    const StateIndex tbit = StateIndex{1} << target;
    const std::int64_t idim  = static_cast<std::int64_t>(dim);
    const std::int64_t block_size =
        static_cast<std::int64_t>(QPLAYER_CACHE_BLOCK_SIZE);

    // Outer loop: L2-tiled blocks of flat state-vector indices.
    // Each thread owns a contiguous block; independent blocks are safe to
    // process in parallel (no shared write between amplitude pairs within
    // different blocks, given each pair (i0,i1) is processed once).
#pragma omp parallel for schedule(static)
    for (std::int64_t blk_start = 0; blk_start < idim; blk_start += block_size) {
        const std::int64_t blk_end =
            (blk_start + block_size < idim) ? (blk_start + block_size) : idim;

        // Inner loop: scan flat indices in [blk_start, blk_end).
        // Process only i0 entries (bit(target)==0) so each pair is touched once.
        for (std::int64_t idx = blk_start; idx < blk_end; ++idx) {
            const StateIndex i0 = static_cast<StateIndex>(idx);
            // Only process index if the target bit is 0 (i0 of the pair).
            if ((i0 >> target) & StateIndex{1}) continue;
            // Control check: skip if any required control bit is not set.
            if (control_mask != 0 && (i0 & control_mask) != control_mask) continue;
            const StateIndex i1 = i0 | tbit;
            const double a0r = re[i0], a0i = im[i0];
            const double a1r = re[i1], a1i = im[i1];
            // Complex matrix-vector multiply via FMA3.
            re[i0] = std::fma(U.u00r, a0r, std::fma(-U.u00i, a0i, std::fma(U.u01r, a1r, -U.u01i * a1i)));
            im[i0] = std::fma(U.u00r, a0i, std::fma( U.u00i, a0r, std::fma(U.u01r, a1i,  U.u01i * a1r)));
            re[i1] = std::fma(U.u10r, a0r, std::fma(-U.u10i, a0i, std::fma(U.u11r, a1r, -U.u11i * a1i)));
            im[i1] = std::fma(U.u10r, a0i, std::fma( U.u10i, a0r, std::fma(U.u11r, a1i,  U.u11i * a1r)));
        }
    }
}

// AVX-512 path: identical body, wider vector registers.
__attribute__((target("avx512f")))
void apply_controlled_1q_blocked_avx512(
        double* __restrict__ re, double* __restrict__ im,
        StateIndex dim,
        QubitIndex target,
        StateIndex control_mask,
        const Mat2x2& U)
{
    const StateIndex tbit = StateIndex{1} << target;
    const std::int64_t idim  = static_cast<std::int64_t>(dim);
    const std::int64_t block_size =
        static_cast<std::int64_t>(QPLAYER_CACHE_BLOCK_SIZE);

#pragma omp parallel for schedule(static)
    for (std::int64_t blk_start = 0; blk_start < idim; blk_start += block_size) {
        const std::int64_t blk_end =
            (blk_start + block_size < idim) ? (blk_start + block_size) : idim;

        for (std::int64_t idx = blk_start; idx < blk_end; ++idx) {
            const StateIndex i0 = static_cast<StateIndex>(idx);
            if ((i0 >> target) & StateIndex{1}) continue;
            if (control_mask != 0 && (i0 & control_mask) != control_mask) continue;
            const StateIndex i1 = i0 | tbit;
            const double a0r = re[i0], a0i = im[i0];
            const double a1r = re[i1], a1i = im[i1];
            re[i0] = std::fma(U.u00r, a0r, std::fma(-U.u00i, a0i, std::fma(U.u01r, a1r, -U.u01i * a1i)));
            im[i0] = std::fma(U.u00r, a0i, std::fma( U.u00i, a0r, std::fma(U.u01r, a1i,  U.u01i * a1r)));
            re[i1] = std::fma(U.u10r, a0r, std::fma(-U.u10i, a0i, std::fma(U.u11r, a1r, -U.u11i * a1i)));
            im[i1] = std::fma(U.u10r, a0i, std::fma( U.u10i, a0r, std::fma(U.u11r, a1i,  U.u11i * a1r)));
        }
    }
}

// Kernel pointer type for the cache-blocked variant.
using BlockedKernelFn = void(*)(double*, double*, StateIndex, QubitIndex,
                                StateIndex, const Mat2x2&);

// Select the blocked kernel once per process (CPUID dispatch).
BlockedKernelFn get_blocked_kernel() {
    return qplayer::detail::select_for_host(
        &apply_controlled_1q_blocked_avx2,
        &apply_controlled_1q_blocked_avx512);
}

// Process-lifetime blocked kernel pointer.
BlockedKernelFn blocked_kernel() {
    static BlockedKernelFn fn = get_blocked_kernel();
    return fn;
}

}  // namespace

// ── CpuStateBuffers::AlignedDeleter ──────────────────────────────────────────

// std::free releases the buffer obtained from std::aligned_alloc in
// alloc_aligned() (— new/delete direct use forbidden).
// FullStateCpuBackend::AlignedDeleter is a using-alias for this type.
void CpuStateBuffers::AlignedDeleter::operator()(double* p) const noexcept {
    std::free(p);
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

// Check env QPLAYER_CACHE_BLOCKING at construction so benchmark scripts can
// toggle the path without code changes. Default OFF since Phase 7-D
// — blocking measured net-negative. "1" opts in; absent/"0" stays off.
FullStateCpuBackend::FullStateCpuBackend() {
    const char* env = std::getenv("QPLAYER_CACHE_BLOCKING");
    if (env != nullptr && std::strcmp(env, "0") != 0) {
        cache_blocking_enabled_ = true;
    }
}

FullStateCpuBackend::~FullStateCpuBackend() = default;

// ── Batch-mode pipeline ───────────────────────────────────────────────────────

// prepare() — memory budget pre-check then state initialisation
//
void FullStateCpuBackend::prepare(const Circuit& circuit) {
    const QubitIndex n = circuit.num_qubits();

    // Memory guard: peak working set = base 2^n x 16 (re + im) plus one execute
    // transient (reorder scratch or sampling prob/cdf, which never overlap), so
    // 2x the bare state vector. Checking only the
    // base would pass the precheck and then swap/OOM during reorder or sampling.
    qplayer::detail::check_memory_budget(
        qplayer::detail::peak_state_bytes(n, qplayer::detail::WorkingSet::Execute),
        "FullState",
        n);

    init_state(n);
}

// ── execute() — fast path + per-shot path ──────

// ── apply_gate_to_buffer — stateless gate dispatch for trajectory paths ───────
// Applies a GateOp to explicit SoA buffers (re, im, dim) without touching any
// backend member fields. Used by the Monte Carlo trajectory loop so each
// trajectory thread operates on its own private buffer concurrently.
// Logic mirrors FullStateCpuBackend::apply_gate() exactly: same gate_matrix(),
// same ctrl_kernel() / apply_swap_kernel() dispatch. The blocked path is skipped
// (trajectory buffers are std::vector-allocated, not aligned_alloc; correctness
// trumps the marginal cache-blocking gain here — net-negative anyway).
static void apply_gate_to_buffer(double* re, double* im, StateIndex dim,
                                 const GateOp& op)
{
    const GateType type = op.type;

    if (type == GateType::SWAP) {
        apply_swap_kernel(re, im, dim, op.qubits[0], op.qubits[1],
                          /*control_mask=*/StateIndex{0});
        return;
    }
    if (type == GateType::CSwap) {
        const StateIndex ctrl_mask = StateIndex{1} << op.qubits[0];
        apply_swap_kernel(re, im, dim, op.qubits[1], op.qubits[2], ctrl_mask);
        return;
    }
    if (type == GateType::ECR) {
        apply_dense_2q_kernel(re, im, dim, op.qubits[0], op.qubits[1],
                              ecr_matrix_data().data());
        return;
    }
    // Google native 2Q gates: genuine 4x4 applied via the dense-2Q kernel.
    if (type == GateType::ISWAP) {
        apply_dense_2q_kernel(re, im, dim, op.qubits[0], op.qubits[1],
                              iswap_matrix_data().data());
        return;
    }
    if (type == GateType::SQRT_ISWAP) {
        apply_dense_2q_kernel(re, im, dim, op.qubits[0], op.qubits[1],
                              sqrt_iswap_matrix_data().data());
        return;
    }
    if (type == GateType::FSIM) {
        // fSim is parametric: compute matrix from params[0]=theta, params[1]=phi.
        const auto m = fsim_matrix_data(op.params[0], op.params[1]);
        apply_dense_2q_kernel(re, im, dim, op.qubits[0], op.qubits[1], m.data());
        return;
    }

    const Mat2x2 U = gate_matrix(type, op.params);
    StateIndex control_mask = StateIndex{0};
    QubitIndex target       = op.qubits[0];

    switch (type) {
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
        case GateType::CCX:
            control_mask = (StateIndex{1} << op.qubits[0]) |
                           (StateIndex{1} << op.qubits[1]);
            target       = op.qubits[2];
            break;
        default:
            break;
    }

    // Unblocked path: trajectory buffers may not be 64B-aligned.
    ctrl_kernel()(re, im, dim, target, control_mask, U);
}

// ── apply_kraus_channel_cpu — Monte Carlo Kraus channel application ───────────
// Applies one KrausChannel to the current SoA state vector in place:
//   1. For each Kraus operator K_i compute p_i = ||K_i|ψ⟩||².
//   2. Draw r ~ Uniform[0,1) from the caller's rng.
//   3. Pick operator index k = first i with cumsum_i p_i > r.
//   4. Apply K_k to target_qubit (2x2 row-major complex on SoA), then renormalise.
// Single-qubit channels only (operators are 2x2). Thread-unsafe on re_/im_:
// callers must ensure exclusive access to the buffer during trajectory replay
// (in the OMP trajectory loop each trajectory owns its own buffer — safe).
static void apply_kraus_channel_cpu(
        double* re, double* im, StateIndex dim,
        const KrausChannel& ch,
        std::mt19937_64& rng)
{
    // Two-qubit (4x4) channels use the shared 2Q Kraus routine so CPU and GPU
    // produce identical statistics. Single-qubit path below.
    if (!ch.operators.empty() && ch.operators.front().size() == 4) {
        qplayer::detail::apply_kraus_2q(re, im, dim, ch.target_qubit,
                                        ch.target_qubit2, ch.operators, rng);
        return;
    }

    const QubitIndex tgt = ch.target_qubit;
    const std::size_t nops = ch.operators.size();

    // Step 1: compute p_i = ||K_i|ψ⟩||² for each operator.
    // For a 2x2 Kraus K applied to qubit `tgt`, |K|ψ⟩||² is the sum of
    // squared magnitudes of the output amplitudes. We compute the output
    // amplitude pair (new_i0, new_i1) per pair (i0,i1) without mutating the
    // state, accumulate |new|² into p_i.
    std::vector<double> probs(nops, 0.0);
    const StateIndex tbit = StateIndex{1} << tgt;
    const std::int64_t idim = static_cast<std::int64_t>(dim);

    for (std::size_t ki = 0; ki < nops; ++ki) {
        const auto& K = ch.operators[ki];
        // K is row-major 2x2: K[row][col], complex<double>.
        const double k00r = K[0][0].real(), k00i = K[0][0].imag();
        const double k01r = K[0][1].real(), k01i = K[0][1].imag();
        const double k10r = K[1][0].real(), k10i = K[1][0].imag();
        const double k11r = K[1][1].real(), k11i = K[1][1].imag();

        double pi = 0.0;
        // Iterate over all amplitude pairs (i0, i1) where bit(i0, tgt)=0.
        for (std::int64_t jj = 0; jj < idim / 2; ++jj) {
            // Reconstruct i0 from flat half-index by inserting 0 at position tgt.
            const StateIndex jk = static_cast<StateIndex>(jj);
            const StateIndex i0 = ((jk >> tgt) << (tgt + 1)) | (jk & (tbit - 1));
            const StateIndex i1 = i0 | tbit;
            const double a0r = re[i0], a0i = im[i0];
            const double a1r = re[i1], a1i = im[i1];
            // n0 = K[0][0]*a0 + K[0][1]*a1
            const double n0r = k00r*a0r - k00i*a0i + k01r*a1r - k01i*a1i;
            const double n0i = k00r*a0i + k00i*a0r + k01r*a1i + k01i*a1r;
            // n1 = K[1][0]*a0 + K[1][1]*a1
            const double n1r = k10r*a0r - k10i*a0i + k11r*a1r - k11i*a1i;
            const double n1i = k10r*a0i + k10i*a0r + k11r*a1i + k11i*a1r;
            pi += std::fma(n0r, n0r, n0i*n0i) + std::fma(n1r, n1r, n1i*n1i);
        }
        probs[ki] = pi;
    }

    // Step 2-3: pick operator by cumulative probability.
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    const double u = dist(rng);
    double cumsum = 0.0;
    std::size_t chosen = 0;
    for (std::size_t ki = 0; ki < nops; ++ki) {
        cumsum += probs[ki];
        if (u < cumsum) { chosen = ki; break; }
        chosen = ki;  // fallback to last if floating-point rounding
    }

    // Step 4: apply K_{chosen} in-place and renormalise.
    const auto& Kc = ch.operators[chosen];
    const double k00r = Kc[0][0].real(), k00i = Kc[0][0].imag();
    const double k01r = Kc[0][1].real(), k01i = Kc[0][1].imag();
    const double k10r = Kc[1][0].real(), k10i = Kc[1][0].imag();
    const double k11r = Kc[1][1].real(), k11i = Kc[1][1].imag();

    for (std::int64_t jj = 0; jj < idim / 2; ++jj) {
        const StateIndex jk = static_cast<StateIndex>(jj);
        const StateIndex i0 = ((jk >> tgt) << (tgt + 1)) | (jk & (tbit - 1));
        const StateIndex i1 = i0 | tbit;
        const double a0r = re[i0], a0i = im[i0];
        const double a1r = re[i1], a1i = im[i1];
        re[i0] = k00r*a0r - k00i*a0i + k01r*a1r - k01i*a1i;
        im[i0] = k00r*a0i + k00i*a0r + k01r*a1i + k01i*a1r;
        re[i1] = k10r*a0r - k10i*a0i + k11r*a1r - k11i*a1i;
        im[i1] = k10r*a0i + k10i*a0r + k11r*a1i + k11i*a1r;
    }

    // Renormalise: norm = probs[chosen] (= ||K_{chosen}|ψ⟩||²).
    const double p_chosen = probs[chosen];
    if (p_chosen < 1e-15) return;  // zero-probability branch: state is already ~0
    const double inv_norm = 1.0 / std::sqrt(p_chosen);
    for (std::int64_t ii = 0; ii < idim; ++ii) {
        re[ii] *= inv_norm;
        im[ii] *= inv_norm;
    }
}

Result FullStateCpuBackend::execute(const Circuit&    circuit,
                                    std::uint32_t     nshots,
                                    const NoiseModel* noise) {
    // Seed the PRNG: use master_seed_ if set (non-zero), otherwise derive a
    // random seed from std::random_device for non-reproducible runs.
    const std::uint64_t seed =
        master_seed_ != 0
            ? master_seed_
            : static_cast<std::uint64_t>(std::random_device{}());

    // ── Monte Carlo trajectory path ─────────────────────
    // Activated when a non-empty NoiseModel is provided. Cannot use the fast-path
    // lazy single-state sampling — each trajectory evolves a different state.
    if (noise != nullptr) {
        const QubitIndex num_q = circuit.num_qubits();
        const ClassicalBitIndex total_c = circuit.total_cubits();
        const auto& cregs = circuit.classical_registers();

        // Readout error gate: skip the per-measure lookup entirely when none
        // are registered. Captured read-only by the lambda.
        const bool readout_active = noise->has_readout_errors();

        // Shared accumulator: merge per-thread local count maps after the OMP
        // region to avoid false sharing / lock contention per iteration.
        std::map<std::string, std::uint64_t> counts;
        std::mutex counts_mutex;

        // Error capture across OMP boundary: flag + message.
        // Exceptions cannot propagate out of a parallel region; we store the
        // first error and re-throw after the region ends.
        std::atomic<bool> error_flag{false};
        std::string       error_msg;
        std::mutex        error_mutex;

        const std::int64_t nshots_i = static_cast<std::int64_t>(nshots);

#pragma omp parallel for schedule(dynamic)
        for (std::int64_t traj = 0; traj < nshots_i; ++traj) {
            // Early exit once another thread set the error flag.
            if (error_flag.load(std::memory_order_relaxed)) continue;

            try {
                // Per-trajectory private SoA state (avoids sharing re_/im_
                // across threads — each trajectory is fully independent).
                const StateIndex dim = StateIndex{1} << num_q;
                const std::size_t dimz = static_cast<std::size_t>(dim);

                // Thread-local heap state: two aligned double arrays.
                // Using std::vector<double> keeps RAII cleanup on exception.
                // Alignment to 64B is best-effort via std::vector (no SIMD
                // here since trajectory body is sequential, not a SIMD loop).
                std::vector<double> traj_re(dimz, 0.0);
                std::vector<double> traj_im(dimz, 0.0);
                traj_re[0] = 1.0;  // |0...0>

                double* re = traj_re.data();
                double* im = traj_im.data();

                // Per-trajectory RNG seeded reproducibly.
                const std::uint64_t traj_seed =
                    (seed != 0)
                        ? qplayer::detail::thread_seed(
                              seed, static_cast<std::uint32_t>(traj))
                        : static_cast<std::uint64_t>(std::random_device{}());
                std::mt19937_64 traj_rng(traj_seed);

                // Classical register state for this trajectory.
                std::vector<int> cubit_vals(static_cast<std::size_t>(total_c), 0);

                std::size_t op_index = 0;
                for (const Op& op : circuit.ops()) {
                    std::visit([&](auto&& node) {
                        using T = std::decay_t<decltype(node)>;

                        if constexpr (std::is_same_v<T, GateOp>) {
                            if (node.type == GateType::Reset) {
                                // Measure + X conditioned on result (collapse to |0>).
                                // Borrow the CPU backend measure helper for the
                                // collapse step, then apply X if needed.
                                double p1 = 0.0;
                                for (std::size_t ii = 0; ii < dimz; ++ii) {
                                    const StateIndex i = static_cast<StateIndex>(ii);
                                    if ((i >> node.qubits[0]) & StateIndex{1}) {
                                        p1 += std::fma(re[ii], re[ii], im[ii]*im[ii]);
                                    }
                                }
                                std::uniform_real_distribution<double> u01(0.0, 1.0);
                                const int outcome = (u01(traj_rng) < p1) ? 1 : 0;
                                const double p_out = (outcome == 1) ? p1 : (1.0 - p1);
                                const double inv = (p_out > 1e-15) ? 1.0/std::sqrt(p_out) : 0.0;
                                const int keep = outcome;
                                for (std::size_t ii = 0; ii < dimz; ++ii) {
                                    const StateIndex i = static_cast<StateIndex>(ii);
                                    const int bit = static_cast<int>((i >> node.qubits[0]) & StateIndex{1});
                                    if (bit != keep) { re[ii] = 0.0; im[ii] = 0.0; }
                                    else { re[ii] *= inv; im[ii] *= inv; }
                                }
                                if (outcome == 1) {
                                    // Apply X gate to flip back to |0>.
                                    const StateIndex tbit = StateIndex{1} << node.qubits[0];
                                    for (std::int64_t jj = 0; jj < static_cast<std::int64_t>(dimz)/2; ++jj) {
                                        const StateIndex jk = static_cast<StateIndex>(jj);
                                        const StateIndex i0 = ((jk >> node.qubits[0]) << (node.qubits[0]+1)) | (jk & (tbit-1));
                                        const StateIndex i1 = i0 | tbit;
                                        std::swap(re[i0], re[i1]);
                                        std::swap(im[i0], im[i1]);
                                    }
                                }
                            } else if (node.type == GateType::Opaque) {
                                throw NotImplementedError{"FullState",
                                    "Opaque gate not yet supported in noisy execute()"};
                            } else {
                                // Apply the gate to the private trajectory buffer.
                                // apply_gate_to_buffer() is a stateless free function
                                // (defined just above execute()) that operates on
                                // explicit re/im/dim pointers. No lock needed — each
                                // trajectory owns its private buffer.
                                apply_gate_to_buffer(re, im, dim, node);

                                // Apply noise channels after the gate.
                                if (noise->has_channels_for_gate(node.name)) {
                                    auto chans = noise->channels_for_gate(
                                        node.name, node.qubits);
                                    for (const KrausChannel& ch : chans) {
                                        apply_kraus_channel_cpu(
                                            re, im, dim, ch, traj_rng);
                                    }
                                }
                            }

                        } else if constexpr (std::is_same_v<T, IdleOp>) {
                            // Apply idle relaxation Kraus channels.
                            auto chans = noise->idle_channels(
                                node.qubit, node.duration_ns);
                            for (const KrausChannel& ch : chans) {
                                apply_kraus_channel_cpu(re, im, dim, ch, traj_rng);
                            }

                        } else if constexpr (std::is_same_v<T, MeasureOp>) {
                            // Born-rule collapse with trajectory RNG.
                            double p1 = 0.0;
                            for (std::size_t ii = 0; ii < dimz; ++ii) {
                                const StateIndex i = static_cast<StateIndex>(ii);
                                if ((i >> node.qubit) & StateIndex{1}) {
                                    p1 += std::fma(re[ii], re[ii], im[ii]*im[ii]);
                                }
                            }
                            std::uniform_real_distribution<double> u01(0.0, 1.0);
                            const int outcome = (u01(traj_rng) < p1) ? 1 : 0;
                            const double p_out = (outcome == 1) ? p1 : (1.0 - p1);
                            if (p_out < 1e-15) {
                                throw SimulatorError{"FullState",
                                    "trajectory " + std::to_string(traj) +
                                    " op " + std::to_string(op_index) +
                                    ": measure p_outcome near zero"};
                            }
                            const double inv = 1.0 / std::sqrt(p_out);
                            const int keep = outcome;
                            for (std::size_t ii = 0; ii < dimz; ++ii) {
                                const StateIndex i = static_cast<StateIndex>(ii);
                                const int bit = static_cast<int>((i >> node.qubit) & StateIndex{1});
                                if (bit != keep) { re[ii] = 0.0; im[ii] = 0.0; }
                                else { re[ii] *= inv; im[ii] *= inv; }
                            }
                            // Readout error: the collapse
                            // above used the true outcome; only the reported
                            // classical bit is flipped by the confusion probs.
                            // IfOp and final counts observe this reported value.
                            int reported = outcome;
                            if (readout_active) {
                                const auto [p01, p10] =
                                    noise->readout_error(node.qubit);
                                if (p01 > 0.0 || p10 > 0.0) {
                                    reported = apply_readout_error(
                                        outcome, p01, p10, u01(traj_rng));
                                }
                            }
                            cubit_vals[static_cast<std::size_t>(node.cubit)] = reported;

                        } else if constexpr (std::is_same_v<T, IfOp>) {
                            // Evaluate classical register and conditionally apply body.
                            const ClassicalRegister& reg = cregs[node.creg];
                            std::uint64_t regval = 0;
                            for (ClassicalBitIndex b = 0; b < reg.width; ++b) {
                                if (cubit_vals[static_cast<std::size_t>(reg.offset + b)]) {
                                    regval |= (std::uint64_t{1} << b);
                                }
                            }
                            if (regval == node.value) {
                                if (node.body.type == GateType::Reset) {
                                    // Inline reset (same as GateOp reset above).
                                    const QubitIndex rq = node.body.qubits[0];
                                    double p1 = 0.0;
                                    for (std::size_t ii = 0; ii < dimz; ++ii) {
                                        const StateIndex i = static_cast<StateIndex>(ii);
                                        if ((i >> rq) & StateIndex{1}) {
                                            p1 += std::fma(re[ii], re[ii], im[ii]*im[ii]);
                                        }
                                    }
                                    std::uniform_real_distribution<double> u01(0.0, 1.0);
                                    const int outcome = (u01(traj_rng) < p1) ? 1 : 0;
                                    const double p_out = (outcome == 1) ? p1 : (1.0 - p1);
                                    const double inv = (p_out > 1e-15) ? 1.0/std::sqrt(p_out) : 0.0;
                                    const int keep = outcome;
                                    for (std::size_t ii = 0; ii < dimz; ++ii) {
                                        const StateIndex i = static_cast<StateIndex>(ii);
                                        const int bit = static_cast<int>((i >> rq) & StateIndex{1});
                                        if (bit != keep) { re[ii] = 0.0; im[ii] = 0.0; }
                                        else { re[ii] *= inv; im[ii] *= inv; }
                                    }
                                    if (outcome == 1) {
                                        const StateIndex tbit = StateIndex{1} << rq;
                                        for (std::int64_t jj = 0; jj < static_cast<std::int64_t>(dimz)/2; ++jj) {
                                            const StateIndex jk = static_cast<StateIndex>(jj);
                                            const StateIndex i0 = ((jk >> rq) << (rq+1)) | (jk & (tbit-1));
                                            const StateIndex i1 = i0 | tbit;
                                            std::swap(re[i0], re[i1]);
                                            std::swap(im[i0], im[i1]);
                                        }
                                    }
                                } else {
                                    apply_gate_to_buffer(re, im, dim, node.body);
                                }
                            }

                        } else {
                            // BarrierOp: no state change.
                            (void)node;
                        }
                    }, op);
                    ++op_index;
                }  // end op loop

                // Norm check: verify |ψ|² ≈ 1 after all ops.
                double norm = 0.0;
                for (std::size_t ii = 0; ii < dimz; ++ii) {
                    norm += std::fma(re[ii], re[ii], im[ii]*im[ii]);
                }
                if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-6) {
                    throw SimulatorError{"FullState",
                        "numerical divergence at trajectory " +
                        std::to_string(traj) + ": |psi|^2 = " +
                        std::to_string(norm)};
                }

                // Build bitstring from cubit_vals (MSB-first, same convention
                // as the noiseless per-shot path).
                std::string bs(static_cast<std::size_t>(total_c), '0');
                for (ClassicalBitIndex k = 0; k < total_c; ++k) {
                    bs[static_cast<std::size_t>(k)] =
                        static_cast<char>('0' + cubit_vals[
                            static_cast<std::size_t>(total_c - 1u - k)]);
                }

                // Accumulate into per-thread map, merge under lock.
                {
                    std::lock_guard<std::mutex> lk(counts_mutex);
                    ++counts[bs];
                }

            } catch (const std::exception& ex) {
                // Capture the first error; signal all threads to stop early.
                if (!error_flag.exchange(true, std::memory_order_acq_rel)) {
                    std::lock_guard<std::mutex> lk(error_mutex);
                    error_msg = ex.what();
                }
            }
        }  // end OMP parallel for

        // Re-throw captured error after the parallel region.
        if (error_flag.load()) {
            throw SimulatorError{"FullState", error_msg};
        }

        // Empty circuit or zero cubits: ensure empty string key.
        if (total_c == 0) {
            counts[""] = nshots;
        }

        Result r;
        result_set_counts(r, std::move(counts));
        result_set_shots(r, nshots);
        return r;
    }
    // ── End Monte Carlo trajectory path ───────────────────────────────────

    // Fast-path nshots sampling is deferred to Result::counts() (lazy);
    // the per-shot path below seeds its own shot_rng from `seed`.
    Result r;
    std::map<std::string, std::uint64_t> counts;
    const ClassicalBitIndex total_c = circuit.total_cubits();

    if (!circuit.has_classical_control()) {
        // ── fast path — no IfOp, no mid-circuit measurement ──────────
        // Apply all gate ops once, then sample the final distribution nshots times.

        prepare(circuit);  // memory budget guard + init_state

        // Runtime gate fusion is usable only when no Reset/Opaque op is present
        // (fuse_circuit() rejects those). Scan once to decide the path.
        bool fusable = fusion_enabled_;
        if (fusable) {
            for (const Op& op : circuit.ops()) {
                if (const auto* g = std::get_if<GateOp>(&op)) {
                    if (g->type == GateType::Reset || g->type == GateType::Opaque) {
                        fusable = false;
                        break;
                    }
                }
            }
        }

        if (fusable) {
            // Fused path: merge adjacent gates into k-qubit unitaries.
            // MeasureOp/Barrier are no-ops here; measurement is sampled below
            // from the final |ψ⟩. Fusion equivalence is verified in.
            apply_fused_program(
                qplayer::detail::fuse_circuit(circuit.ops(), fusion_max_width_));
        } else {
            // Unfused path. Apply unitary ops; skip MeasureOp / BarrierOp
            // boundaries (IfOp cannot appear: has_classical_control()==false).
            for (const Op& op : circuit.ops()) {
                std::visit([this](auto&& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, GateOp>) {
                        if (node.type == GateType::Reset) {
                            // Reset on the fast path: |0⟩ projection (rare but valid).
                            apply_reset(node.qubits[0]);
                        } else if (node.type == GateType::Opaque) {
                            throw NotImplementedError{"FullState", "Opaque gate not yet supported"};
                        } else {
                            apply_gate(node);
                        }
                    } else if constexpr (std::is_same_v<T, MeasureOp>) {
                        // Defer measurement — sampled from final |ψ⟩ below.
                        (void)node;
                    } else {
                        // BarrierOp, IdleOp: no state change.
                        (void)node;
                    }
                }, op);
            }
        }

        // Collect MeasureOps for bitstring construction.
        std::vector<MeasureOp> measure_ops;
        for (const Op& op : circuit.ops()) {
            if (std::holds_alternative<MeasureOp>(op)) {
                measure_ops.push_back(std::get<MeasureOp>(op));
            }
        }

        // P2: hand the SoA buffers to the Result as a shared owner
        // instead of eager AoS materialisation (which was 33% of execute() time
        // for q27). statevector() / get_statevector_raw() lazily AoS-materialise
        // on first access; counts()-only workloads never pay the 2 GiB copy.
        result_set_cpu_resident(r, state_owner_, re_, im_, dim_);

        // Lazy sampling: defer the nshots inverse-CDF draw to the
        // first Result::counts() call. Capture the resolved seed (so deferred
        // sampling is reproducible) and the (qubit, cubit) measure map;
        // the draw runs in Result::perform_lazy_sampling() from the SoA state.
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map;
        measure_map.reserve(measure_ops.size());
        for (const MeasureOp& m : measure_ops) {
            measure_map.emplace_back(m.qubit, m.cubit);
        }
        result_set_lazy_sampling(r, nshots, seed, total_c, std::move(measure_map));
        result_set_shots(r, nshots);
        return r;

    } else {
        // ── per-shot path — IfOp or mid-circuit measurement present ───
        // Each shot: reset to |0…0⟩, replay the full op sequence with collapse.

        const QubitIndex num_q = circuit.num_qubits();
        const auto& cregs      = circuit.classical_registers();

        std::vector<int> cubit_vals(total_c, 0);

        for (std::uint32_t shot = 0; shot < nshots; ++shot) {
            init_state(num_q);
            std::fill(cubit_vals.begin(), cubit_vals.end(), 0);

            // Per-shot RNG derived from master_seed_ and the shot index so that
            // results are reproducible when set_seed() has been called.
            const std::uint64_t shot_seed =
                seed != 0
                    ? qplayer::detail::thread_seed(seed,
                          static_cast<std::uint32_t>(shot))
                    : static_cast<std::uint64_t>(std::random_device{}());
            std::mt19937_64 shot_rng(shot_seed);

            for (const Op& op : circuit.ops()) {
                std::visit([&](auto&& node) {
                    using T = std::decay_t<decltype(node)>;

                    if constexpr (std::is_same_v<T, GateOp>) {
                        if (node.type == GateType::Reset) {
                            // Use shot_rng for deterministic reset.
                            apply_reset_rng(node.qubits[0], shot_rng);
                        } else if (node.type == GateType::Opaque) {
                            throw NotImplementedError{"FullState",
                                "Opaque gate not yet supported"};
                        } else {
                            apply_gate(node);
                        }

                    } else if constexpr (std::is_same_v<T, MeasureOp>) {
                        // Born-rule collapse with shot-scoped RNG for determinism.
                        cubit_vals[node.cubit] =
                            apply_measure_rng(node.qubit, shot_rng);

                    } else if constexpr (std::is_same_v<T, IfOp>) {
                        // Evaluate the register value and branch on match.
                        // IfOp.creg indexes circuit.classical_registers().
                        const ClassicalRegister& reg = cregs[node.creg];
                        // Reconstruct unsigned integer from cubit_vals[offset..offset+width-1].
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
                                throw NotImplementedError{"FullState",
                                    "Opaque gate in IfOp not yet supported"};
                            } else {
                                apply_gate(node.body);
                            }
                        }

                    } else {
                        // BarrierOp, IdleOp: no state change.
                        (void)node;
                    }
                }, op);
            }  // end op loop

            // Build bitstring from cubit_vals (MSB-first convention).
            std::string bs(total_c, '0');
            for (ClassicalBitIndex k = 0; k < total_c; ++k) {
                bs[k] = ('0' + cubit_vals[total_c - 1u - k]);
            }
            ++counts[bs];
        }  // end shot loop

        // : statevector = state after the last shot.
        if (num_qubits_ > 0) {
            result_set_statevector(r, get_statevector());
        }
    }

    // Empty circuit or zero cubits: ensure empty string key with nshots count.
    if (total_c == 0) {
        counts[""] = nshots;
    }

    result_set_counts(r, std::move(counts));
    result_set_shots(r, nshots);
    return r;
}

// ── Interactive-mode primitives ───────────────────────────────────────────────

// init_state() — initialise the SoA state to |0...0> (,
// P1+P2+P3).
//
// P3 — buffer reuse: when state_owner_ is the sole reference (use_count==1,
// no live Result holds the previous buffer) and dim matches, we re-use the
// existing allocation and skip the malloc/free round-trip. Otherwise a fresh
// CpuStateBuffers is created (safe: any Result that still holds the old buffer
// via its cpu_owner_ copy will not be clobbered, / invariant).
//
// P1 — parallel first-touch zero-init: re_ and im_ are zeroed in parallel with
// schedule(static) matching the gate-kernel layout. Each thread first-touches
// its own pages, placing them on the local NUMA node (2-socket affinity). The
// padding region beyond `dim` is also zeroed so SIMD kernels that over-touch
// the alignment tail never read uninitialised data.
void FullStateCpuBackend::init_state(QubitIndex num_qubits) {
    const StateIndex dim = StateIndex{1} << num_qubits;

    // P3: reuse buffer when this backend is the sole owner and dim matches.
    if (!state_owner_ || state_owner_.use_count() != 1 || state_owner_->dim != dim) {
        auto buf = std::make_shared<CpuStateBuffers>();
        std::size_t padded_count_re = 0, padded_count_im = 0;
        buf->re  = alloc_aligned(dim, &padded_count_re);
        buf->im  = alloc_aligned(dim, &padded_count_im);
        buf->dim = dim;
        state_owner_ = std::move(buf);
    }

    re_ = state_owner_->re.get();
    im_ = state_owner_->im.get();

    // P1: parallel first-touch zero over [0, dim+padding). Padding region is
    // zeroed to protect SIMD kernels from reading uninitialised tail bytes.
    // Compute padded count from the raw allocation size (same formula as alloc_aligned).
    const std::size_t align        = 64;
    const std::size_t raw_bytes    = static_cast<std::size_t>(dim) * sizeof(double);
    const std::size_t padded_bytes = (raw_bytes + align - 1) & ~(align - 1);
    const std::int64_t padded_n    = static_cast<std::int64_t>(padded_bytes / sizeof(double));

#pragma omp parallel for schedule(static)
    for (std::int64_t i = 0; i < padded_n; ++i) {
        re_[i] = 0.0;
        im_[i] = 0.0;
    }

    // Set |0...0> amplitude to 1 (after the parallel zero so no data race).
    re_[0]      = 1.0;
    num_qubits_ = num_qubits;
    dim_        = dim;
}

// ── NaN/Inf + norm validation helper ───────────────────────────────

// verify_state_finite_and_normalized() — checks all amplitudes are finite and
// that sum |amp_i|^2 == 1 within tolerance.  Called by execute() after each
// gate (reuses this). op_index appears in the error message so the
// caller can diagnose which gate caused numerical divergence.
void FullStateCpuBackend::verify_state_finite_and_normalized(std::size_t op_index) {
    if (!re_) {
        throw SimulatorError{"FullState", "verify_state called before init_state"};
    }

    double norm = 0.0;
    const std::int64_t idim = static_cast<std::int64_t>(dim_);

    // Reduction loop: sum |amp|^2 with OpenMP parallel reduction.
#pragma omp parallel for reduction(+:norm) schedule(static)
    for (std::int64_t i = 0; i < idim; ++i) {
        const double r = re_[i], im = im_[i];
        // Early NaN/Inf detection via isfinite; can be caught here inside OMP.
        // We accumulate to norm so the main check below also handles Inf case.
        if (!std::isfinite(r) || !std::isfinite(im)) {
            // Cannot throw directly from an OpenMP parallel region.
            // Set norm to a sentinel so the post-check fires.
            norm += 2.0;  // Will push norm well outside [1-eps, 1+eps].
        } else {
            norm += std::fma(r, r, im * im);
        }
    }

    if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-6) {
        throw SimulatorError{
            "FullState",
            "numerical divergence at op " + std::to_string(op_index) +
            ": |psi|^2 = " + std::to_string(norm)};
    }
}

// ── apply_gate — gate dispatch and kernel invocation ──────────────────

void FullStateCpuBackend::apply_gate(const GateOp& op) {
    if (!re_) {
        throw SimulatorError{"FullState", "apply_gate called before init_state"};
    }

    const GateType type = op.type;

    // Reset and Opaque are not handled here (and future Phase).
    if (type == GateType::Reset) {
        throw NotImplementedError{"FullState", "Phase 5 pending"};
    }
    if (type == GateType::Opaque) {
        throw NotImplementedError{"FullState", "Phase 5 pending"};
    }

    // Dispatch SWAP-type gates directly; they do not use the 2x2 matrix path.
    if (type == GateType::SWAP) {
        apply_swap_kernel(re_, im_, dim_,
                          op.qubits[0], op.qubits[1],
                          /*control_mask=*/StateIndex{0});
        return;
    }
    if (type == GateType::CSwap) {
        // Fredkin: control=qubits[0], swap qubits[1] and qubits[2].
        const StateIndex ctrl_mask = StateIndex{1} << op.qubits[0];
        apply_swap_kernel(re_, im_, dim_,
                          op.qubits[1], op.qubits[2],
                          ctrl_mask);
        return;
    }
    // ECR: genuine 4x4, applied via the dense-2Q kernel.
    if (type == GateType::ECR) {
        apply_dense_2q_kernel(re_, im_, dim_, op.qubits[0], op.qubits[1],
                              ecr_matrix_data().data());
        return;
    }
    // Google native 2Q gates: genuine 4x4 applied via the dense-2Q kernel.
    if (type == GateType::ISWAP) {
        apply_dense_2q_kernel(re_, im_, dim_, op.qubits[0], op.qubits[1],
                              iswap_matrix_data().data());
        return;
    }
    if (type == GateType::SQRT_ISWAP) {
        apply_dense_2q_kernel(re_, im_, dim_, op.qubits[0], op.qubits[1],
                              sqrt_iswap_matrix_data().data());
        return;
    }
    if (type == GateType::FSIM) {
        // fSim is parametric: compute matrix from params[0]=theta, params[1]=phi.
        const auto m = fsim_matrix_data(op.params[0], op.params[1]);
        apply_dense_2q_kernel(re_, im_, dim_, op.qubits[0], op.qubits[1], m.data());
        return;
    }

    // All remaining gate types reduce to a controlled-1Q application.
    // Derive the target-qubit sub-unitary and control mask.
    const Mat2x2 U = gate_matrix(type, op.params);

    // Build control mask from the appropriate qubit positions.
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

    // Dispatch: blocked path uses L2-tiled traversal; unblocked path
    // uses the group-parallel SIMD path.  Both produce identical results.
    // Cache blocking applies to 1Q/2Q gates only.
    // 3Q/4Q fused unitaries bypass this via apply_fused_program.
    if (cache_blocking_enabled_) {
        blocked_kernel()(re_, im_, dim_, target, control_mask, U);
    } else {
        ctrl_kernel()(re_, im_, dim_, target, control_mask, U);
    }
}

// apply_measure_rng() — core Born-rule collapse kernel with caller-supplied RNG.
// Shared by the public apply_measure() (standalone/interactive use) and by
// execute()'s per-shot loop (which maintains a single RNG stream per shot for
// reproducibility across multiple measurements in one shot).
int FullStateCpuBackend::apply_measure_rng(QubitIndex qubit,
                                            std::mt19937_64& rng) {
    if (!re_) {
        throw SimulatorError{"FullState", "apply_measure called before init_state"};
    }
    if (qubit >= num_qubits_) {
        throw SimulatorError{"FullState", "apply_measure: qubit index out of range"};
    }

    // Accumulate probability that qubit == 1.
    double p1 = 0.0;
    const std::int64_t idim = static_cast<std::int64_t>(dim_);
#pragma omp parallel for reduction(+:p1) schedule(static)
    for (std::int64_t ii = 0; ii < idim; ++ii) {
        const StateIndex i = static_cast<StateIndex>(ii);
        if ((i >> qubit) & StateIndex{1}) {
            p1 += std::fma(re_[ii], re_[ii], im_[ii] * im_[ii]);
        }
    }

    // Sample measurement outcome from caller's RNG stream.
    std::uniform_real_distribution<double> uniform01(0.0, 1.0);
    const double u        = uniform01(rng);
    const int    outcome  = (u < p1) ? 1 : 0;
    const double p_out    = (outcome == 1) ? p1 : (1.0 - p1);

    if (p_out < 1e-15) {
        throw SimulatorError{"FullState",
            "apply_measure: p_outcome near zero (numerical underflow)"};
    }

    // Collapse and renormalise surviving subspace.
    const double norm_inv = 1.0 / std::sqrt(p_out);
    const int    keep_bit = outcome;
#pragma omp parallel for schedule(static)
    for (std::int64_t ii = 0; ii < idim; ++ii) {
        const StateIndex i   = static_cast<StateIndex>(ii);
        const int        bit = static_cast<int>((i >> qubit) & StateIndex{1});
        if (bit != keep_bit) {
            re_[ii] = 0.0;
            im_[ii] = 0.0;
        } else {
            re_[ii] *= norm_inv;
            im_[ii] *= norm_inv;
        }
    }

    return outcome;
}

// apply_reset_rng() — project qubit to |0⟩ using a caller-supplied RNG.
void FullStateCpuBackend::apply_reset_rng(QubitIndex qubit,
                                           std::mt19937_64& rng) {
    const int b = apply_measure_rng(qubit, rng);
    if (b == 1) {
        GateOp x_op;
        x_op.type   = GateType::X;
        x_op.qubits = {qubit};
        apply_gate(x_op);
    }
}

// apply_measure() — public interface; creates a local RNG seeded from
// master_seed_ so standalone (interactive / unit-test) calls are reproducible
// when a seed has been set, and random otherwise.
int FullStateCpuBackend::apply_measure(QubitIndex qubit) {
    const std::uint64_t seed =
        master_seed_ != 0
            ? qplayer::detail::splitmix64(master_seed_ ^
                  (static_cast<std::uint64_t>(qubit) << 16))
            : static_cast<std::uint64_t>(std::random_device{}());
    std::mt19937_64 local_rng(seed);
    return apply_measure_rng(qubit, local_rng);
}

// apply_reset() — public interface; delegates to apply_reset_rng with a local
// RNG. X is applied after collapse so the qubit is forced to |0⟩.
void FullStateCpuBackend::apply_reset(QubitIndex qubit) {
    if (!re_) {
        throw SimulatorError{"FullState", "apply_reset called before init_state"};
    }
    const std::uint64_t seed =
        master_seed_ != 0
            ? qplayer::detail::splitmix64(master_seed_ ^
                  (static_cast<std::uint64_t>(qubit) << 24))
            : static_cast<std::uint64_t>(std::random_device{}());
    std::mt19937_64 local_rng(seed);
    apply_reset_rng(qubit, local_rng);
}

// set_seed() — override the master PRNG seed for reproducible sampling.
void FullStateCpuBackend::set_seed(std::uint64_t seed) {
    master_seed_ = seed;
}

void FullStateCpuBackend::set_fusion(bool enable, std::uint32_t max_width) {
    fusion_enabled_   = enable;
    fusion_max_width_ = max_width;
}

void FullStateCpuBackend::set_cache_blocking(bool enable) {
    cache_blocking_enabled_ = enable;
}

// ── State access ──────────────────────────────────────────────────────────────

// get_statevector() — SoA -> AoS conversion.
StateVector FullStateCpuBackend::get_statevector() {
    if (!re_) {
        throw SimulatorError{"FullState", "get_statevector before init_state"};
    }
    // The AoS copy doubles memory on top of the resident SoA, so check the
    // extra 2^n x 16 against the live budget rather than a fixed cap
    // MAX_STATEVECTOR_QUBITS now governs only SparseState/TN densification,
    // not FullState dense materialisation.
    qplayer::detail::check_memory_budget(
        qplayer::detail::peak_state_bytes(num_qubits_,
                                          qplayer::detail::WorkingSet::Materialize),
        "FullState",
        num_qubits_);

    StateVector out(static_cast<std::size_t>(dim_));
    for (StateIndex i = 0; i < dim_; ++i) {
        out[static_cast<std::size_t>(i)] =
            std::complex<double>(re_[i], im_[i]);
    }
    return out;
}

// get_statevector_raw() — read-only SoA view.
RawStateView FullStateCpuBackend::get_statevector_raw() {
    if (!re_) {
        throw SimulatorError{"FullState", "get_statevector_raw before init_state"};
    }
    return RawStateView{re_, im_, dim_};
}

// nonzero_amplitudes() — enumerate (index, amplitude) pairs with |amp| >=
// kNonzeroAmplitudeThreshold in ascending index order without densifying or
// converting to AoS. The threshold (1e-12) matches
// SparseState's default truncation, so the nonzero set is identical across
// backends and floating-point dust (~1e-17) is excluded. Compares squared
// magnitudes to avoid a sqrt; mathematically equivalent to |amp| >= thr.
// Scans the SoA arrays in index order 0..dim_-1 so output is naturally sorted.
// Returns an empty vector before init_state() (re_ is null; no throw).
std::vector<std::pair<StateIndex, std::complex<double>>>
FullStateCpuBackend::nonzero_amplitudes() const {
    std::vector<std::pair<StateIndex, std::complex<double>>> result;
    if (!re_) {
        return result;  // Not yet initialised — defined as empty.
    }
    constexpr double thr2 =
        kNonzeroAmplitudeThreshold * kNonzeroAmplitudeThreshold;
    for (StateIndex i = 0; i < dim_; ++i) {
        const double re = re_[static_cast<std::size_t>(i)];
        const double im = im_[static_cast<std::size_t>(i)];
        if (re * re + im * im >= thr2) {
            result.emplace_back(i, std::complex<double>(re, im));
        }
    }
    return result;
}

// num_qubits() — active qubit count (0 before init_state). Used by
// dump_statevector() to format bitstring width without densifying.
QubitIndex FullStateCpuBackend::num_qubits() const {
    return num_qubits_;
}

// expectation_value() — <psi|P|psi> for a Pauli string P.
// Pauli-string qubit order is Qiskit-standard (decided 2026-06-02):
// the leftmost char pauli[0] acts on the highest qubit (n-1), pauli[n-1] on
// qubit 0. P is Hermitian so the result is real; <psi|P|psi> reduces to
//   sum_j conj(psi[j ^ flip]) * coeff(j) * psi[j],
// where flip flips the X/Y qubits and coeff(j) = i^num_Y * (-1)^popcount(j&sign).
double FullStateCpuBackend::expectation_value(std::string_view pauli) {
    if (!re_) {
        throw SimulatorError{"FullState", "expectation_value before init_state"};
    }
    // An empty Pauli string defaults to all-Z over every qubit: <Z...Z>
    // Built here because the backend owns num_qubits_; all-Z is
    // permutation-symmetric so the qubit order is irrelevant. The local
    // string outlives the view for the rest of this call.
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
    StateIndex flip_mask = 0;
    StateIndex sign_mask = 0;
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
    double ph_re = 1.0, ph_im = 0.0;
    switch (num_y & 3) {
        case 0: ph_re = 1.0;  ph_im = 0.0;  break;
        case 1: ph_re = 0.0;  ph_im = 1.0;  break;
        case 2: ph_re = -1.0; ph_im = 0.0;  break;
        case 3: ph_re = 0.0;  ph_im = -1.0; break;
    }

    const std::int64_t idim = static_cast<std::int64_t>(dim_);
    double acc_re = 0.0, acc_im = 0.0;

#pragma omp parallel for reduction(+:acc_re,acc_im) schedule(static)
    for (std::int64_t jj = 0; jj < idim; ++jj) {
        const StateIndex j  = static_cast<StateIndex>(jj);
        const StateIndex jp = j ^ flip_mask;
        // Parity over sign-mask bits; n <= ~35 so the low 64 bits hold all of them.
        const int parity = __builtin_popcountll(
            static_cast<std::uint64_t>(j & sign_mask)) & 1;
        const double sgn  = parity ? -1.0 : 1.0;
        const double c_re = sgn * ph_re;
        const double c_im = sgn * ph_im;
        const double bj_re = re_[j],  bj_im = im_[j];
        const double cp_re = re_[jp], cp_im = -im_[jp];  // conj(psi[jp])
        const double t1_re = c_re * bj_re - c_im * bj_im;
        const double t1_im = c_re * bj_im + c_im * bj_re;
        acc_re += cp_re * t1_re - cp_im * t1_im;
        acc_im += cp_re * t1_im + cp_im * t1_re;
    }

    // P Hermitian -> acc_im is numerical noise (~0); return the real part.
    return acc_re;
}

// ── Metadata ──────────────────────────────────────────────────────────────────

SimType          FullStateCpuBackend::sim_type()       const { return SimType::FullState; }
bool             FullStateCpuBackend::supports_noise() const { return true;  }
bool             FullStateCpuBackend::supports_gpu()   const { return false; }
std::string_view FullStateCpuBackend::name()           const { return "FullState"; }

// max_qubits() — host peak working-set model.
std::uint32_t FullStateCpuBackend::max_qubits(detail::WorkingSet ws) const noexcept {
    return qplayer::detail::max_qubits_for_budget(ws);
}

// ── Op-variant dispatcher ─────────────────────────────────────────

// apply_op() routes each IR node to the appropriate backend primitive.
// Fusion boundaries (BarrierOp, MeasureOp, IfOp, IdleOp) are never merged
//
void FullStateCpuBackend::apply_op(const Op& op) {
    std::visit([this](auto&& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, GateOp>) {
            apply_gate(node);

        } else if constexpr (std::is_same_v<T, MeasureOp>) {
            apply_measure(node.qubit);

        } else if constexpr (std::is_same_v<T, BarrierOp>) {
            // Fusion boundary only; no state change.
            (void)node;

        } else if constexpr (std::is_same_v<T, IfOp>) {
            // IfOp classical-conditional dispatch implemented in.
            // execute() handles IfOp inline in the per-shot loop; apply_op()
            // is also called from apply_fused_program() which never sees IfOp
            // (fusion boundaries prevent merging across IfOp).
            // Interactive mode: execute body unconditionally if creg value
            // cannot be evaluated here (no classical register state).
            // For safety, throw a SimulatorError to force callers to use
            // execute() for IfOp circuits (design boundary).
            throw SimulatorError{"FullState",
                "apply_op: IfOp must be handled by execute() per-shot loop "
                "(use QRegister::run() for circuits with classical control)"};

        } else if constexpr (std::is_same_v<T, IdleOp>) {
            apply_idle(node);
        }
    }, op);
}

// apply_idle() — Phase 5 dispatch slot only. Amplitude Damping + Dephasing
// Kraus channels activated in Phase 8.
void FullStateCpuBackend::apply_idle(const IdleOp& /*op*/) {
}

// ── Runtime Gate Fusion seam ─────────────────────────────────

// apply_fused_program() — traverse a fused program from detail::fuse_circuit()
// and apply each FusedGate to the current SoA state vector.  Boundary ops
// (Measure/Barrier/If/Idle) are no-ops here; execute() handles them.
void FullStateCpuBackend::apply_fused_program(
        const std::vector<qplayer::detail::FusedOp>& prog) {
    if (!re_) {
        throw SimulatorError{"FullState",
            "apply_fused_program called before init_state"};
    }
    for (const qplayer::detail::FusedOp& fop : prog) {
        std::visit([this](auto&& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, qplayer::detail::FusedGate>) {
                qplayer::detail::apply_fused_gate(
                    re_, im_, dim_, node);
            } else if constexpr (std::is_same_v<T, GateOp>) {
                // Passthrough permutation gate: Swap/CSwap excluded
                // from the dense GEMV window -> apply via the dedicated kernel.
                apply_gate(node);
            } else {
                // Boundary ops (Measure/Barrier/If/Idle): no-op at fusion seam.
                // Full handling is in execute().
                (void)node;
            }
        }, fop);
    }
}

}  // namespace qplayer
