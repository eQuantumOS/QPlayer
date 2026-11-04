/// @file qubit_reorder.cu
/// @brief GPU coalesced-access qubit reorder.
///
/// Compiled only when QPLAYER_BUILD_GPU=ON. Permutes the SoA state vector so a
/// target qubit lands in a low-stride bit position before gate application,
/// guaranteeing coalesced device memory access. The external qubit-index scheme
/// is unchanged (the permutation is tracked logically) — transparency.
///
/// Convention matches the CPU reference qplayer::detail::reorder_qubits
/// (qubit_reorder.cpp): new_index bit k = old_index bit perm[k]. The scatter is
/// out-of-place (reorder scratch) so the bijection writes each output
/// slot exactly once with no race.

#include <cuda_runtime.h>

#include <cstddef>

namespace qplayer::cuda {

// Scatter kernel: out[permute(i)] = in[i], where permute(i) bit k = i bit perm[k].
// One thread per source amplitude; the bijection guarantees a unique destination.
__global__ void qubit_reorder_kernel(const double* in_re, const double* in_im,
                                     double* out_re, double* out_im,
                                     unsigned long long dim,
                                     const unsigned int* perm,
                                     unsigned int n_qubits) {
    const unsigned long long i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= dim) return;

    unsigned long long j = 0ULL;
    for (unsigned int k = 0; k < n_qubits; ++k) {
        const unsigned long long bit = (i >> perm[k]) & 1ULL;
        j |= (bit << k);
    }
    out_re[j] = in_re[i];
    out_im[j] = in_im[i];
}

namespace {
constexpr int kBlockSize = 256;
}  // namespace

extern "C" {

// Self-contained host reorder launcher. Permutes a host SoA state vector
// out-of-place on the device. `perm` must be a valid permutation of
// [0, n_qubits) — the caller validates it (the CPU detail::reorder_qubits is the
// reference + validator). Returns a cudaError_t (0 == success); nullptr frees
// are safe no-ops on the error path. The reorder scratch is the second device
// buffer (Execute 2× working set).
int qplayer_cuda_reorder_host(double* h_re, double* h_im,
                              unsigned long long dim,
                              unsigned int n_qubits,
                              const unsigned int* perm) {
    const std::size_t bytes  = static_cast<std::size_t>(dim) * sizeof(double);
    const std::size_t pbytes = static_cast<std::size_t>(n_qubits) * sizeof(unsigned int);

    double*       d_in_re  = nullptr;
    double*       d_in_im  = nullptr;
    double*       d_out_re = nullptr;
    double*       d_out_im = nullptr;
    unsigned int* d_perm   = nullptr;

    auto cleanup = [&]() {
        cudaFree(d_in_re);  cudaFree(d_in_im);
        cudaFree(d_out_re); cudaFree(d_out_im);
        cudaFree(d_perm);
    };

    cudaError_t e = cudaMalloc(&d_in_re, bytes);
    if (e == cudaSuccess) e = cudaMalloc(&d_in_im, bytes);
    if (e == cudaSuccess) e = cudaMalloc(&d_out_re, bytes);
    if (e == cudaSuccess) e = cudaMalloc(&d_out_im, bytes);
    if (e == cudaSuccess) e = cudaMalloc(&d_perm, pbytes);
    if (e != cudaSuccess) { cleanup(); return e; }

    e = cudaMemcpy(d_in_re, h_re, bytes, cudaMemcpyHostToDevice);
    if (e == cudaSuccess) e = cudaMemcpy(d_in_im, h_im, bytes, cudaMemcpyHostToDevice);
    if (e == cudaSuccess) e = cudaMemcpy(d_perm, perm, pbytes, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) { cleanup(); return e; }

    const unsigned int blocks =
        static_cast<unsigned int>((dim + kBlockSize - 1) / kBlockSize);
    qubit_reorder_kernel<<<blocks, kBlockSize>>>(
        d_in_re, d_in_im, d_out_re, d_out_im, dim, d_perm, n_qubits);

    e = cudaGetLastError();
    if (e == cudaSuccess) e = cudaDeviceSynchronize();
    if (e == cudaSuccess) e = cudaMemcpy(h_re, d_out_re, bytes, cudaMemcpyDeviceToHost);
    if (e == cudaSuccess) e = cudaMemcpy(h_im, d_out_im, bytes, cudaMemcpyDeviceToHost);

    cleanup();
    return e;
}

}  // extern "C"

}  // namespace qplayer::cuda
