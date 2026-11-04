/// @file blas_loader.hpp
/// @brief Lazy OpenBLAS acquisition for the TensorNetwork SVD.
///
/// OpenBLAS builds its thread pool when the library is INITIALISED, not when a
/// BLAS routine is first called. Linking it therefore charged every process --
/// including FullState-only ones that never call BLAS -- a pool of one thread
/// per core, which contended with QPlayer's own OpenMP teams and made small
/// per-shot circuits up to 100x slower. Resolving zgesvd_ through dlopen keeps
/// OpenBLAS out of processes that do not use TensorNetwork.

#ifndef QPLAYER_BACKEND_TENSOR_NETWORK_BLAS_LOADER_HPP
#define QPLAYER_BACKEND_TENSOR_NETWORK_BLAS_LOADER_HPP

#include <complex>

namespace qplayer::detail {

/// LAPACK complex SVD signature. Mirrors the Fortran ABI: no lapacke.h ships
/// with the OpenBLAS packages we target, and forbids adding a
/// dependency just for a prototype.
using ZgesvdFn = void (*)(const char*, const char*, const int*, const int*,
                          std::complex<double>*, const int*, double*,
                          std::complex<double>*, const int*,
                          std::complex<double>*, const int*,
                          std::complex<double>*, const int*, double*, int*);

/// BLAS complex GEMM signature, same Fortran ABI reasoning as ZgesvdFn.
using ZgemmFn = void (*)(const char*, const char*, const int*, const int*, const int*,
                         const std::complex<double>*, const std::complex<double>*, const int*,
                         const std::complex<double>*, const int*,
                         const std::complex<double>*, std::complex<double>*, const int*);

/// Resolve zgesvd_ from OpenBLAS, loading the library on first call.
/// Thread-safe and idempotent; the handle is kept for process lifetime.
/// @return the resolved entry point (never null -- failure throws).
/// @throws SimulatorError when OpenBLAS or the symbol cannot be found.
ZgesvdFn zgesvd_entry();

/// Load OpenBLAS eagerly so the first zgesvd_ call inside an OpenMP region does
/// not have to dlopen. Called from the TensorNetwork backend's
/// constructor. Same failure contract as zgesvd_entry().
/// @throws SimulatorError when OpenBLAS or the symbol cannot be found.
void ensure_blas_loaded();

/// Resolve zgemm_ from the same handle. The two-site merge is a plain
/// contraction over the shared bond, i.e. a GEMM -- see mps.cpp for why the
/// stored layouts let one call do it with no transpose buffer.
/// @return the resolved entry point (never null -- failure throws).
/// @throws SimulatorError when OpenBLAS or the symbol cannot be found.
ZgemmFn zgemm_entry();

/// OpenBLAS threading controls used by the guard. These are OPTIONAL:
/// an OpenBLAS built without threading does not export them, so a null return
/// means "nothing to control" rather than an error. Resolving them through the
/// same handle as zgesvd_ is what keeps the guard working now that the library
/// is dlopen'd instead of linked -- as weak symbols they would always be null
/// silently disarming the calling-thread bound relies on.
/// @return the entry point, or nullptr when this build does not export it.
using BlasSetNumThreadsFn = void (*)(int);
using BlasGetNumThreadsFn = int (*)(void);
using BlasGetConfigFn     = const char* (*)(void);
BlasSetNumThreadsFn blas_set_num_threads_fn();
BlasGetNumThreadsFn blas_get_num_threads_fn();
BlasGetConfigFn     blas_get_config_fn();

/// Call zgemm_ through the lazily resolved entry point. Argument order matches
/// the BLAS routine exactly, so call sites read as a normal GEMM.
void zgemm(const char* transa, const char* transb, const int* m, const int* n, const int* k,
           const std::complex<double>* alpha, const std::complex<double>* a, const int* lda,
           const std::complex<double>* b, const int* ldb,
           const std::complex<double>* beta, std::complex<double>* c, const int* ldc);

/// Call zgesvd_ through the lazily resolved entry point. Argument order matches
/// the LAPACK routine exactly so call sites read as they did when the symbol
/// was linked directly.
void zgesvd(const char* jobu, const char* jobvt, const int* m, const int* n,
            std::complex<double>* a, const int* lda, double* s,
            std::complex<double>* u, const int* ldu,
            std::complex<double>* vt, const int* ldvt,
            std::complex<double>* work, const int* lwork,
            double* rwork, int* info);

}  // namespace qplayer::detail

#endif  // QPLAYER_BACKEND_TENSOR_NETWORK_BLAS_LOADER_HPP
