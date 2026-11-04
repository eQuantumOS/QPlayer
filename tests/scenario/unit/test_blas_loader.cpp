/// @file test_blas_loader.cpp
/// @brief Lazy OpenBLAS acquisition contract.
///
/// The regression these guard against was invisible to every existing test: a
/// linked OpenBLAS builds one thread per core at library-initialisation time,
/// and those idle threads contended with QPlayer's own OpenMP teams until small
/// per-shot FullState circuits ran ~100x slower. Nothing about the numerics
/// changed, so only the load mechanics can be asserted.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include "../../../include/backend/tensor_network/blas_loader.hpp"
#include "../../../include/qregister.hpp"

#include <cstdlib>
#include <string>

using namespace qplayer;

// The library is dlopen'd rather than linked, so a failure to resolve would
// surface here as an exception instead of at process start.
TEST(BlasLoader, EnsureLoaded_ResolvesZgesvdEntry) {
    EXPECT_NO_THROW(qplayer::detail::ensure_blas_loaded());
    EXPECT_TRUE(qplayer::detail::zgesvd_entry() != nullptr);
}

// The pool cap must be in place BEFORE OpenBLAS initialises -- that is the only
// moment it can be, since openblas_set_num_threads() cannot shrink a pool that
// already exists. Asserting the environment is what makes the ordering testable:
// counting threads would be read as this process's whole OpenMP footprint.
TEST(BlasLoader, EnsureLoaded_CapsPoolBeforeInit) {
    qplayer::detail::ensure_blas_loaded();

    const char* raw = std::getenv("OPENBLAS_NUM_THREADS");
    EXPECT_TRUE(raw != nullptr);
    // A caller-supplied value wins (setenv overwrite=0), so only the default is
    // pinned to "1"; any explicit setting is honoured as-is.
    if (raw != nullptr && std::string{raw} != "1") {
        EXPECT_TRUE(std::string{raw}.size() > 0);
    }
}

// The guard reads its three OpenBLAS controls through the same handle.
// As weak symbols they bound to null once the link was dropped, which silently
// disarmed the calling-thread bound and crashed inside OpenBLAS -- so a build
// that exports them must still see them here.
TEST(BlasLoader, ThreadingControls_ResolveWhenExported) {
    qplayer::detail::ensure_blas_loaded();

    const auto get_config = qplayer::detail::blas_get_config_fn();
    if (get_config == nullptr) return;  // SKIP: OpenBLAS built without threading

    EXPECT_TRUE(qplayer::detail::blas_set_num_threads_fn() != nullptr);
    EXPECT_TRUE(qplayer::detail::blas_get_num_threads_fn() != nullptr);
    const char* cfg = get_config();
    EXPECT_TRUE(cfg != nullptr);
}

// A TensorNetwork run must still produce correct amplitudes through the
// dlopen'd zgesvd_ -- the loader is transparent or it is broken.
TEST(BlasLoader, TensorNetworkRun_StillCorrectThroughLoadedZgesvd) {
    QRegister qr(SimType::TensorNetwork);
    qr.set_seed(1234);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    auto counts = qr.run(circ, 4096).counts();

    // Bell state: only the correlated outcomes appear (exact, no truncation at
    // chi=2), and they split evenly within sampling tolerance.
    EXPECT_EQ(counts.count("01"), 0u);
    EXPECT_EQ(counts.count("10"), 0u);
    EXPECT_NEAR(static_cast<double>(counts["00"]) / 4096.0, 0.5, 0.03);
    EXPECT_NEAR(static_cast<double>(counts["11"]) / 4096.0, 0.5, 0.03);
}
