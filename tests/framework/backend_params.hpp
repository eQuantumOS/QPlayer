// backend_params.hpp — Backend-parameterised test macro TEST_BACKENDS.
// Provides testable_procs() and TEST_BACKENDS(suite, name) so a single test body
// is automatically registered once per testable ProcType at program startup.
// CPU is always included; GPU is added only when QPLAYER_BUILD_GPU is defined
// and qplayer_cuda_device_count() > 0 at runtime (same probe as factory.cpp).
//
// Core framework headers (test_case.hpp / test_runner.hpp) remain backend-
// agnostic; this header extends them with ProcType-awareness.
//
// Macro hygiene: generated symbols encode suite+name to avoid link-time
// collisions, following the pattern used by TEST() and TEST_F() in test_case.hpp.

#pragma once

#include <string>
#include <vector>

#include "test_case.hpp"   // QPLAYER_TEST_CAT, TestRunner::instance()
#include "../../include/backend/simulator_backend.hpp"  // ProcType

// Forward-declare the CUDA device-count probe at global scope inside the GPU
// build guard. Symbol is defined in full_state_gpu.cu; this pattern matches
// factory.cpp and test_full_state_gpu.cpp.
#if defined(QPLAYER_BUILD_GPU)
extern "C" int qplayer_cuda_device_count();
#endif

namespace qplayer::test {

// Returns a short suffix string used for ctest case labeling.
inline const char* proc_suffix(::qplayer::ProcType p) {
    return (p == ::qplayer::ProcType::GPU) ? "GPU" : "CPU";
}

// Returns the ordered set of ProcTypes to exercise in the current build and
// runtime environment. Always contains ProcType::CPU. Adds ProcType::GPU only
// when QPLAYER_BUILD_GPU is defined and at least one CUDA device is visible
// (graceful: GPU build on a GPU-less host -> CPU only, consistent with
//). Calling this from a static initialiser is pre-main safe (CUDA
// cudaGetDeviceCount is safe at any point;).
inline std::vector<::qplayer::ProcType> testable_procs() {
    std::vector<::qplayer::ProcType> procs;
    procs.push_back(::qplayer::ProcType::CPU);
#if defined(QPLAYER_BUILD_GPU)
    if (qplayer_cuda_device_count() > 0) {
        procs.push_back(::qplayer::ProcType::GPU);
    }
#endif
    return procs;
}

}  // namespace qplayer::test

// ── TEST_BACKENDS(suite, name) ────────────────────────────────────────────────
// Declares a parameterised test body that is registered once per testable
// ProcType at static-initialiser time. ctest sees:
//   suite.name_CPU   (always present)
//   suite.name_GPU   (GPU build with visible CUDA device only)
//
// The test body has ProcType proc in scope:
//
//   TEST_BACKENDS(BellState, Distribution_MatchesTheory) {
//       QRegister qr(SimType::FullState, proc);  // proc is in scope
//       ...
//   }
//
// Macro hygiene: all generated symbols include suite+name to avoid link-time
// collisions between suites that share a case name.

#define TEST_BACKENDS(suite, name)                                              \
    static void QPLAYER_TEST_CAT(qplayer_tb_fn_,                               \
        QPLAYER_TEST_CAT(suite, QPLAYER_TEST_CAT(_, name)))(                    \
            ::qplayer::ProcType proc);                                          \
    namespace {                                                                 \
    struct QPLAYER_TEST_CAT(qplayer_tb_reg_,                                   \
                            QPLAYER_TEST_CAT(suite,                             \
                                            QPLAYER_TEST_CAT(_, name))) {       \
        QPLAYER_TEST_CAT(qplayer_tb_reg_,                                      \
                         QPLAYER_TEST_CAT(suite,                                \
                                         QPLAYER_TEST_CAT(_, name)))() {        \
            for (::qplayer::ProcType _p :                                       \
                     ::qplayer::test::testable_procs()) {                       \
                std::string _case_name = std::string(#name) + "_" +            \
                                         ::qplayer::test::proc_suffix(_p);      \
                ::qplayer::test::TestRunner::instance().register_test(          \
                    #suite, _case_name,                                         \
                    [_p]() {                                                    \
                        QPLAYER_TEST_CAT(qplayer_tb_fn_,                        \
                            QPLAYER_TEST_CAT(suite,                             \
                                QPLAYER_TEST_CAT(_, name)))(_p);                \
                    },                                                           \
                    false);                                                      \
            }                                                                   \
        }                                                                       \
    };                                                                          \
    static QPLAYER_TEST_CAT(qplayer_tb_reg_,                                   \
                            QPLAYER_TEST_CAT(suite,                             \
                                            QPLAYER_TEST_CAT(_, name)))         \
        QPLAYER_TEST_CAT(qplayer_tb_reg_,                                      \
            QPLAYER_TEST_CAT(suite,                                             \
                QPLAYER_TEST_CAT(_, QPLAYER_TEST_CAT(name, _inst_))));          \
    }                                                                           \
    static void QPLAYER_TEST_CAT(qplayer_tb_fn_,                               \
        QPLAYER_TEST_CAT(suite, QPLAYER_TEST_CAT(_, name)))(                    \
            ::qplayer::ProcType proc)
