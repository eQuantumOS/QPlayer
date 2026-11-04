/// @file blas_loader.cpp
/// @brief dlopen-based OpenBLAS acquisition.

#include "backend/tensor_network/blas_loader.hpp"

#include "error.hpp"

#include <dlfcn.h>

#include <cstdlib>

#include <mutex>
#include <string>

namespace qplayer::detail {

namespace {

// The soname, not the linker name: libopenblas.so belongs to the -dev package
// and is absent on a runtime-only host. dlopen follows the ld.so search path,
// so a build-time path is neither needed nor honoured.
constexpr const char* kOpenBlasSoname = "libopenblas.so.0";

struct BlasHandle {
    void*     lib    = nullptr;
    ZgesvdFn  zgesvd = nullptr;
    ZgemmFn   zgemm  = nullptr;
    // Optional -- absent on a non-threaded OpenBLAS build.
    BlasSetNumThreadsFn set_threads = nullptr;
    BlasGetNumThreadsFn get_threads = nullptr;
    BlasGetConfigFn     get_config  = nullptr;
};

// Resolved once. RTLD_LOCAL keeps the symbols out of the global namespace so a
// host application's own BLAS is unaffected by ours.
BlasHandle& resolve() {
    static BlasHandle handle;
    static std::once_flag once;
    static std::string   failure;

    std::call_once(once, [] {
        // Cap the pool BEFORE the library initialises, which is the only moment
        // that can (revised). OpenBLAS reads this on init and a later
        // openblas_set_num_threads(1) does not shrink an existing pool, so
        // without this a process that touches TensorNetwork keeps a
        // one-thread-per-core pool for its lifetime -- and the FullState work
        // that follows in the SAME process pays the contention the lazy load was
        // meant to remove (measured: FullState 1.05 ms alone, 122~146 ms after a
        // TN run in-process). Single-threaded BLAS is not a sacrifice for TN
        // either: the SVDs are 2*chi x 2*chi, below the BLAS threading
        // break-even, and it measured FASTER (clean q26_ising 17.0 -> 1.4 ms,
        // noisy trajectory 3.8 -> 3.3 ms).
        //
        // overwrite=0: an explicit OPENBLAS_NUM_THREADS from the user wins.
        setenv("OPENBLAS_NUM_THREADS", "1", 0);

        handle.lib = dlopen(kOpenBlasSoname, RTLD_NOW | RTLD_LOCAL);
        if (handle.lib == nullptr) {
            const char* err = dlerror();
            failure = std::string("cannot load ") + kOpenBlasSoname + " (" +
                      (err != nullptr ? err : "no dlerror") + ")";
            return;
        }
        handle.zgesvd =
            reinterpret_cast<ZgesvdFn>(dlsym(handle.lib, "zgesvd_"));
        if (handle.zgesvd == nullptr) {
            const char* err = dlerror();
            failure = std::string("loaded ") + kOpenBlasSoname +
                      " but symbol zgesvd_ is missing (" +
                      (err != nullptr ? err : "no dlerror") + ")";
            return;
        }
        handle.zgemm = reinterpret_cast<ZgemmFn>(dlsym(handle.lib, "zgemm_"));
        if (handle.zgemm == nullptr) {
            const char* err = dlerror();
            failure = std::string("loaded ") + kOpenBlasSoname +
                      " but symbol zgemm_ is missing (" +
                      (err != nullptr ? err : "no dlerror") + ")";
            return;
        }
        handle.set_threads = reinterpret_cast<BlasSetNumThreadsFn>(
            dlsym(handle.lib, "openblas_set_num_threads"));
        handle.get_threads = reinterpret_cast<BlasGetNumThreadsFn>(
            dlsym(handle.lib, "openblas_get_num_threads"));
        handle.get_config = reinterpret_cast<BlasGetConfigFn>(
            dlsym(handle.lib, "openblas_get_config"));
    });

    if (handle.zgesvd == nullptr) {
        throw SimulatorError{"TensorNetwork",
            "MPS/MPO truncation needs LAPACK zgesvd_ from OpenBLAS: " + failure};
    }
    return handle;
}

}  // namespace

ZgesvdFn zgesvd_entry() {
    return resolve().zgesvd;
}

ZgemmFn zgemm_entry() {
    return resolve().zgemm;
}

void ensure_blas_loaded() {
    static_cast<void>(resolve());
}

BlasSetNumThreadsFn blas_set_num_threads_fn() { return resolve().set_threads; }
BlasGetNumThreadsFn blas_get_num_threads_fn() { return resolve().get_threads; }
BlasGetConfigFn     blas_get_config_fn()      { return resolve().get_config; }

void zgesvd(const char* jobu, const char* jobvt, const int* m, const int* n,
            std::complex<double>* a, const int* lda, double* s,
            std::complex<double>* u, const int* ldu,
            std::complex<double>* vt, const int* ldvt,
            std::complex<double>* work, const int* lwork,
            double* rwork, int* info) {
    resolve().zgesvd(jobu, jobvt, m, n, a, lda, s, u, ldu, vt, ldvt,
                     work, lwork, rwork, info);
}

void zgemm(const char* transa, const char* transb, const int* m, const int* n, const int* k,
           const std::complex<double>* alpha, const std::complex<double>* a, const int* lda,
           const std::complex<double>* b, const int* ldb,
           const std::complex<double>* beta, std::complex<double>* c, const int* ldc) {
    resolve().zgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}

}  // namespace qplayer::detail
