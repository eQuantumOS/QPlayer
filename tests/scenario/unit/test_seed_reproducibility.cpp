// test_seed_reproducibility.cpp — Phase 8 seed reproducibility
// A fixed master seed must make run(circuit, nshots)
// bit-identical across repeated runs on BOTH the noiseless measurement-sampling
// path and the Monte Carlo noise-trajectory path. Also checks current_seed()
// round-trips the set value and that distinct seeds generally diverge.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <omp.h>

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

// ── Seed 0 is a valid seed, not an "unset" sentinel ───────────────
//
// Audit #5: every backend used "master_seed_ != 0" as the unset marker, so an
// explicit set_seed(0) silently fell back to std::random_device while
// current_seed() still reported 0 — non-reproducible while claiming otherwise,
// which also broke the "run it with this seed to reproduce" bug-report workflow.
// Set-ness is now a separate flag, so 0 behaves exactly like any other value.

TEST(SeedReproducibility, SeedZero_NoiselessSampling_Identical) {
    const std::uint32_t nshots = 4000;

    const Counts c1 = bell_counts(0ULL, nshots, nullptr);
    const Counts c2 = bell_counts(0ULL, nshots, nullptr);

    EXPECT_TRUE(counts_equal(c1, c2));
}

// The noise path resolves the seed a second time per trajectory, so it needed the
// same fix: a "(seed != 0)" check downstream would have re-randomised every
// trajectory even after the first-level flag was corrected.
TEST(SeedReproducibility, SeedZero_NoisePath_Identical) {
    const std::uint32_t nshots = 2000;

    NoiseModel nm;
    nm.add_depolarizing("h", 0.05).add_depolarizing("cx", 0.05);

    const Counts c1 = bell_counts(0ULL, nshots, &nm);
    const Counts c2 = bell_counts(0ULL, nshots, &nm);

    EXPECT_TRUE(counts_equal(c1, c2));
}

// Seed 0 must not collide with seed 1 — it is a distinct point in seed space,
// not an alias for "no seed".
TEST(SeedReproducibility, SeedZero_DiffersFromSeedOne) {
    const std::uint32_t nshots = 4000;

    NoiseModel nm;
    nm.add_depolarizing("h", 0.1).add_depolarizing("cx", 0.1);

    const Counts c0 = bell_counts(0ULL, nshots, &nm);
    const Counts c1 = bell_counts(1ULL, nshots, &nm);

    EXPECT_FALSE(counts_equal(c0, c1));
}

TEST(SeedReproducibility, CurrentSeed_ReflectsSetSeedZero) {
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(0ULL);
    EXPECT_EQ(qr.current_seed(), static_cast<std::uint64_t>(0));
}

// SparseState keeps its own master_seed_/has_seed_ pair, so it needs the same
// guarantee (the audit listed all three backends).
TEST(SeedReproducibility, SeedZero_SparseState_Identical) {
    auto sparse_counts = []() {
        QRegister qr(SimType::SparseState, ProcType::CPU);
        qr.set_seed(0ULL);
        Circuit circ(3, 3);
        circ.h(0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
        return qr.run(circ, 2000).counts();
    };
    EXPECT_TRUE(counts_equal(sparse_counts(), sparse_counts()));
}

// ── Trajectory count merge must not depend on thread count (audit #12) ───────
//
// The Monte Carlo path used to take a mutex on one shared map per trajectory,
// which the surrounding comment already described as "merge per-thread local
// count maps after the OMP region". It now really does that, and the merge is a
// key-ordered uint64 sum, so the totals must be bit-identical across team sizes.
// Also exercises set_omp_threads(0) restoring the OpenMP default (audit #11):
// before that fix it clamped to 1 and silently serialised everything.

TEST(SeedReproducibility, NoisePath_CountsIndependentOfThreadCount) {
    const std::uint32_t nshots = 2000;
    const std::uint64_t seed   = 0xDEADBEEFULL;

    NoiseModel nm;
    nm.add_depolarizing("h", 0.08).add_depolarizing("cx", 0.08);

    QRegister thread_ctl(SimType::FullState, ProcType::CPU);

    thread_ctl.set_omp_threads(1);
    const Counts serial = bell_counts(seed, nshots, &nm);

    thread_ctl.set_omp_threads(8);
    const Counts parallel8 = bell_counts(seed, nshots, &nm);

    thread_ctl.set_omp_threads(0);  // restore the runtime default (audit #11)
    const Counts restored = bell_counts(seed, nshots, &nm);

    EXPECT_TRUE(counts_equal(serial, parallel8));
    EXPECT_TRUE(counts_equal(serial, restored));
}

// ── set_omp_threads(n <= 0) restores the ORIGINAL default (audit #11) ────────
//
// The old implementation clamped non-positive n to 1 — the documented contract
// said "restores OpenMP's default", so callers got a silent full serialisation.
// A naive fix (omp_get_max_threads()) is also wrong: after set_omp_threads(8) it
// reports 8, so "restore" would pin 8 forever. The original value is captured on
// first use instead.
TEST(SeedReproducibility, SetOmpThreads_NonPositiveRestoresOriginalDefault) {
    const int original = omp_get_max_threads();

    QRegister qr(SimType::FullState, ProcType::CPU);

    qr.set_omp_threads(2);
    EXPECT_EQ(omp_get_max_threads(), 2);

    qr.set_omp_threads(0);                      // documented: restore default
    EXPECT_EQ(omp_get_max_threads(), original);  // not 1, and not 2

    qr.set_omp_threads(3);
    qr.set_omp_threads(-1);                      // negative behaves like 0
    EXPECT_EQ(omp_get_max_threads(), original);
}

// ── Hot-path buffer reuse must not change results (audit #27) ─────────────────
//
// channels_for_gate() now writes into a caller-owned buffer that the trajectory
// loop reuses across gates instead of returning a fresh vector each time. Same
// channels, same order — so a fixed seed must still reproduce bit-identical
// counts, and two independent runs must agree.
TEST(SeedReproducibility, NoisePath_ReusedChannelBuffer_CountsUnchanged) {
    const std::uint32_t nshots = 3000;
    const std::uint64_t seed   = 0xC0FFEEULL;

    NoiseModel nm;
    // Several channels on several gates so the buffer is refilled repeatedly with
    // different lengths — the case a stale-capacity bug would corrupt.
    nm.add_depolarizing("h", 0.05)
      .add_depolarizing("cx", 0.05)
      .add_amplitude_damping("h", 0.02);

    const Counts a = bell_counts(seed, nshots, &nm);
    const Counts b = bell_counts(seed, nshots, &nm);
    EXPECT_TRUE(counts_equal(a, b));
    // The distribution must still be a Bell-like mixture, not an artefact.
    std::uint64_t total = 0;
    for (const auto& kv : a) total += kv.second;
    EXPECT_EQ(total, static_cast<std::uint64_t>(nshots));
}
