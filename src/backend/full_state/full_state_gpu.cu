/// @file full_state_gpu.cu
/// @brief FullState GPU CUDA kernels.
///
/// Compiled only when QPLAYER_BUILD_GPU=ON (CMakeLists). The matching.cpp
/// stub stays free of CUDA headers — the one-way include policy means a .cpp
/// never includes this file; it links against the extern "C" launchers below.
///
/// lands the real 1Q/2Q gate applicators on a SoA state vector plus
/// self-contained host launchers that own a transient device buffer
/// (cudaMalloc → H2D → kernel → D2H → free). GPU-resident persistence, pinned
/// host buffers, CUDA Streams, and the VRAM pre-check are deferred to /
/// cudaGetDeviceCount() is also surfaced here so factory.cpp can run
/// the runtime probe without dragging in cuda_runtime.h.

#include <cuda_runtime.h>
#include <curand_kernel.h>
#include <thrust/device_ptr.h>          // GPU-resident sampling CDF
#include <thrust/execution_policy.h>
#include <thrust/scan.h>                // thrust::inclusive_scan — device CDF

#include <cstddef>

namespace qplayer::cuda {

// Complex multiply (ur+i·ui)·(ar+i·ai) accumulated into (or,oi). __device__
// inline so both kernels share one definition without a separate TU.
__device__ inline void cmul_add(double ur, double ui,
                                double ar, double ai,
                                double& or_acc, double& oi_acc) {
    or_acc += ur * ar - ui * ai;
    oi_acc += ur * ai + ui * ar;
}

// Apply a single-qubit 2x2 unitary U to a SoA state vector resident on the
// device. One thread handles one amplitude pair (i0, i1) where i1 = i0 | stride
// and stride = 1<<target. The grid spans dim/2 pairs.
__global__ void apply_1q_gate_kernel(double* re, double* im,
                                     unsigned long long dim,
                                     unsigned int target,
                                     double u00_re, double u00_im,
                                     double u01_re, double u01_im,
                                     double u10_re, double u10_im,
                                     double u11_re, double u11_im) {
    const unsigned long long pair = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned long long pairs = dim >> 1;
    if (pair >= pairs) return;

    const unsigned long long stride = 1ULL << target;
    // Reconstruct the |target=0> index: insert a 0 bit at position `target`.
    const unsigned long long i0 =
        ((pair >> target) << (target + 1)) | (pair & (stride - 1));
    const unsigned long long i1 = i0 | stride;

    const double a0r = re[i0], a0i = im[i0];
    const double a1r = re[i1], a1i = im[i1];

    double n0r = 0.0, n0i = 0.0, n1r = 0.0, n1i = 0.0;
    cmul_add(u00_re, u00_im, a0r, a0i, n0r, n0i);
    cmul_add(u01_re, u01_im, a1r, a1i, n0r, n0i);
    cmul_add(u10_re, u10_im, a0r, a0i, n1r, n1i);
    cmul_add(u11_re, u11_im, a1r, a1i, n1r, n1i);

    re[i0] = n0r; im[i0] = n0i;
    re[i1] = n1r; im[i1] = n1i;
}

// Apply a controlled single-qubit unitary: U acts on `target` only for basis
// states where `control` is 1. Covers CX/CY/CZ/CH/CRZ/CU1/CU3 (baseline).
// One thread handles one (control=1) amplitude pair; the grid spans dim/4.
__global__ void apply_2q_gate_kernel(double* re, double* im,
                                     unsigned long long dim,
                                     unsigned int control,
                                     unsigned int target,
                                     double u00_re, double u00_im,
                                     double u01_re, double u01_im,
                                     double u10_re, double u10_im,
                                     double u11_re, double u11_im) {
    const unsigned long long quad = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned long long quads = dim >> 2;
    if (quad >= quads) return;

    // Lay out the two free bits (control, target) deterministically: low bit of
    // the gather index maps to the lower-numbered qubit so the reconstruction
    // is unambiguous regardless of control/target ordering.
    const unsigned int lo = control < target ? control : target;
    const unsigned int hi = control < target ? target : control;
    const unsigned long long lo_mask = (1ULL << lo) - 1ULL;
    const unsigned long long mid_mask = ((1ULL << (hi - 1)) - 1ULL) & ~lo_mask;

    // Insert two 0 bits at positions `lo` and `hi` to form the base index.
    unsigned long long base = (quad & lo_mask)
        | ((quad & mid_mask) << 1)
        | ((quad >> (hi - 1)) << (hi + 1));

    const unsigned long long cbit = 1ULL << control;
    const unsigned long long tbit = 1ULL << target;
    const unsigned long long i0 = base | cbit;          // control=1, target=0
    const unsigned long long i1 = base | cbit | tbit;   // control=1, target=1

    const double a0r = re[i0], a0i = im[i0];
    const double a1r = re[i1], a1i = im[i1];

    double n0r = 0.0, n0i = 0.0, n1r = 0.0, n1i = 0.0;
    cmul_add(u00_re, u00_im, a0r, a0i, n0r, n0i);
    cmul_add(u01_re, u01_im, a1r, a1i, n0r, n0i);
    cmul_add(u10_re, u10_im, a0r, a0i, n1r, n1i);
    cmul_add(u11_re, u11_im, a1r, a1i, n1r, n1i);

    re[i0] = n0r; im[i0] = n0i;
    re[i1] = n1r; im[i1] = n1i;
}

// Initialise one cuRAND Philox state per Monte Carlo trajectory.
// Each trajectory j gets the host-derived seed seeds[j] = splitmix64(master ^
// (j<<32)) (same formula as the CPU derivation); subsequence/offset
// are 0 since the seed already decorrelates trajectories.
__global__ void init_curand_states_kernel(curandStatePhilox4_32_10* states,
                                           const unsigned long long* seeds,
                                           int n_traj) {
    const int j = blockIdx.x * blockDim.x + threadIdx.x;
    if (j >= n_traj) return;
    curand_init(seeds[j], 0ULL, 0ULL, &states[j]);
}

// ── GPU-resident sampling kernels ──────────────────────────
// prob[i] = |psi_i|^2 = re^2 + im^2. One thread per amplitude. Feeds the device
// CDF (thrust::inclusive_scan) so the nshots draw never touches host memory —
// counts-only workloads skip the 2GiB D2H + AoS materialization (: 97.4%
// of q27 counts() time was host handoff). Not normalised here; the draw kernel
// scales each uniform by the unnormalised total (= CDF[dim-1]) instead, which
// reproduces the CPU upper_bound(normalised_cdf, u) result (parity).
__global__ void prob_kernel(const double* re, const double* im,
                            double* prob, unsigned long long dim) {
    const unsigned long long i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= dim) return;
    const double r = re[i];
    const double m = im[i];
    prob[i] = fma(r, r, m * m);   // match full_state_cpu.cpp / result.cpp std::fma
}

// Inverse-CDF draw: for each host-supplied uniform u in [0,1), find the first
// basis index k with cdf[k] > u*total (upper_bound). One thread per shot; the
// uniforms come from the host std::mt19937_64(seed) stream (same as the CPU
// path) so a fixed seed reproduces a statistically identical distribution.
__global__ void draw_kernel(const double* cdf, unsigned long long dim,
                            const double* uniforms, unsigned long long nshots,
                            double total, unsigned long long* out_basis) {
    const unsigned long long s = blockIdx.x * blockDim.x + threadIdx.x;
    if (s >= nshots) return;
    const double target = uniforms[s] * total;
    // Binary search for the first index whose cumulative prob exceeds target.
    unsigned long long lo = 0, hi = dim;       // [lo, hi)
    while (lo < hi) {
        const unsigned long long mid = lo + ((hi - lo) >> 1);
        if (cdf[mid] > target) hi = mid;
        else                   lo = mid + 1;
    }
    out_basis[s] = lo < dim ? lo : dim - 1;    // clamp (u*total == total edge)
}

namespace {

constexpr int kBlockSize = 256;

// Allocate a transient device SoA buffer, copy the host state in, and report
// the first CUDA error (cudaFree(nullptr) is a safe no-op on the failure path).
cudaError_t upload_state(const double* h_re, const double* h_im,
                         unsigned long long dim,
                         double*& d_re, double*& d_im) {
    const size_t bytes = static_cast<size_t>(dim) * sizeof(double);
    const cudaError_t e_re = cudaMalloc(&d_re, bytes);
    const cudaError_t e_im = cudaMalloc(&d_im, bytes);
    if (e_re != cudaSuccess || e_im != cudaSuccess) {
        cudaFree(d_re);
        cudaFree(d_im);
        return e_re != cudaSuccess ? e_re : e_im;
    }
    const cudaError_t c_re = cudaMemcpy(d_re, h_re, bytes, cudaMemcpyHostToDevice);
    if (c_re != cudaSuccess) return c_re;
    return cudaMemcpy(d_im, h_im, bytes, cudaMemcpyHostToDevice);
}

// Synchronise, copy the device state back to the host, and free the buffer.
// Returns the first error encountered along the way.
cudaError_t download_and_free(double* h_re, double* h_im,
                              unsigned long long dim,
                              double* d_re, double* d_im) {
    const size_t bytes = static_cast<size_t>(dim) * sizeof(double);
    cudaError_t e = cudaGetLastError();          // surface launch errors
    if (e == cudaSuccess) e = cudaDeviceSynchronize();
    if (e == cudaSuccess) e = cudaMemcpy(h_re, d_re, bytes, cudaMemcpyDeviceToHost);
    if (e == cudaSuccess) e = cudaMemcpy(h_im, d_im, bytes, cudaMemcpyDeviceToHost);
    cudaFree(d_re);
    cudaFree(d_im);
    return e;
}

// ── Fused k-qubit GEMV kernel (on GPU) ─────────────────────
// Generalises apply_dense_2q_gate_kernel to a dense 2^K x 2^K unitary. One
// thread owns one base block: it gathers 2^K amplitudes into registers, runs the
// dense complex GEMV, and scatters them back.
//
// Index convention matches the CPU path exactly (gate_fusion.hpp): `qubits` is
// sorted ascending, local index bit b selects qubits[b], and the matrix is
// row-major with out[r] = sum_c M[r][c] * amp[c].
//
// K is a compile-time constant so the amplitude arrays stay in registers; a
// runtime K would spill them to local memory and give up the whole point.
// The matrix lives in global memory and is served from L2: every block reads the
// same elements in the same order, so the hit rate is effectively 1. __constant__
// would force a cudaMemcpyToSymbol per gate, breaking the single-stream async
// pipeline.
template <int K>
__global__ void apply_fused_kq_kernel(double* re, double* im,
                                      unsigned long long nblocks,
                                      const unsigned int* __restrict__ qubits,
                                      const double* __restrict__ mat) {
    const unsigned long long b = blockIdx.x * blockDim.x + threadIdx.x;
    if (b >= nblocks) return;

    constexpr int kDim = 1 << K;

    // Rebuild the base index by inserting a 0 bit at each gate qubit position.
    // Ascending order matters: inserting at the lower position first keeps the
    // higher positions valid in the partially expanded index.
    unsigned long long base = b;
    for (int i = 0; i < K; ++i) {
        const unsigned long long lo_mask = (1ULL << qubits[i]) - 1ULL;
        const unsigned long long lo      = base & lo_mask;
        base = ((base ^ lo) << 1) | lo;
    }

    // Bit positions as a register-resident copy: `qubits` is global memory and
    // the offset helper below reads it 2^K times per thread.
    unsigned int qb[K];
    for (int i = 0; i < K; ++i) qb[i] = qubits[i];

    // Offset contributed by local pattern p, recomputed on demand. Caching all
    // 2^K offsets in an array costs 2^K more 64-bit registers per thread, which
    // at K=4 pushes past the point where occupancy collapses — recomputing a few
    // shifts is far cheaper than the spill.
    const auto offset_of = [&](int p) {
        unsigned long long o = 0;
        for (int i = 0; i < K; ++i) {
            if ((p >> i) & 1) o |= 1ULL << qb[i];
        }
        return o;
    };

    double ar[kDim], ai[kDim];
    for (int p = 0; p < kDim; ++p) {
        const unsigned long long idx = base + offset_of(p);
        ar[p] = re[idx];
        ai[p] = im[idx];
    }

    for (int r = 0; r < kDim; ++r) {
        double nr = 0.0, ni = 0.0;
        for (int c = 0; c < kDim; ++c) {
            cmul_add(mat[2 * (r * kDim + c)], mat[2 * (r * kDim + c) + 1],
                     ar[c], ai[c], nr, ni);
        }
        const unsigned long long idx = base + offset_of(r);
        re[idx] = nr;
        im[idx] = ni;
    }
}

}  // namespace

extern "C" {

// Runtime GPU probe for factory.cpp::gpu_build_available(). Returns
// the visible CUDA device count, or 0 when the runtime reports an error so the
// factory falls back to CPU without ever touching cuda_runtime.h itself.
int qplayer_cuda_device_count() {
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) return 0;
    return n;
}

// ── GPU-resident state vector + pinned host memory ─────────
// All launchers return a cudaError_t (0 == cudaSuccess) so the .cpp backend can
// wrap failures in GPUError without including cuda_runtime.h.

// cudaMemGetInfo wrapper for the VRAM budget term. On error the
// outputs are zeroed and the cudaError_t is returned so the caller can disable
// the precheck (line 113).
int qplayer_cuda_mem_get_info(unsigned long long* free_out,
                              unsigned long long* total_out) {
    std::size_t f = 0, t = 0;
    const cudaError_t e = cudaMemGetInfo(&f, &t);
    if (e != cudaSuccess) { *free_out = 0; *total_out = 0; return e; }
    *free_out  = static_cast<unsigned long long>(f);
    *total_out = static_cast<unsigned long long>(t);
    return cudaSuccess;
}

// Allocate the GPU-resident SoA state vector (2 x 2^n x 8 bytes). On failure
// both pointers are freed (cudaFree(nullptr) is a no-op) and left null.
int qplayer_cuda_alloc_state(double** d_re, double** d_im,
                             unsigned long long dim) {
    *d_re = nullptr;
    *d_im = nullptr;
    const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
    const cudaError_t e_re = cudaMalloc(d_re, bytes);
    const cudaError_t e_im = cudaMalloc(d_im, bytes);
    if (e_re != cudaSuccess || e_im != cudaSuccess) {
        cudaFree(*d_re); cudaFree(*d_im);
        *d_re = nullptr; *d_im = nullptr;
        return e_re != cudaSuccess ? e_re : e_im;
    }
    return cudaSuccess;
}

// Free the GPU-resident state vector. Null pointers are safe no-ops.
void qplayer_cuda_free_state(double* d_re, double* d_im) {
    cudaFree(d_re);
    cudaFree(d_im);
}

// Initialise the device state to |0...0>: zero both arrays, then set re[0]=1.
int qplayer_cuda_init_zero_state(double* d_re, double* d_im,
                                 unsigned long long dim) {
    const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
    cudaError_t e = cudaMemset(d_re, 0, bytes);
    if (e == cudaSuccess) e = cudaMemset(d_im, 0, bytes);
    if (e != cudaSuccess) return e;
    const double one = 1.0;
    return cudaMemcpy(d_re, &one, sizeof(double), cudaMemcpyHostToDevice);
}

// Allocate pinned host buffers (cudaMallocHost) for the final device->host copy
// (max transfer bandwidth). Symmetric free path.
int qplayer_cuda_alloc_pinned(double** h_re, double** h_im,
                              unsigned long long dim) {
    *h_re = nullptr;
    *h_im = nullptr;
    const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
    const cudaError_t e_re = cudaMallocHost(h_re, bytes);
    const cudaError_t e_im = cudaMallocHost(h_im, bytes);
    if (e_re != cudaSuccess || e_im != cudaSuccess) {
        cudaFreeHost(*h_re); cudaFreeHost(*h_im);
        *h_re = nullptr; *h_im = nullptr;
        return e_re != cudaSuccess ? e_re : e_im;
    }
    return cudaSuccess;
}

void qplayer_cuda_free_pinned(double* h_re, double* h_im) {
    cudaFreeHost(h_re);
    cudaFreeHost(h_im);
}

// Final device->host copy (the single GPU->Host transfer of the GPU-resident
// policy). Synchronises first so all queued kernels have completed.
int qplayer_cuda_download_state(double* h_re, double* h_im,
                                const double* d_re, const double* d_im,
                                unsigned long long dim) {
    const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
    cudaError_t e = cudaDeviceSynchronize();
    if (e == cudaSuccess) e = cudaMemcpy(h_re, d_re, bytes, cudaMemcpyDeviceToHost);
    if (e == cudaSuccess) e = cudaMemcpy(h_im, d_im, bytes, cudaMemcpyDeviceToHost);
    return e;
}

// Self-contained 1Q launcher. `u8` packs u00,u01,u10,u11 as (re,im) pairs.
// Returns a cudaError_t (0 == success) so the .cpp caller can wrap it in
// GPUError once the GPU-resident path lands.
int qplayer_cuda_apply_1q(double* h_re, double* h_im,
                          unsigned long long dim,
                          unsigned int target,
                          const double* u8) {
    double* d_re = nullptr;
    double* d_im = nullptr;
    const cudaError_t up = upload_state(h_re, h_im, dim, d_re, d_im);
    if (up != cudaSuccess) { cudaFree(d_re); cudaFree(d_im); return up; }

    const unsigned long long pairs = dim >> 1;
    const unsigned int blocks =
        static_cast<unsigned int>((pairs + kBlockSize - 1) / kBlockSize);
    apply_1q_gate_kernel<<<blocks, kBlockSize>>>(
        d_re, d_im, dim, target,
        u8[0], u8[1], u8[2], u8[3], u8[4], u8[5], u8[6], u8[7]);

    return download_and_free(h_re, h_im, dim, d_re, d_im);
}

// Self-contained controlled-U launcher (control + target + 2x2 unitary).
int qplayer_cuda_apply_2q(double* h_re, double* h_im,
                          unsigned long long dim,
                          unsigned int control,
                          unsigned int target,
                          const double* u8) {
    double* d_re = nullptr;
    double* d_im = nullptr;
    const cudaError_t up = upload_state(h_re, h_im, dim, d_re, d_im);
    if (up != cudaSuccess) { cudaFree(d_re); cudaFree(d_im); return up; }

    const unsigned long long quads = dim >> 2;
    const unsigned int blocks =
        static_cast<unsigned int>((quads + kBlockSize - 1) / kBlockSize);
    apply_2q_gate_kernel<<<blocks, kBlockSize>>>(
        d_re, d_im, dim, control, target,
        u8[0], u8[1], u8[2], u8[3], u8[4], u8[5], u8[6], u8[7]);

    return download_and_free(h_re, h_im, dim, d_re, d_im);
}

// ── CUDA stream lifecycle ─────────────────────────────────────────────────────
// The backend holds a stream handle as a uint64_t to keep cuda_runtime.h out of
// the .cpp (one-way include policy). The launchers below convert to/from
// cudaStream_t (opaque pointer). A single stream per backend instance ensures
// all gate kernels execute in order so data-races between overlapping kernels
// are structurally impossible.

int qplayer_cuda_stream_create(unsigned long long* handle_out) {
    cudaStream_t s = nullptr;
    const cudaError_t e = cudaStreamCreate(&s);
    if (e != cudaSuccess) { *handle_out = 0; return e; }
    // cudaStream_t is a pointer; storing it as uint64_t is safe on all 64-bit
    // CUDA ABIs — the backend never dereferences it in C++.
    *handle_out = reinterpret_cast<unsigned long long>(s);
    return cudaSuccess;
}

void qplayer_cuda_stream_destroy(unsigned long long handle) {
    if (handle == 0) return;
    cudaStreamDestroy(reinterpret_cast<cudaStream_t>(handle));
}

// Block until all kernels submitted to `handle` have completed.
int qplayer_cuda_stream_sync(unsigned long long handle) {
    if (handle == 0) return cudaDeviceSynchronize();
    return cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(handle));
}

// ── Device-resident 1Q/2Q gate launchers ──────────────────────────────
// Unlike the self-contained qplayer_cuda_apply_1q/2q launchers above (which own
// a transient device buffer), these operate directly on the backend's
// GPU-resident d_re/d_im buffers. The stream ensures in-order dispatch so
// consecutive calls on the same state are safely serialised.

int qplayer_cuda_apply_1q_dev(double* d_re, double* d_im,
                               unsigned long long dim,
                               unsigned int target,
                               const double* u8,
                               unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const unsigned long long pairs = dim >> 1;
    const unsigned int blocks =
        static_cast<unsigned int>((pairs + kBlockSize - 1) / kBlockSize);
    apply_1q_gate_kernel<<<blocks, kBlockSize, 0, s>>>(
        d_re, d_im, dim, target,
        u8[0], u8[1], u8[2], u8[3], u8[4], u8[5], u8[6], u8[7]);
    return cudaGetLastError();
}

int qplayer_cuda_apply_2q_dev(double* d_re, double* d_im,
                               unsigned long long dim,
                               unsigned int control,
                               unsigned int target,
                               const double* u8,
                               unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const unsigned long long quads = dim >> 2;
    const unsigned int blocks =
        static_cast<unsigned int>((quads + kBlockSize - 1) / kBlockSize);
    apply_2q_gate_kernel<<<blocks, kBlockSize, 0, s>>>(
        d_re, d_im, dim, control, target,
        u8[0], u8[1], u8[2], u8[3], u8[4], u8[5], u8[6], u8[7]);
    return cudaGetLastError();
}

// ── SWAP kernel (device-resident) ────────────────────────────────────────────
// Swaps amplitudes for all index pairs where bit(i,q_a)==0 and bit(i,q_b)==1.
// Each thread processes one index; the conditional ensures each pair is swapped
// exactly once. control_bit == UINT_MAX means unconditional SWAP; otherwise it
// is the bit position of the control qubit for CSwap (Fredkin).
__global__ void apply_swap_kernel_dev(double* re, double* im,
                                      unsigned long long dim,
                                      unsigned int q_a, unsigned int q_b,
                                      unsigned int control_bit) {
    const unsigned long long i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= dim) return;

    // Process only (q_a=0, q_b=1) entries to avoid double swap.
    if (((i >> q_a) & 1ULL) != 0ULL) return;
    if (((i >> q_b) & 1ULL) != 1ULL) return;

    // Control check for Fredkin gate.
    if (control_bit != 0xFFFFFFFFU) {
        if (((i >> control_bit) & 1ULL) == 0ULL) return;
    }

    const unsigned long long j = (i | (1ULL << q_a)) & ~(1ULL << q_b);
    const double tr = re[i], ti = im[i];
    re[i] = re[j]; im[i] = im[j];
    re[j] = tr;    im[j] = ti;
}

int qplayer_cuda_apply_swap_dev(double* d_re, double* d_im,
                                 unsigned long long dim,
                                 unsigned int q_a, unsigned int q_b,
                                 unsigned int control_bit,
                                 unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const unsigned int blocks =
        static_cast<unsigned int>((dim + kBlockSize - 1) / kBlockSize);
    apply_swap_kernel_dev<<<blocks, kBlockSize, 0, s>>>(
        d_re, d_im, dim, q_a, q_b, control_bit);
    return cudaGetLastError();
}

// ── CCX (Toffoli) kernel (device-resident) ───────────────────────────────────
// Flips `target` for every basis state where BOTH controls are 1 — an exact
// controlled-controlled-X in a single pass, mirroring the CPU control_mask path.
// This replaces an earlier H/T/CX decomposition that was both slow (15 launches
// per CCX) and incorrect (a missing T on the target left the gate non-unitary
// for the firing subspace). Each thread owns one index; the target=0 guard
// swaps each |..0..> <-> |..1..> pair exactly once.
__global__ void apply_ccx_kernel_dev(double* re, double* im,
                                     unsigned long long dim,
                                     unsigned int c0, unsigned int c1,
                                     unsigned int target) {
    const unsigned long long i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= dim) return;
    if (((i >> c0) & 1ULL) == 0ULL) return;          // control 0 must be 1
    if (((i >> c1) & 1ULL) == 0ULL) return;          // control 1 must be 1
    if (((i >> target) & 1ULL) != 0ULL) return;      // pair owned by target=0

    const unsigned long long j = i | (1ULL << target);
    const double tr = re[i], ti = im[i];
    re[i] = re[j]; im[i] = im[j];
    re[j] = tr;    im[j] = ti;
}

int qplayer_cuda_apply_ccx_dev(double* d_re, double* d_im,
                               unsigned long long dim,
                               unsigned int c0, unsigned int c1,
                               unsigned int target,
                               unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const unsigned int blocks =
        static_cast<unsigned int>((dim + kBlockSize - 1) / kBlockSize);
    apply_ccx_kernel_dev<<<blocks, kBlockSize, 0, s>>>(
        d_re, d_im, dim, c0, c1, target);
    return cudaGetLastError();
}

// ── Dense 2-qubit gate kernel (device-resident) ────────────────────
// Applies a full 4x4 unitary to two qubits (qa = operand 0, qb = operand 1).
// Used for ECR and any genuine 2Q gate that is not control+1Q. Matrix is
// row-major, operand-order local bits: local index x = 2*b1 + b0 where
// b0 = bit(qa), b1 = bit(qb). Mirrors apply_2q_gate_kernel's quad enumeration
// but gathers all four amplitudes (no control filter). M.m[8*r+2*c (+1)] = M[r][c].
struct DenseMat4 { double m[32]; };

__global__ void apply_dense_2q_gate_kernel(double* re, double* im,
                                           unsigned long long dim,
                                           unsigned int qa, unsigned int qb,
                                           DenseMat4 M) {
    const unsigned long long quad  = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned long long quads = dim >> 2;
    if (quad >= quads) return;

    const unsigned int lo = qa < qb ? qa : qb;
    const unsigned int hi = qa < qb ? qb : qa;
    const unsigned long long lo_mask  = (1ULL << lo) - 1ULL;
    const unsigned long long mid_mask = ((1ULL << (hi - 1)) - 1ULL) & ~lo_mask;
    const unsigned long long base = (quad & lo_mask)
        | ((quad & mid_mask) << 1)
        | ((quad >> (hi - 1)) << (hi + 1));

    const unsigned long long abit = 1ULL << qa;   // operand-0 bit
    const unsigned long long bbit = 1ULL << qb;    // operand-1 bit
    const unsigned long long idx[4] = {
        base, base | abit, base | bbit, base | abit | bbit
    };

    double ar[4], ai[4];
    for (int k = 0; k < 4; ++k) { ar[k] = re[idx[k]]; ai[k] = im[idx[k]]; }
    for (int r = 0; r < 4; ++r) {
        double nr = 0.0, ni = 0.0;
        for (int c = 0; c < 4; ++c) {
            cmul_add(M.m[8 * r + 2 * c], M.m[8 * r + 2 * c + 1],
                     ar[c], ai[c], nr, ni);
        }
        re[idx[r]] = nr; im[idx[r]] = ni;
    }
}

int qplayer_cuda_apply_dense2q_dev(double* d_re, double* d_im,
                                   unsigned long long dim,
                                   unsigned int qa, unsigned int qb,
                                   const double* m32,
                                   unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const unsigned long long quads = dim >> 2;
    const unsigned int blocks =
        static_cast<unsigned int>((quads + kBlockSize - 1) / kBlockSize);
    DenseMat4 M;
    for (int i = 0; i < 32; ++i) M.m[i] = m32[i];
    apply_dense_2q_gate_kernel<<<blocks, kBlockSize, 0, s>>>(
        d_re, d_im, dim, qa, qb, M);
    return cudaGetLastError();
}

// Launch the fused kernel for a runtime width k (1..kMaxGpuFusionWidth).
// d_qubits holds k ascending qubit indices, d_mat holds (2^k)^2 interleaved
// (re,im) pairs; both are device buffers owned by the caller.
int qplayer_cuda_apply_fused_dev(double* d_re, double* d_im,
                                 unsigned long long dim,
                                 unsigned int k,
                                 const unsigned int* d_qubits,
                                 const double* d_mat,
                                 unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const unsigned long long nblocks = dim >> k;
    const unsigned int grid =
        static_cast<unsigned int>((nblocks + kBlockSize - 1) / kBlockSize);

    switch (k) {
        case 1: apply_fused_kq_kernel<1><<<grid, kBlockSize, 0, s>>>(
                    d_re, d_im, nblocks, d_qubits, d_mat); break;
        case 2: apply_fused_kq_kernel<2><<<grid, kBlockSize, 0, s>>>(
                    d_re, d_im, nblocks, d_qubits, d_mat); break;
        case 3: apply_fused_kq_kernel<3><<<grid, kBlockSize, 0, s>>>(
                    d_re, d_im, nblocks, d_qubits, d_mat); break;
        case 4: apply_fused_kq_kernel<4><<<grid, kBlockSize, 0, s>>>(
                    d_re, d_im, nblocks, d_qubits, d_mat); break;
        case 5: apply_fused_kq_kernel<5><<<grid, kBlockSize, 0, s>>>(
                    d_re, d_im, nblocks, d_qubits, d_mat); break;
        default:
            // The backend clamps to kMaxGpuFusionWidth before getting here, so a
            // width outside 1..5 is a programming error, not user input.
            return cudaErrorInvalidValue;
    }
    return cudaGetLastError();
}

// Scratch buffers backing the fused kernel's qubit list and matrix. Allocated
// once per width growth and reused across windows: a circuit can
// hold dozens of windows and a malloc/free pair each would cost more than the
// launches fusion saves. mat_doubles counts interleaved (re,im) doubles.
int qplayer_cuda_alloc_fused_scratch(unsigned int** d_qubits, double** d_mat,
                                     unsigned long long mat_doubles,
                                     unsigned long long qubit_count) {
    *d_qubits = nullptr;
    *d_mat    = nullptr;
    const cudaError_t e_q = cudaMalloc(
        d_qubits, static_cast<std::size_t>(qubit_count ? qubit_count : 1)
                      * sizeof(unsigned int));
    const cudaError_t e_m =
        cudaMalloc(d_mat, static_cast<std::size_t>(mat_doubles) * sizeof(double));
    if (e_q != cudaSuccess || e_m != cudaSuccess) {
        cudaFree(*d_qubits); cudaFree(*d_mat);
        *d_qubits = nullptr; *d_mat = nullptr;
        return e_q != cudaSuccess ? e_q : e_m;
    }
    return cudaSuccess;
}

void qplayer_cuda_free_fused_scratch(unsigned int* d_qubits, double* d_mat) {
    cudaFree(d_qubits);
    cudaFree(d_mat);
}

// Upload one window's qubit list and matrix on the backend stream, keeping the
// single-stream ordering guarantee with the kernel that follows.
int qplayer_cuda_upload_fused(unsigned int* d_qubits, const unsigned int* h_qubits,
                              unsigned long long qubit_count,
                              double* d_mat, const double* h_mat,
                              unsigned long long mat_doubles,
                              unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    cudaError_t e = cudaMemcpyAsync(d_qubits, h_qubits,
                                    static_cast<std::size_t>(qubit_count) * sizeof(unsigned int),
                                    cudaMemcpyHostToDevice, s);
    if (e != cudaSuccess) return e;
    e = cudaMemcpyAsync(d_mat, h_mat,
                        static_cast<std::size_t>(mat_doubles) * sizeof(double),
                        cudaMemcpyHostToDevice, s);
    if (e != cudaSuccess) return e;
    // The host matrix buffer is a stack/heap temporary in the caller, so the
    // async copy must complete before it goes out of scope.
    return cudaStreamSynchronize(s);
}

// Self-contained CCX launcher (owns a transient device buffer) for unit tests,
// mirroring qplayer_cuda_apply_2q. Uses the default stream.
int qplayer_cuda_apply_ccx(double* h_re, double* h_im,
                           unsigned long long dim,
                           unsigned int c0, unsigned int c1,
                           unsigned int target) {
    double* d_re = nullptr;
    double* d_im = nullptr;
    const cudaError_t up = upload_state(h_re, h_im, dim, d_re, d_im);
    if (up != cudaSuccess) { cudaFree(d_re); cudaFree(d_im); return up; }

    const unsigned int blocks =
        static_cast<unsigned int>((dim + kBlockSize - 1) / kBlockSize);
    apply_ccx_kernel_dev<<<blocks, kBlockSize>>>(d_re, d_im, dim, c0, c1, target);

    return download_and_free(h_re, h_im, dim, d_re, d_im);
}

// Device->host download using the async stream, then sync before the copy so
// the host sees the fully-updated state (single-stream order guarantee).
int qplayer_cuda_download_state_stream(double* h_re, double* h_im,
                                       const double* d_re, const double* d_im,
                                       unsigned long long dim,
                                       unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
    cudaError_t e = cudaStreamSynchronize(s);
    if (e == cudaSuccess) e = cudaMemcpyAsync(h_re, d_re, bytes,
                                               cudaMemcpyDeviceToHost, s);
    if (e == cudaSuccess) e = cudaMemcpyAsync(h_im, d_im, bytes,
                                               cudaMemcpyDeviceToHost, s);
    if (e == cudaSuccess) e = cudaStreamSynchronize(s);
    return e;
}

// ── GPU Monte Carlo trajectory cuRAND state init ───────────
// Allocate `n_traj` curandStatePhilox4_32_10 states on the device and seed each
// from the host-derived per-trajectory seed (splitmix64(master ^ (j<<32)),
// computed by the .cpp caller). The opaque device pointer is returned via
// *states_out (cast to void* so the .cpp stays free of curand_kernel.h). On
// error the buffer is freed and *states_out is null. Returns cudaError_t.
int qplayer_cuda_init_curand_states(void** states_out,
                                    const unsigned long long* h_seeds,
                                    int n_traj) {
    *states_out = nullptr;
    if (n_traj <= 0) return cudaSuccess;

    curandStatePhilox4_32_10* d_states = nullptr;
    unsigned long long*       d_seeds  = nullptr;
    const std::size_t states_bytes =
        static_cast<std::size_t>(n_traj) * sizeof(curandStatePhilox4_32_10);
    const std::size_t seeds_bytes =
        static_cast<std::size_t>(n_traj) * sizeof(unsigned long long);

    cudaError_t e = cudaMalloc(&d_states, states_bytes);
    if (e == cudaSuccess) e = cudaMalloc(&d_seeds, seeds_bytes);
    if (e != cudaSuccess) {
        cudaFree(d_states);
        cudaFree(d_seeds);
        return e;
    }
    e = cudaMemcpy(d_seeds, h_seeds, seeds_bytes, cudaMemcpyHostToDevice);
    if (e == cudaSuccess) {
        const unsigned int blocks =
            static_cast<unsigned int>((n_traj + kBlockSize - 1) / kBlockSize);
        init_curand_states_kernel<<<blocks, kBlockSize>>>(d_states, d_seeds, n_traj);
        e = cudaGetLastError();
        if (e == cudaSuccess) e = cudaDeviceSynchronize();
    }
    cudaFree(d_seeds);  // seeds consumed; states retained for the trajectory loop
    if (e != cudaSuccess) {
        cudaFree(d_states);
        return e;
    }
    *states_out = static_cast<void*>(d_states);
    return cudaSuccess;
}

// Free the trajectory cuRAND state buffer. Null pointer is a safe no-op.
void qplayer_cuda_free_curand_states(void* states) {
    cudaFree(states);
}

// H2D upload launcher for the Kraus CPU-apply-then-upload trajectory path.
// Copies pinned host buffers h_re/h_im into already-allocated device buffers
// d_re/d_im via the per-backend stream (in-order with gate kernels).
// Returns cudaError_t (0 == success) for GPUError wrapping.
int qplayer_cuda_upload_state(const double* h_re, const double* h_im,
                              double* d_re, double* d_im,
                              unsigned long long dim,
                              unsigned long long stream_handle) {
    const cudaStream_t s = reinterpret_cast<cudaStream_t>(stream_handle);
    const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
    cudaError_t e = cudaMemcpyAsync(d_re, h_re, bytes, cudaMemcpyHostToDevice, s);
    if (e == cudaSuccess) {
        e = cudaMemcpyAsync(d_im, h_im, bytes, cudaMemcpyHostToDevice, s);
    }
    if (e == cudaSuccess) {
        e = cudaStreamSynchronize(s);
    }
    return e;
}

// ── GPU-resident sampler ───────────────────────────────────
// Draws `nshots` basis indices directly from the device-resident state vector
// without downloading it to the host. Pipeline (default stream):
//   prob[i]=|psi_i|^2  →  thrust::inclusive_scan → unnormalised CDF
//   total = CDF[dim-1] →  per-shot upper_bound(CDF, u*total) → basis index.
// h_uniforms (nshots) are generated by the caller's std::mt19937_64(seed) (CPU
// parity); only the nshots basis indices return to the host, so the
// 2GiB D2H + AoS materialization of the full state is avoided for counts-only.
// A transient device prob/CDF buffer (dim doubles) is allocated and freed here.
// Returns cudaError_t (0 == success) for GPUError wrapping.
int qplayer_cuda_sample_counts(const double* d_re, const double* d_im,
                               unsigned long long dim,
                               const double* h_uniforms,
                               unsigned long long nshots,
                               unsigned long long* h_out_basis) {
    double*             d_prob     = nullptr;
    double*             d_uniforms = nullptr;
    unsigned long long* d_basis    = nullptr;
    const std::size_t prob_bytes = static_cast<std::size_t>(dim) * sizeof(double);
    const std::size_t unif_bytes = static_cast<std::size_t>(nshots) * sizeof(double);
    const std::size_t bas_bytes  =
        static_cast<std::size_t>(nshots) * sizeof(unsigned long long);

    cudaError_t e = cudaMalloc(&d_prob, prob_bytes);
    if (e == cudaSuccess) e = cudaMalloc(&d_uniforms, unif_bytes);
    if (e == cudaSuccess) e = cudaMalloc(&d_basis, bas_bytes);
    if (e != cudaSuccess) {
        cudaFree(d_prob); cudaFree(d_uniforms); cudaFree(d_basis);
        return e;
    }

    // prob[i] = |psi_i|^2.
    const unsigned int prob_blocks =
        static_cast<unsigned int>((dim + kBlockSize - 1) / kBlockSize);
    prob_kernel<<<prob_blocks, kBlockSize>>>(d_re, d_im, d_prob, dim);
    e = cudaGetLastError();

    // In-place inclusive scan → unnormalised CDF on the device (Thrust).
    if (e == cudaSuccess) {
        thrust::device_ptr<double> p(d_prob);
        thrust::inclusive_scan(thrust::device, p, p + dim, p);
        e = cudaGetLastError();
    }

    // total = CDF[dim-1] (unnormalised probability mass).
    double total = 0.0;
    if (e == cudaSuccess) {
        e = cudaMemcpy(&total, d_prob + (dim - 1), sizeof(double),
                       cudaMemcpyDeviceToHost);
    }

    // Upload host uniforms, draw on device, download the basis indices.
    if (e == cudaSuccess) {
        e = cudaMemcpy(d_uniforms, h_uniforms, unif_bytes, cudaMemcpyHostToDevice);
    }
    if (e == cudaSuccess) {
        const unsigned int draw_blocks =
            static_cast<unsigned int>((nshots + kBlockSize - 1) / kBlockSize);
        draw_kernel<<<draw_blocks, kBlockSize>>>(d_prob, dim, d_uniforms, nshots,
                                                 total, d_basis);
        e = cudaGetLastError();
    }
    if (e == cudaSuccess) e = cudaDeviceSynchronize();
    if (e == cudaSuccess) {
        e = cudaMemcpy(h_out_basis, d_basis, bas_bytes, cudaMemcpyDeviceToHost);
    }

    cudaFree(d_prob);
    cudaFree(d_uniforms);
    cudaFree(d_basis);
    return e;
}

}  // extern "C"

}  // namespace qplayer::cuda
