// test_seed.cpp — Seed policy + SplitMix64 derivation.
// Covers QRegister::set_seed/current_seed, QPLAYER_SEED env override + lock,
// and the splitmix64 / thread_seed helpers. Determinism is
// asserted by property (same input -> same output, distinct inputs -> distinct
// outputs) rather than magic constants.,.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <cstdlib>

#include "../../../include/qregister.hpp"
#include "../../../include/detail/rng.hpp"

using qplayer::QRegister;
using qplayer::SimType;
using qplayer::detail::splitmix64;
using qplayer::detail::thread_seed;

namespace {

// Clears QPLAYER_SEED before/after each case so env-driven tests stay isolated
// from the host environment and from one another.
class SeedFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override    { ::unsetenv("QPLAYER_SEED"); }
    void TearDown() override  { ::unsetenv("QPLAYER_SEED"); }
};

}  // namespace

// ── SplitMix64 helper ─────────────────────────────────────────────────────────

// Same input must always produce the same output (pure function).
TEST(Seed, SplitMix64_SameInput_SameOutput) {
    EXPECT_TRUE(splitmix64(12345ULL) == splitmix64(12345ULL));
    EXPECT_TRUE(splitmix64(0ULL)     == splitmix64(0ULL));
}

// Distinct (and adjacent) inputs must diverge — the whole point of the mix.
TEST(Seed, SplitMix64_DistinctInputs_DistinctOutputs) {
    EXPECT_TRUE(splitmix64(0ULL) != splitmix64(1ULL));
    EXPECT_TRUE(splitmix64(100ULL) != splitmix64(101ULL));
    // splitmix64(0) must not pass zero through unchanged.
    EXPECT_TRUE(splitmix64(0ULL) != 0ULL);
}

// thread_seed must decorrelate adjacent thread ids under a fixed master seed.
TEST(Seed, ThreadSeed_DistinctThreadIds_DistinctSeeds) {
    const std::uint64_t master = 0xDEADBEEFCAFEF00DULL;
    EXPECT_TRUE(thread_seed(master, 0) != thread_seed(master, 1));
    EXPECT_TRUE(thread_seed(master, 1) != thread_seed(master, 2));
    // Deterministic: same (master, tid) -> same seed.
    EXPECT_TRUE(thread_seed(master, 3) == thread_seed(master, 3));
}

// ── QRegister seed API ────────────────────────────────────────────────────────

// set_seed() stores the value; current_seed() returns it (no env active).
TEST_F(SeedFixture, SetSeed_NoEnv_CurrentSeedReflectsValue) {
    // Arrange
    QRegister qr(SimType::FullState);

    // Act
    qr.set_seed(42);

    // Assert
    EXPECT_EQ(qr.current_seed(), std::uint64_t{42});
}

// QPLAYER_SEED present at construction wins; current_seed() returns the env
// value (priority table).
TEST_F(SeedFixture, EnvSeed_SetAtConstruction_OverridesDefault) {
    // Arrange
    ::setenv("QPLAYER_SEED", "777", 1);

    // Act
    QRegister qr(SimType::FullState);

    // Assert
    EXPECT_EQ(qr.current_seed(), std::uint64_t{777});
}

// When the env locks the seed, set_seed() is ignored (warns once internally).
TEST_F(SeedFixture, EnvSeed_Locked_SetSeedIgnored) {
    // Arrange
    ::setenv("QPLAYER_SEED", "777", 1);
    QRegister qr(SimType::FullState);

    // Act: attempt to override the locked seed.
    qr.set_seed(999);

    // Assert: env value still in effect.
    EXPECT_EQ(qr.current_seed(), std::uint64_t{777});
}

// A malformed QPLAYER_SEED is ignored (warn) and set_seed() works normally.
TEST_F(SeedFixture, EnvSeed_Invalid_FallsBackAndSetSeedWorks) {
    // Arrange
    ::setenv("QPLAYER_SEED", "not-a-number", 1);
    QRegister qr(SimType::FullState);

    // Act: env did not lock, so set_seed() takes effect.
    qr.set_seed(55);

    // Assert
    EXPECT_EQ(qr.current_seed(), std::uint64_t{55});
}
