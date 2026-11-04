/// @file rng.hpp
/// @brief SplitMix64 seed-derivation helpers.
/// @ingroup core
///
/// A master seed (QRegister::set_seed / QPLAYER_SEED) is mixed into independent,
/// deterministic per-thread seeds for the OpenMP parallel sampling and Monte
/// Carlo trajectory loops. SplitMix64's finalizer decorrelates adjacent seeds —
/// a plain `master + tid` would leave mt19937_64 initial states correlated.

#pragma once

#include <cstdint>

namespace qplayer::detail {

// SplitMix64 finalizer (Vigna 2015). Bijective avalanche mix of a 64-bit value;
// used here purely as a seed decorrelator, not as the working PRNG
// (sampling keeps mt19937_64). Pure function — same input, same output.
inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Derives a thread-local seed from the master seed and OpenMP thread id.
// thread_id is shifted into the high lanes before mixing so threads 0..N never
// alias the raw master seed (: splitmix64(master ^ (tid << 32))).
inline std::uint64_t thread_seed(std::uint64_t master_seed,
                                 std::uint32_t thread_id) noexcept {
    return splitmix64(master_seed ^ (static_cast<std::uint64_t>(thread_id) << 32));
}

}  // namespace qplayer::detail
