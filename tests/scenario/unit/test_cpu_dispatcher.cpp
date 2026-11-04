// test_cpu_dispatcher.cpp — Unit tests for the CPUID runtime SIMD dispatcher
// Tests verify self-consistency on the build host without
// asserting a specific tier — the host may or may not support AVX-512F.
// (self-hosted framework), (Arrange/Act/Assert).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include "../../../include/backend/full_state/cpu_dispatcher.hpp"

using qplayer::detail::SimdTier;
using qplayer::detail::cpu_supports_avx2;
using qplayer::detail::cpu_supports_avx512;
using qplayer::detail::detect_simd_tier;
using qplayer::detail::select_for_host;
using qplayer::detail::simd_tier_name;

// 1. detect_simd_tier() returns one of the two valid tier values.
TEST(CpuDispatcher, DetectTier_ReturnsValidTier) {
    // Arrange / Act
    SimdTier tier = detect_simd_tier();

    // Assert — either AVX2 or AVX512; no third option exists.
    EXPECT_TRUE(tier == SimdTier::AVX2 || tier == SimdTier::AVX512);
}

// 2. Tier is consistent with cpu_supports_avx512(): AVX-512 capable host
//    must yield AVX512, everything else must yield AVX2.
TEST(CpuDispatcher, DetectTier_ConsistentWithAvx512Support) {
    // Arrange
    bool has_avx512 = cpu_supports_avx512();

    // Act
    SimdTier tier = detect_simd_tier();

    // Assert — the two APIs must agree.
    if (has_avx512) {
        EXPECT_TRUE(tier == SimdTier::AVX512);
    } else {
        EXPECT_TRUE(tier == SimdTier::AVX2);
    }
}

// 3. detect_simd_tier() returns the same value on every call (cached result).
TEST(CpuDispatcher, DetectTier_Cached_StableAcrossCalls) {
    // Arrange / Act
    SimdTier first  = detect_simd_tier();
    SimdTier second = detect_simd_tier();
    SimdTier third  = detect_simd_tier();

    // Assert — all three calls must agree (function-local static).
    EXPECT_TRUE(first == second);
    EXPECT_TRUE(second == third);
}

// 4. select_for_host() picks the pointer that matches the host tier.
//    Sentinel integers stand in for any pointer type — the template is
// instantiated with int* here; will use kernel function pointers.
TEST(CpuDispatcher, SelectForHost_PicksByTier) {
    // Arrange
    int a = 2;    // sentinel for AVX2 path
    int b = 512;  // sentinel for AVX-512 path
    bool has_avx512 = cpu_supports_avx512();

    // Act
    int* chosen = select_for_host(&a, &b);

    // Assert — choice must match host capability.
    if (has_avx512) {
        EXPECT_TRUE(chosen == &b);
    } else {
        EXPECT_TRUE(chosen == &a);
    }
}

// 5. simd_tier_name() returns non-empty strings that differ between tiers.
TEST(CpuDispatcher, SimdTierName_NonEmpty) {
    // Arrange / Act
    const char* avx2_name   = simd_tier_name(SimdTier::AVX2);
    const char* avx512_name = simd_tier_name(SimdTier::AVX512);

    // Assert — both non-null, non-empty, and distinct (diagnostic utility).
    EXPECT_TRUE(avx2_name   != nullptr);
    EXPECT_TRUE(avx512_name != nullptr);
    EXPECT_TRUE(avx2_name[0]   != '\0');
    EXPECT_TRUE(avx512_name[0] != '\0');
    // Tier names must not be identical so log output is unambiguous.
    EXPECT_NE(std::string_view{avx2_name}, std::string_view{avx512_name});
}

// 6. AVX-512F CPUs are always AVX2-capable (AVX-512 is a superset of AVX2).
//    If avx512 is reported true, avx2 must also be true.
TEST(CpuDispatcher, Avx512ImpliesAvx2) {
    // Arrange / Act
    bool has_avx2   = cpu_supports_avx2();
    bool has_avx512 = cpu_supports_avx512();

    // Assert — architectural guarantee: AVX-512F CPUs include AVX2 (Intel SDM).
    if (has_avx512) {
        EXPECT_TRUE(has_avx2);
    }
    // When avx512 is false, avx2 may still be true or false — no assertion.
}
