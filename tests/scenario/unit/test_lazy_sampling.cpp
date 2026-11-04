// test_lazy_sampling.cpp — Result::counts() deferred sampling.
// The FullState CPU fast path defers the nshots inverse-CDF draw to the
// first counts() call. These tests verify: (1) the deferred distribution still
// matches theory, (2) the final state is available without ever sampling,
// (3) repeated counts() calls are stable (cached), (4) a fixed seed reproduces
// identical counts through the lazy path, and (5) the per-shot path (IfOp) stays
// eager and correct.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <cstdlib>   // std::getenv, setenv, unsetenv — threshold env override
#include <complex>   // std::abs(std::complex) — GPU deferred statevector check
#include <map>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"

using qplayer::QRegister;
using qplayer::Circuit;
using qplayer::SimType;
using qplayer::ProcType;
using qplayer::Result;
using qplayer::StateVector;
using qplayer::GateOp;
using qplayer::GateType;

namespace {
double frac(const std::map<std::string, std::uint64_t>& c, const char* k,
            double shots) {
    auto it = c.find(k);
    return it == c.end() ? 0.0 : static_cast<double>(it->second) / shots;
}
}  // namespace

// Lazy draw still yields the correct Bell distribution on first counts() access.
TEST(LazySampling, FastPath_DeferredCounts_MatchesTheory) {
    // Arrange
    QRegister qr(SimType::FullState);
    qr.set_seed(42);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act: run() must not sample; counts() triggers the deferred draw.
    Result r = qr.run(circ, 10000);
    const auto& counts = r.counts();

    // Assert
    EXPECT_NEAR(frac(counts, "00", 10000.0), 0.5, 0.02);  // stat tol @ 10000
    EXPECT_NEAR(frac(counts, "11", 10000.0), 0.5, 0.02);
    EXPECT_NEAR(frac(counts, "01", 10000.0), 0.0, 1e-9);
    EXPECT_NEAR(frac(counts, "10", 10000.0), 0.0, 1e-9);
}

// |psi_final> is exposed by run() before any sampling occurs (no counts() call).
TEST(LazySampling, FastPath_StatevectorAvailableWithoutSampling) {
    // Arrange
    QRegister qr(SimType::FullState);
    qr.set_seed(7);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act: never call counts(); the statevector must still be present.
    Result r = qr.run(circ, 1000);

    // Assert: Bell amplitudes |00>,|11> = 1/sqrt(2); |01>,|10> = 0.
    EXPECT_TRUE(r.has_statevector());
    const StateVector& sv = r.statevector();
    EXPECT_NEAR(sv[0].real(), 0.70710678, 1e-6);
    EXPECT_NEAR(sv[3].real(), 0.70710678, 1e-6);
    EXPECT_NEAR(sv[1].real(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0, 1e-12);
}

// Repeated counts() calls return the identical cached map (single draw).
TEST(LazySampling, FastPath_RepeatedCounts_AreStable) {
    // Arrange
    QRegister qr(SimType::FullState);
    qr.set_seed(123);
    Circuit circ(3, 3);
    circ.h(0).h(1).h(2).measure(0, 0).measure(1, 1).measure(2, 2);

    // Act
    Result r = qr.run(circ, 4096);
    const auto first = r.counts();   // copy of the cached map after first draw
    const auto& second = r.counts(); // must be byte-identical (cache hit)

    // Assert
    EXPECT_TRUE(first == second);
}

// A fixed seed reproduces identical counts across two independent runs via lazy.
TEST(LazySampling, FastPath_FixedSeed_Reproducible) {
    // Arrange
    auto run_once = [](std::uint64_t seed) {
        QRegister qr(SimType::FullState);
        qr.set_seed(seed);
        Circuit circ(3, 3);
        circ.h(0).h(1).h(2).measure(0, 0).measure(1, 1).measure(2, 2);
        return qr.run(circ, 4096).counts();
    };

    // Act
    const auto a = run_once(2026);
    const auto b = run_once(2026);

    // Assert: same seed -> identical lazy draw.
    EXPECT_TRUE(a == b);
}

// Per-shot path (IfOp) stays eager and produces the correct deterministic result.
// X on q0, measure->c0, then if(creg0==1) X on q1, measure->c1: every shot -> "11".
TEST(LazySampling, PerShotPath_IfOp_EagerAndCorrect) {
    // Arrange: default creg index 0 (width 2). Mid-circuit measure + IfOp routes
    // to the per-shot path, which samples eagerly inside run().
    QRegister qr(SimType::FullState);
    qr.set_seed(99);
    Circuit circ(2, 2);
    circ.x(0).measure(0, 0);
    GateOp x_gate;
    x_gate.type   = GateType::X;
    x_gate.qubits = {1};
    circ.if_then(0, 1, x_gate);
    circ.measure(1, 1);
    EXPECT_TRUE(circ.has_classical_control());

    // Act
    Result r = qr.run(circ, 256);
    const auto& counts = r.counts();

    // Assert: deterministic — all shots collapse to "11".
    EXPECT_EQ(counts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(counts.at("11"), static_cast<std::uint64_t>(256));
}

// ── GPU (dense) + SparseState (sparse) lazy ──────────────────

// FullState GPU lazy (dense mode). GPU execute() downloads the
// state and attaches |psi> + a lazy snapshot; counts() draws on first call.
// (Falls back to CPU if no CUDA device — lazy behaviour holds either way.)
TEST(LazySampling, GPU_DeferredCounts_MatchesAndReproducible) {
    // Arrange
    auto run_once = [](std::uint64_t seed) {
        QRegister qr(SimType::FullState, ProcType::GPU);
        qr.set_seed(seed);
        Circuit circ(2, 2);
        circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
        Result r = qr.run(circ, 8192);
        EXPECT_TRUE(r.has_statevector());   // state attached before sampling
        return r.counts();
    };

    // Act
    const auto a = run_once(2026);
    const auto b = run_once(2026);

    // Assert: Bell correlated outcomes + fixed-seed reproducibility.
    EXPECT_NEAR(frac(a, "00", 8192.0), 0.5, 0.03);
    EXPECT_NEAR(frac(a, "11", 8192.0), 0.5, 0.03);
    EXPECT_NEAR(frac(a, "01", 8192.0), 0.0, 1e-9);
    EXPECT_TRUE(a == b);
}

// SparseState lazy (sparse mode). counts() draws from the sparse snapshot.
TEST(LazySampling, Sparse_DeferredCounts_MatchesAndReproducible) {
    // Arrange
    auto run_once = [](std::uint64_t seed) {
        QRegister qr(SimType::SparseState);
        qr.set_seed(seed);
        Circuit circ(2, 2);
        circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
        return qr.run(circ, 8192).counts();
    };

    // Act
    const auto a = run_once(7);
    const auto b = run_once(7);

    // Assert: Bell distribution + reproducibility through the sparse lazy path.
    EXPECT_NEAR(frac(a, "00", 8192.0), 0.5, 0.03);
    EXPECT_NEAR(frac(a, "11", 8192.0), 0.5, 0.03);
    EXPECT_NEAR(frac(a, "01", 8192.0), 0.0, 1e-9);
    EXPECT_TRUE(a == b);
}

// SparseState lazy beyond MAX_STATEVECTOR_QUBITS (=25): a 30-qubit GHZ has only
// two non-zero amplitudes, so no dense statevector is attached — sparse lazy must
// still produce counts from the O(nnz) snapshot. This is the capability the dense
// (statevector_) path could not provide.
TEST(LazySampling, Sparse_LargeNoStatevector_LazyWorks) {
    // Arrange: 30-qubit GHZ |0..0> + |1..1>.
    QRegister qr(SimType::SparseState);
    qr.set_seed(123);
    const qplayer::QubitIndex n = 30;
    Circuit circ(n, n);
    circ.h(0);
    for (qplayer::QubitIndex q = 0; q + 1 < n; ++q) { circ.cx(q, q + 1); }
    circ.measure_all();

    // Act
    Result r = qr.run(circ, 4096);
    EXPECT_FALSE(r.has_statevector());   // > 25 qubits: no densified state
    const auto& counts = r.counts();     // sparse lazy draw

    // Assert: only the two GHZ outcomes appear, ~50/50.
    const std::string zeros(n, '0');
    const std::string ones(n, '1');
    EXPECT_EQ(counts.size(), static_cast<std::size_t>(2));
    EXPECT_NEAR(frac(counts, zeros.c_str(), 4096.0), 0.5, 0.04);
    EXPECT_NEAR(frac(counts, ones.c_str(), 4096.0), 0.5, 0.04);
}

// ── : GPU-resident sampling (Phase 7-C) ─────────────────────────────

// counts() on the GPU path now samples on-device (no 2GiB D2H); statevector()
// triggers a *deferred* D2H. Verify the deferred materialization yields the
// correct Bell amplitudes even though counts() was the only prior access, and
// that statevector() works when counts() is never called at all (counts-only
// must not be required to populate the state). (Falls back to CPU sans CUDA.)
TEST(LazySampling, GPU_StatevectorDeferred_MaterializesCorrectly) {
    // Arrange: Bell pair, GPU backend.
    QRegister qr(SimType::FullState, ProcType::GPU);
    qr.set_seed(99);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act: statevector() WITHOUT calling counts() first (deferred D2H path).
    Result r = qr.run(circ, 1024);
    EXPECT_TRUE(r.has_statevector());        // available (deferred),
    const StateVector& sv = r.statevector(); // triggers the lazy D2H

    // Assert: |psi> = (|00> + |11>)/sqrt(2). Index 0 = |00>, index 3 = |11>.
    const double inv_sqrt2 = 0.7071067811865476;
    EXPECT_NEAR(std::abs(sv[0]), inv_sqrt2, 1e-9);
    EXPECT_NEAR(std::abs(sv[3]), inv_sqrt2, 1e-9);
    EXPECT_NEAR(std::abs(sv[1]), 0.0, 1e-9);
    EXPECT_NEAR(std::abs(sv[2]), 0.0, 1e-9);

    // counts() after statevector() must still draw correctly (both orders work).
    const auto& counts = r.counts();
    EXPECT_NEAR(frac(counts, "00", 1024.0), 0.5, 0.06);
    EXPECT_NEAR(frac(counts, "11", 1024.0), 0.5, 0.06);
    EXPECT_NEAR(frac(counts, "01", 1024.0), 0.0, 1e-9);
}

// GPU device sampling shares the host mt19937_64(seed) uniform stream with the
// CPU path, so the same seed must give a statistically
// equivalent distribution on both backends (GPU CDF is a parallel scan, so not
// bit-for-bit — checked within statistical tolerance).
TEST(LazySampling, GPU_vs_CPU_SameSeed_DistributionParity) {
    // Arrange: a 4-qubit GHZ so a few outcomes carry meaningful mass.
    auto run_on = [](ProcType proc) {
        QRegister qr(SimType::FullState, proc);
        qr.set_seed(4242);
        Circuit circ(4, 4);
        circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3).measure_all();
        return qr.run(circ, 16384).counts();
    };

    // Act
    const auto gpu = run_on(ProcType::GPU);
    const auto cpu = run_on(ProcType::CPU);

    // Assert: GHZ -> ~50% |0000>, ~50% |1111>; GPU and CPU agree within tol.
    EXPECT_NEAR(frac(gpu, "0000", 16384.0), 0.5, 0.02);
    EXPECT_NEAR(frac(gpu, "1111", 16384.0), 0.5, 0.02);
    EXPECT_NEAR(frac(gpu, "0000", 16384.0), frac(cpu, "0000", 16384.0), 0.03);
    EXPECT_NEAR(frac(gpu, "1111", 16384.0), frac(cpu, "1111", 16384.0), 0.03);
}

// ── : CPU-resident SoA sampling (Phase 7-F) ─────────────────────────

// P2: statevector() deferred — lazy SoA->AoS materialisation must yield the
// exact Bell amplitudes. run() sets cpu_resident; statevector() AoS-materialises
// on first access without having called counts() first.
TEST(LazySampling, CpuResident_StatevectorDeferred_MaterializesCorrectly) {
    // Arrange: Bell pair, CPU backend.
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(99);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act: access statevector() WITHOUT calling counts() first.
    Result r = qr.run(circ, 1024);
    EXPECT_TRUE(r.has_statevector());         // CPU-resident -> available
    const StateVector& sv = r.statevector();  // triggers lazy SoA->AoS materialize

    // Assert: |psi> = (|00> + |11>) / sqrt(2). Index 0 = |00>, index 3 = |11>.
    // Tolerance 1e-9: exact formula, no statistical noise.
    const double inv_sqrt2 = 0.7071067811865476;
    EXPECT_NEAR(std::abs(sv[0]), inv_sqrt2, 1e-9);
    EXPECT_NEAR(std::abs(sv[3]), inv_sqrt2, 1e-9);
    EXPECT_NEAR(std::abs(sv[1]), 0.0, 1e-9);
    EXPECT_NEAR(std::abs(sv[2]), 0.0, 1e-9);

    // counts() after statevector() must also work (both orders valid).
    const auto& counts = r.counts();
    EXPECT_NEAR(frac(counts, "00", 1024.0), 0.5, 0.06);  // stat tol @ 1024
    EXPECT_NEAR(frac(counts, "11", 1024.0), 0.5, 0.06);
    EXPECT_NEAR(frac(counts, "01", 1024.0), 0.0, 1e-9);
}

// P2 + : CPU-resident lazy counts must be bit-exact for a fixed seed,
// matching the prior eager/AoS-lazy path. Two independent runs with the same
// seed must produce identical count maps (Golden invariance check).
TEST(LazySampling, CpuResident_LazyVsEager_FixedSeed_BitExact) {
    auto run_once = [](std::uint64_t seed) {
        QRegister qr(SimType::FullState, ProcType::CPU);
        qr.set_seed(seed);
        const qplayer::QubitIndex n = 10;
        Circuit circ(n, n);
        for (qplayer::QubitIndex q = 0; q < n; ++q) { circ.h(q); }
        for (qplayer::QubitIndex q = 0; q + 1 < n; ++q) { circ.cx(q, q + 1); }
        circ.measure_all();
        return qr.run(circ, 4096).counts();
    };

    // Same seed -> bit-identical counts through the SoA lazy path (P2).
    const auto a = run_once(20260616);
    const auto b = run_once(20260616);
    EXPECT_TRUE(a == b);

    // Sanity: total shots preserved.
    std::uint64_t total = 0;
    for (const auto& kv : a) { total += kv.second; }
    EXPECT_EQ(total, std::uint64_t{4096});
}

// P3: buffer reuse safety — two consecutive runs of the same circuit (same dim)
// reuse the buffer (use_count==1); results must be independent and correct.
// Also verifies that keeping the first Result alive forces a new allocation
// (use_count>1 -> new buffer), preventing clobber of the first result.
TEST(LazySampling, CpuResident_BufferReuse_ConsecutiveRuns_Independent) {
    // Arrange: GHZ 4q.
    auto make_ghz4 = []() {
        Circuit c(4, 4);
        c.h(0).cx(0, 1).cx(1, 2).cx(2, 3).measure_all();
        return c;
    };

    {
        // Case A: run twice without keeping the first Result — buffer should be
        // reused (no allocation on second run if dim matches). Results are independent.
        QRegister qr(SimType::FullState);
        qr.set_seed(111);
        Circuit circ = make_ghz4();
        auto r1 = qr.run(circ, 2048);
        // r1 goes out of scope here, so use_count will be 1 before second run.
        const auto c1 = r1.counts();  // trigger lazy draw now
        // Verify r1 is a proper GHZ distribution before we lose the buffer.
        EXPECT_NEAR(frac(c1, "0000", 2048.0), 0.5, 0.05);
        EXPECT_NEAR(frac(c1, "1111", 2048.0), 0.5, 0.05);
    }

    {
        // Case B: keep r1 alive while running r2. use_count==2 so init_state()
        // must allocate a fresh buffer; r1 must not be clobbered.
        QRegister qr(SimType::FullState);
        qr.set_seed(222);
        Circuit circ = make_ghz4();
        Result r1 = qr.run(circ, 2048);
        // r1 is alive; its cpu_owner_ keeps the buffer alive (use_count>=2).
        qr.set_seed(333);
        Result r2 = qr.run(circ, 2048);  // must allocate fresh buffer

        // Both results must be independently correct.
        const auto& c1 = r1.counts();
        const auto& c2 = r2.counts();
        EXPECT_NEAR(frac(c1, "0000", 2048.0), 0.5, 0.05);
        EXPECT_NEAR(frac(c1, "1111", 2048.0), 0.5, 0.05);
        EXPECT_NEAR(frac(c2, "0000", 2048.0), 0.5, 0.05);
        EXPECT_NEAR(frac(c2, "1111", 2048.0), 0.5, 0.05);

        // The statevector of r1 must also still be valid (lazy materialize after
        // the backend already moved to r2's buffer — cpu_owner_ keeps r1 alive).
        EXPECT_TRUE(r1.has_statevector());
        const StateVector& sv1 = r1.statevector();
        // GHZ 4q: |psi> = (|0000> + |1111>) / sqrt(2). Index 0 and 15.
        const double inv_sqrt2 = 0.7071067811865476;
        EXPECT_NEAR(std::abs(sv1[0]),  inv_sqrt2, 1e-9);
        EXPECT_NEAR(std::abs(sv1[15]), inv_sqrt2, 1e-9);
    }
}

// ── : size-gated parallel CDF (large-circuit path) ──────────────────

// Reproducibility: the parallel block-scan path is the default for dimi > 2^18
// (contention resilience). We pin it ON explicitly by lowering
// QPLAYER_PARALLEL_SAMPLE_THRESHOLD to 2048 (> 1024 Golden guard, well below the
// 24q circuit's 2^24 states), so the test is independent of the default and the
// dense path provably takes the parallel branch.  The same fixed seed must produce
// identical count maps on two independent runs regardless of OMP thread count
// block boundaries depend only on dimi and kScanBlock (both
// compile-time constants) — determinism is guaranteed by design.
// NOTE: "0" does NOT work as a force value — the env guard rejects <= 1024 and
// falls back to the default (serial). Use a value in (1024, dim).
TEST(LazySampling, ParallelCdf_LargeCircuit_FixedSeed_Reproducible) {
    // Force parallel path: threshold 2048 < 2^24 -> 24q circuit goes parallel.
    // The env var is read once per process as a static; set it before the first
    // counts() call. (setenv is POSIX-only — Linux target)
    const char* prev = std::getenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD");
    setenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD", "2048", /*overwrite=*/1);

    // Arrange: 24-qubit H-layer + adjacent CX pairs -> broad superposition.
    // 2^24 = 16,777,216 — large enough to exercise the block-scan CDF path,
    // and small enough to run quickly in the test suite (< 2s per run).
    // With threshold forced to 2048 this always takes the parallel path.
    auto run_once = [](std::uint64_t seed) {
        QRegister qr(SimType::FullState, ProcType::CPU);
        qr.set_seed(seed);
        const qplayer::QubitIndex n = 24;
        Circuit circ(n, n);
        for (qplayer::QubitIndex q = 0; q < n; ++q) { circ.h(q); }
        for (qplayer::QubitIndex q = 0; q + 1u < n; ++q) { circ.cx(q, q + 1u); }
        circ.measure_all();
        return qr.run(circ, 4096).counts();
    };

    // Act: two runs with the same seed.
    const auto a = run_once(20260616);
    const auto b = run_once(20260616);

    // Restore env (best-effort — test framework re-uses the same process).
    if (prev != nullptr) setenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD", prev, 1);
    else unsetenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD");

    // Assert: bit-identical count maps (determinism via fixed block size).
    EXPECT_TRUE(a == b);

    // Sanity: all 4096 shots accounted for.
    std::uint64_t total_a = 0;
    for (const auto& kv : a) { total_a += kv.second; }
    EXPECT_EQ(total_a, std::uint64_t{4096});
}

// Threshold boundary: both the serial path (small circuit) and the parallel scan
// path (large circuit, forced via env) must produce correct total shot counts and
// non-empty distributions.
//
// The default threshold is 2^18 (parallel for >= ~18 qubits); we test
// both paths explicitly by overriding the env var rather than relying on the default.
TEST(LazySampling, ParallelCdf_ThresholdBoundary_CorrectCounts) {
    // Serial path: force threshold to 2^40 (always serial), use a 16-qubit
    // circuit (65536 states) well below any reasonable default.
    {
        const char* prev = std::getenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD");
        setenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD",
               "1099511627776" /* 2^40 */, /*overwrite=*/1);

        QRegister qr(SimType::FullState, ProcType::CPU);
        qr.set_seed(42);
        Circuit circ(static_cast<qplayer::QubitIndex>(16),
                     static_cast<qplayer::QubitIndex>(16));
        for (qplayer::QubitIndex q = 0; q < 16u; ++q) { circ.h(q); }
        circ.measure_all();
        const auto c = qr.run(circ, 4096).counts();
        std::uint64_t tot = 0;
        for (const auto& kv : c) { tot += kv.second; }
        EXPECT_EQ(tot, std::uint64_t{4096});
        EXPECT_TRUE(c.size() > 1u);  // H^16 -> many distinct outcomes

        if (prev != nullptr) setenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD", prev, 1);
        else unsetenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD");
    }

    // Parallel path: force threshold to 2048 (> 1024 guard, < 2^20), use a
    // 20-qubit circuit (1,048,576 states). Exercises the block-scan code path.
    {
        const char* prev = std::getenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD");
        setenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD", "2048", /*overwrite=*/1);

        QRegister qr(SimType::FullState, ProcType::CPU);
        qr.set_seed(42);
        Circuit circ(static_cast<qplayer::QubitIndex>(20),
                     static_cast<qplayer::QubitIndex>(20));
        for (qplayer::QubitIndex q = 0; q < 20u; ++q) { circ.h(q); }
        circ.measure_all();
        const auto c = qr.run(circ, 4096).counts();
        std::uint64_t tot = 0;
        for (const auto& kv : c) { tot += kv.second; }
        EXPECT_EQ(tot, std::uint64_t{4096});
        EXPECT_TRUE(c.size() > 1u);  // H^20 -> many distinct outcomes

        if (prev != nullptr) setenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD", prev, 1);
        else unsetenv("QPLAYER_PARALLEL_SAMPLE_THRESHOLD");
    }
}

// ── CPU dense sampling: bit-exact parallelisation ───────────────────

// The dense prob/normalize build is now OpenMP-parallel while prob_sum and the
// prefix-sum stay serial so a fixed seed must still give bit-for-bit
// identical counts across runs — independent of thread count. Use a 12-qubit
// QFT-like superposition (4096 basis states) so the parallel-for engages over a
// non-trivial range, then assert two same-seed runs match exactly.
TEST(LazySampling, CpuDenseParallel_FixedSeed_BitExactReproducible) {
    auto run_once = []() {
        QRegister qr(SimType::FullState, ProcType::CPU);
        qr.set_seed(20260615);
        const qplayer::QubitIndex n = 12;
        Circuit circ(n, n);
        for (qplayer::QubitIndex q = 0; q < n; ++q) { circ.h(q); }
        for (qplayer::QubitIndex q = 0; q + 1 < n; ++q) { circ.cx(q, q + 1); }
        circ.measure_all();
        return qr.run(circ, 8192).counts();
    };

    const auto a = run_once();
    const auto b = run_once();

    // Exact map equality: same seed -> identical counts (bit-exact).
    EXPECT_TRUE(a == b);
    std::uint64_t total = 0;
    for (const auto& kv : a) { total += kv.second; }
    EXPECT_EQ(total, std::uint64_t{8192});
}
