// test_seed_reproducibility.cpp — Phase 8 seed reproducibility
// A fixed master seed must make run(circuit, nshots)
// bit-identical across repeated runs on BOTH the noiseless measurement-sampling
// path and the Monte Carlo noise-trajectory path. Also checks current_seed()
// round-trips the set value and that distinct seeds generally diverge.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <map>
#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/qregister.hpp"
#include "../../../include/result.hpp"
#include "../../../include/types.hpp"

using qplayer::Circuit;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::Result;
using qplayer::SimType;

namespace {

using Counts = std::map<std::string, std::uint64_t>;

bool counts_equal(const Counts& a, const Counts& b) {
    if (a.size() != b.size()) return false;
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end() || it->second != kv.second) return false;
    }
    return true;
}

Counts bell_counts(std::uint64_t seed, std::uint32_t nshots, const NoiseModel* nm) {
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(seed);
    if (nm) qr.set_noise_model(*nm);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    return qr.run(circ, nshots, /*enable_fusion=*/false).counts();
}

}  // namespace

// ── Noiseless measurement sampling: same seed -> identical counts ─────────────

TEST(SeedReproducibility, NoiselessSampling_SameSeed_Identical) {
    const std::uint32_t nshots = 4000;
    const std::uint64_t seed   = 0xA5A5A5A5ULL;

    const Counts c1 = bell_counts(seed, nshots, nullptr);
    const Counts c2 = bell_counts(seed, nshots, nullptr);

    // Exact equality — lazy sampling draws from the resolved seed (0305).
    EXPECT_TRUE(counts_equal(c1, c2));
}

// ── Monte Carlo noise path: same seed -> identical counts ────────────────────

TEST(SeedReproducibility, NoisePath_SameSeed_Identical) {
    const std::uint32_t nshots = 4000;
    const std::uint64_t seed   = 0x1234ABCDULL;

    NoiseModel nm;
    nm.add_depolarizing("h", 0.05).add_depolarizing("cx", 0.05);

    const Counts c1 = bell_counts(seed, nshots, &nm);
    const Counts c2 = bell_counts(seed, nshots, &nm);

    // Exact equality — per-trajectory RNG seeded by thread_seed(seed, traj).
    EXPECT_TRUE(counts_equal(c1, c2));
}

// ── current_seed() round-trips the value set via set_seed() ──────────────────

TEST(SeedReproducibility, CurrentSeed_ReflectsSetSeed) {
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(987654321ULL);
    EXPECT_EQ(qr.current_seed(), static_cast<std::uint64_t>(987654321ULL));
}

// ── Distinct seeds generally produce different noise trajectories ─────────────

TEST(SeedReproducibility, DifferentSeeds_NoisePath_Diverge) {
    const std::uint32_t nshots = 4000;

    NoiseModel nm;
    nm.add_depolarizing("h", 0.1).add_depolarizing("cx", 0.1);

    const Counts c1 = bell_counts(111ULL, nshots, &nm);
    const Counts c2 = bell_counts(222ULL, nshots, &nm);

    // Not a strict guarantee, but with 4000 noisy shots two independent seeds
    // virtually never yield byte-identical histograms.
    EXPECT_FALSE(counts_equal(c1, c2));
}
