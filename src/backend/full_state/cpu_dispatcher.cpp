// cpu_dispatcher.cpp — CPUID runtime SIMD tier detection implementation
//
// GCC >= 11 / Clang >= 13 is guaranteed by, so __builtin_cpu_supports
// and __builtin_cpu_init are available on all supported compilers.
// No extra -mavx512f flag is needed here; __builtin_cpu_supports queries CPUID
// at runtime regardless of the compiler's own SIMD target.

#include "backend/full_state/cpu_dispatcher.hpp"

#include <string>

#include "log.hpp"

namespace qplayer::detail {

bool cpu_supports_avx2() {
    // __builtin_cpu_init() ensures the internal CPU feature cache is populated
    // before the first __builtin_cpu_supports() call (GCC manual).
    // Calling it more than once is safe (idempotent).
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
}

bool cpu_supports_avx512() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx512f") != 0;
}

SimdTier detect_simd_tier() {
    // Function-local static: C++11 guarantees initialisation happens exactly
    // once even under concurrent first calls (thread-safe static init).
    // After the first call the branch is never re-evaluated; CPUID overhead is
    // paid only at the first invocation.
    static const SimdTier tier = []() -> SimdTier {
        SimdTier t = cpu_supports_avx512() ? SimdTier::AVX512 : SimdTier::AVX2;
        // Log the selected tier once so it appears in diagnostic output.
        qplayer::log::info(std::string("SIMD dispatch tier: ") + simd_tier_name(t));
        return t;
    }();
    return tier;
}

const char* simd_tier_name(SimdTier tier) {
    switch (tier) {
        case SimdTier::AVX512: return "AVX-512F";
        case SimdTier::AVX2:   return "AVX2";
    }
    // Unreachable; silence -Wreturn-type.
    return "unknown";
}

}  // namespace qplayer::detail
