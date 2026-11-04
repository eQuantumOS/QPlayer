/// @file tn_svd_gpu.hpp
/// @brief cuSolver SVD offload for the MPS two-site update.
/// @ingroup backend
///
/// Only the SVD leaves the host. merge / gate / split / renormalise stay on the
/// CPU because the measured transfer is 1% of the SVD at chi=256 -- moving the
/// whole chain to the device would buy that 1% while reopening four settled
/// contracts ( thread budget, resident sampling,
/// max_qubits, tensor lifetimes). Keeping the offload to one function keeps the
/// surface that can go wrong to one function.

#ifndef QPLAYER_BACKEND_TENSOR_NETWORK_TN_SVD_GPU_HPP
#define QPLAYER_BACKEND_TENSOR_NETWORK_TN_SVD_GPU_HPP

#include <complex>

namespace qplayer::detail {

/// Smallest min(rows, cols) at which the offload pays.
///
/// The measured break-even is chi ~ 48 and the SVD matrix is 2*chi on a side.
/// Below this the kernel launch and transfer dominate, which is the same trap
/// and already cost this project twice.
inline constexpr int kGpuSvdMinDim = 96;

/// Relative gap below which two singular values count as degenerate.
///
/// Compared against the BOUNDARY singular value, not the largest one: scaling by
/// s[0] makes neighbours in the tail of any smooth spectrum look degenerate, and
/// measurement showed that firing the fallback on every random circuit and
/// erasing the speedup.
inline constexpr double kGpuSvdDegenerateTol = 1e-12;

/// Concurrent SVDs admitted per device.
///
/// Trajectory threads beyond this wait for a slot instead of piling onto the
/// device. Measured on 2x H100 NVL, chi=256 surface code, 64 trajectories: with
/// 64 threads the wall time is flat over 1..4 slots (75-78 s), and with 8
/// threads 4 slots give the best point seen (58 s). Before the pool, the same
/// run took 333 s on one default stream.
inline constexpr int kGpuSvdSlotsPerDevice = 4;

/// True when this build has CUDA and the host has a usable device.
bool gpu_svd_available();

/// Number of offload slots still usable: kGpuSvdSlotsPerDevice per eligible
/// device, minus any retired by a hard failure. 0 on a CPU-only build or host.
int gpu_svd_slot_count();

/// Thin complex SVD (`jobu = jobvt = 'S'`) of a column-major `rows x cols`
/// matrix, computed with cusolverDnZgesvd.
///
/// `a` is READ-ONLY -- unlike LAPACK's zgesvd_ this does not destroy its input,
/// which is what lets the caller re-run on the CPU when the degeneracy check of
/// fires without having to keep a backup copy.
///
/// Handles `rows < cols` by factorising A^H on the device and swapping the
/// factors back on the host; cuSolver's gesvd requires m >= n.
///
/// @return false when the offload declines or fails -- no CUDA in this build, no
///         device, a dimension below kGpuSvdMinDim, or a cuSolver error. The
///         caller must then use the CPU path. NEVER throws: a GPU that cannot
///         help must not be able to fail a run that the CPU can complete.
bool gpu_zgesvd_thin(int rows, int cols, const std::complex<double>* a,
                     double* s, std::complex<double>* u, std::complex<double>* vt);

}  // namespace qplayer::detail

#endif  // QPLAYER_BACKEND_TENSOR_NETWORK_TN_SVD_GPU_HPP
