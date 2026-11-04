// cpu_dispatcher.hpp — CPUID runtime SIMD tier detection.
// Detects AVX-512F support at runtime so a single binary serves both AVX2-only
// and AVX-512 capable hosts without compile-time branching (performance-rules
// coding-rules).

#pragma once

#include <string_view>

namespace qplayer::detail {

// Two-tier model: AVX2 is the x86-64 baseline.
// Scalar fallback is intentionally absent — the project targets AVX2-capable
// hosts only.
enum class SimdTier {
    AVX2,   // 256-bit AVX2 + FMA3 path
    AVX512, // 512-bit AVX-512F path
};

// Returns true when the host CPU advertises AVX2 support.
// Used by detect_simd_tier() to assert the baseline; AVX2 is always expected.
bool cpu_supports_avx2();

// Returns true when the host CPU advertises AVX-512F support.
// When true, detect_simd_tier() selects SimdTier::AVX512.
bool cpu_supports_avx512();

// Returns the optimal SimdTier for the current host CPU.
// CPUID is executed exactly once; the result is cached for the process lifetime
// using a function-local static (C++11 thread-safe initialisation).
// Callers should prefer select_for_host() over branching on the returned value.
SimdTier detect_simd_tier();

// Returns a short human-readable label for logging and diagnostics.
// AVX512 -> "AVX-512F", AVX2 -> "AVX2".
const char* simd_tier_name(SimdTier tier);

// Returns avx512_fn when the host supports AVX-512F, avx2_fn otherwise.
// Intended use by : bind the appropriate kernel pointer once at startup,
// e.g. apply_gate_fn = select_for_host(&kernel_avx2, &kernel_avx512).
// Template keeps the helper header-only so can instantiate it with any
// matching function-pointer type without additional link dependencies.
template <typename Fn>
Fn select_for_host(Fn avx2_fn, Fn avx512_fn) {
    return detect_simd_tier() == SimdTier::AVX512 ? avx512_fn : avx2_fn;
}

}  // namespace qplayer::detail
