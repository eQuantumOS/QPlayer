/// @file tn_svd_gpu.cpp
/// @brief CPU-only stub for the cuSolver SVD offload.
///
/// The real implementation lives in tn_svd_gpu.cu and is compiled only when
/// QPLAYER_BUILD_GPU is on. This translation unit stays free of CUDA headers so
/// the default build never needs nvcc -- the same split full_state_gpu.{cpp,cu}
/// uses.

#include "backend/tensor_network/tn_svd_gpu.hpp"

#if !defined(QPLAYER_BUILD_GPU)

namespace qplayer::detail {

bool gpu_svd_available() { return false; }

int gpu_svd_slot_count() { return 0; }

bool gpu_zgesvd_thin(int, int, const std::complex<double>*, double*,
                     std::complex<double>*, std::complex<double>*) {
    return false;
}

}  // namespace qplayer::detail

#endif  // !QPLAYER_BUILD_GPU
