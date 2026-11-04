// test_tn_gpu_svd.cpp — TensorNetwork cuSolver SVD offload.
// Tests follow the Arrange / Act / Assert layout.
//
// The offload has one contract and it is not about speed: the GPU must not
// change the answer. So the claims here are
//   (1) it declines rather than throws when it cannot help (no CUDA build, no
//       device, matrix below the break-even) — a GPU that cannot run must never
//       be able to fail a run the CPU could finish,
//   (2) a circuit run with the offload requested matches the CPU state, and
//   (3) the offload is opt-in — ProcType::Auto must not silently take it.
//
// These pass on a CPU-only build too: there gpu_svd_available() is false, the
// offload declines, and every path is the CPU path. That is deliberate — a test
// that only means something on one machine is a test that rots.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <omp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/backend/tensor_network/tn_svd_gpu.hpp"
#include "../../../include/backend/tensor_network/blas_loader.hpp"
#include "../../../include/noise/noise_model.hpp"

using qplayer::Circuit;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::SimType;

namespace {

// Brickwork: entangling enough that the bonds grow past the offload threshold,
// small enough that the dense comparison stays cheap.
Circuit brickwork(std::uint32_t n, int depth, bool measure) {
    Circuit c(n, measure ? n : 0);
    for (std::uint32_t q = 0; q < n; ++q) c.h(q);
    double angle = 0.1;
    for (int layer = 0; layer < depth; ++layer) {
        for (std::uint32_t q = layer % 2; q + 1 < n; q += 2) c.cz(q, q + 1);
        for (std::uint32_t q = 0; q < n; ++q) {
            c.rz(angle, q);
            c.rx(angle * 1.7 + 0.3, q);
            angle += 0.037;
        }
    }
    if (measure) c.measure_all();
    return c;
}

std::vector<std::complex<double>> run_state(ProcType proc, const Circuit& c) {
    QRegister qr(SimType::TensorNetwork, proc);
    qr.run(c, 1);
    return qr.get_statevector();
}

}  // namespace

// A matrix below the measured break-even must be refused by the offload itself,
// so the caller never has to know the threshold.
TEST(TnGpuSvd, BelowBreakEvenDimension_OffloadDeclines) {
    // Arrange
    const int dim = qplayer::detail::kGpuSvdMinDim - 1;
    std::vector<std::complex<double>> a(static_cast<std::size_t>(dim) * dim, {1.0, 0.0});
    std::vector<double> s(static_cast<std::size_t>(dim), 0.0);
    std::vector<std::complex<double>> u(a.size()), vt(a.size());

    // Act
    const bool taken = qplayer::detail::gpu_zgesvd_thin(dim, dim, a.data(), s.data(),
                                                        u.data(), vt.data());

    // Assert
    EXPECT_FALSE(taken);
}

// The probe must answer without throwing on every host, because the dispatch in
// mps.cpp calls it on the hot path and has no way to recover from an exception.
TEST(TnGpuSvd, AvailabilityProbe_DoesNotThrow) {
    EXPECT_NO_THROW(static_cast<void>(qplayer::detail::gpu_svd_available()));
}

// The contract: same circuit, same answer, whichever processor was asked for.
// On a CPU-only build this compares CPU against CPU and still guards the
// dispatch wiring — a bug that made the GPU branch corrupt state would fail
// here on a GPU host and be inert elsewhere, which is the correct asymmetry.
TEST(TnGpuSvd, ExplicitGpuRequest_MatchesCpuState) {
    // Arrange
    // depth 12 drives chi to 64, so the merged matrix is 128x128 -- above
    // kGpuSvdMinDim. A shallower circuit would compare the CPU against
    // itself and prove nothing.
    const Circuit c = brickwork(14, 12, /*measure=*/false);

    // Act
    const std::vector<std::complex<double>> cpu = run_state(ProcType::CPU, c);
    const std::vector<std::complex<double>> gpu = run_state(ProcType::GPU, c);

    // Assert
    EXPECT_EQ(cpu.size(), gpu.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < cpu.size(); ++i)
        worst = std::max(worst, std::abs(cpu[i] - gpu[i]));
    // 1e-10 is the E cross-backend tolerance; the measured offload
    // difference is ~1e-15, so this fails by orders of magnitude if the GPU
    // branch ever returns a different state.
    EXPECT_NEAR(worst, 0.0, 1e-10);
}

// Counts must agree too: the state comparison above cannot see a sampling path
// that read the chain differently.
TEST(TnGpuSvd, ExplicitGpuRequest_MatchesCpuCounts) {
    // Arrange
    const Circuit c = brickwork(12, 12, /*measure=*/true);   // chi 64 -> offload fires
    QRegister cpu(SimType::TensorNetwork, ProcType::CPU);
    QRegister gpu(SimType::TensorNetwork, ProcType::GPU);
    cpu.set_seed(20260827);
    gpu.set_seed(20260827);

    // Act
    const auto a = cpu.run(c, 512).counts();
    const auto b = gpu.run(c, 512).counts();

    // Assert
    EXPECT_EQ(a.size(), b.size());
    double tvd = 0.0;
    for (const auto& [key, n] : a) {
        const auto it = b.find(key);
        const double q = (it == b.end()) ? 0.0 : static_cast<double>(it->second);
        tvd += std::abs(static_cast<double>(n) - q);
    }
    tvd /= (2.0 * 512.0);
    // F: TN GPU is not a bit-identical backend, so this is a
    // distribution claim rather than an equality one. At 1e-15 amplitude
    // differences the two draws agree in practice, and a real divergence
    // (a wrong subspace, a transposed factor) moves this far off zero.
    EXPECT_NEAR(tvd, 0.0, 0.02);
}

// Above the break-even the offload must actually RUN on a GPU build, and its
// factorisation must agree with LAPACK's. This is the only test that reaches
// gpu_zgesvd_thin with a matrix it will accept, so without it the circuit-level
// tests above could pass while the offload silently never fired.
TEST(TnGpuSvd, AboveBreakEven_AgreesWithLapack) {
    // Arrange — a low-rank matrix, which is the shape an MPS block actually has.
    const int n = 128;
    std::vector<std::complex<double>> a(static_cast<std::size_t>(n) * n, {0.0, 0.0});
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            double re = 0.0, im = 0.0;
            for (int t = 0; t < n / 4; ++t) {
                const double p = 0.017 * (i + 1) * (t + 1);
                const double q = 0.023 * (j + 1) * (t + 1);
                re += std::cos(p) * std::cos(q);
                im += std::sin(p) * std::cos(q);
            }
            a[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * n] = {re, im};
        }
    std::vector<double> s_gpu(static_cast<std::size_t>(n), 0.0);
    std::vector<std::complex<double>> u_gpu(a.size()), vt_gpu(a.size());

    // Act
    const bool taken = qplayer::detail::gpu_zgesvd_thin(n, n, a.data(), s_gpu.data(),
                                                        u_gpu.data(), vt_gpu.data());

    // Assert
    if (!qplayer::detail::gpu_svd_available()) {
        EXPECT_FALSE(taken);          // CPU-only build or no device: declines
        return;
    }
    EXPECT_TRUE(taken);

    std::vector<std::complex<double>> work_a = a;   // zgesvd_ destroys its input
    std::vector<double> s_cpu(static_cast<std::size_t>(n), 0.0);
    std::vector<double> rwork(static_cast<std::size_t>(5 * n), 0.0);
    std::vector<std::complex<double>> u_cpu(a.size()), vt_cpu(a.size());
    int info = 0, lwork = -1;
    std::complex<double> query{0.0, 0.0};
    qplayer::detail::zgesvd("S", "S", &n, &n, work_a.data(), &n, s_cpu.data(),
                            u_cpu.data(), &n, vt_cpu.data(), &n, &query, &lwork,
                            rwork.data(), &info);
    lwork = static_cast<int>(query.real());
    std::vector<std::complex<double>> work(static_cast<std::size_t>(lwork > 0 ? lwork : 1));
    qplayer::detail::zgesvd("S", "S", &n, &n, work_a.data(), &n, s_cpu.data(),
                            u_cpu.data(), &n, vt_cpu.data(), &n, work.data(), &lwork,
                            rwork.data(), &info);
    EXPECT_EQ(info, 0);

    // Singular values are unique, so they must agree; the singular VECTORS are
    // gauge and deliberately are not compared (B).
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::abs(s_cpu[static_cast<std::size_t>(i)] -
                                         s_gpu[static_cast<std::size_t>(i)]));
    EXPECT_NEAR(worst / (s_cpu[0] > 0.0 ? s_cpu[0] : 1.0), 0.0, 1e-10);
}

// A flat Schmidt spectrum under a binding cap -- the shape a stabilizer state
// has, and the regime where E says the two SVD implementations may keep
// different (equally valid) subspaces.
//
// HONEST LIMIT: this test does NOT prove the degeneracy fallback ran. Checking
// stderr showed no fallback warning here, so the offload either declined on the
// gates that carry the flat bond or the boundary did not land inside it. What
// the test does guarantee is the property users care about -- GPU and CPU agree
// on a flat-spectrum circuit that truncates -- and it would fail loudly if the
// GPU ever started keeping a different subspace on this input. The fallback
// mechanism itself is evidenced, which
// measures the 0.46~0.67 divergence directly on raw matrices; an in-tree test
// that provably reaches the fallback is still missing (see the session log).
TEST(TnGpuSvd, FlatSpectrumUnderCap_GpuMatchesCpu) {
    // Arrange — 14 qubits, 7 crossing Bell pairs => centre chi = 128, all equal.
    //
    // The cap has to satisfy two things at once, which is why these numbers are
    // what they are: the merged matrix is 2 * (left bond) and the left bond is
    // itself clamped by the cap, so a cap of 64 is the smallest that keeps the
    // matrix at 128 >= kGpuSvdMinDim -- and the degeneracy must exceed the cap
    // for the boundary to land inside it, which needs 2^7 = 128 > 64. A cap of
    // 16 (the obvious first choice) makes the matrix 32x32, the offload declines,
    // and the test silently stops testing anything.
    const std::uint32_t n = 14;
    Circuit c(n, 0);
    for (std::uint32_t i = 0; i < n / 2; ++i) {
        c.h(i);
        c.cx(i, n - 1 - i);
    }
    // Merged matrix 2*64 = 128 >= kGpuSvdMinDim, so the offload is eligible, and
    // the cap of 64 cuts inside the 128-fold degeneracy.
    auto run = [&](ProcType proc) {
        QRegister qr(SimType::TensorNetwork, proc);
        qr.set_mps_truncation(64, 1e-12);
        qr.run(c, 1);
        return qr.get_statevector();
    };

    // Act
    const std::vector<std::complex<double>> cpu = run(ProcType::CPU);
    const std::vector<std::complex<double>> gpu = run(ProcType::GPU);

    // Assert
    EXPECT_EQ(cpu.size(), gpu.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < cpu.size(); ++i)
        worst = std::max(worst, std::abs(cpu[i] - gpu[i]));
    // Without the fallback this is O(0.1); with it the two are the same CPU
    // factorisation and agree to rounding.
    EXPECT_NEAR(worst, 0.0, 1e-10);
}

// C: Auto must stay on the CPU. If this ever flips, existing users get
// a silently different answer, which is exactly what the ADR refused.
TEST(TnGpuSvd, AutoProcType_DoesNotEnableOffload) {
    // Arrange
    const Circuit c = brickwork(14, 12, /*measure=*/false);

    // Act
    const std::vector<std::complex<double>> autoproc = run_state(ProcType::Auto, c);
    const std::vector<std::complex<double>> cpu      = run_state(ProcType::CPU, c);

    // Assert — bit-identical, not merely close: Auto is the CPU path.
    EXPECT_EQ(autoproc.size(), cpu.size());
    bool identical = true;
    for (std::size_t i = 0; i < cpu.size(); ++i)
        if (autoproc[i] != cpu[i]) { identical = false; break; }
    EXPECT_TRUE(identical);
}

// B: the offload is a fixed pool, kGpuSvdSlotsPerDevice per eligible
// device. The count is what bounds VRAM and device contention, so it is pinned.
TEST(TnGpuSvd, SlotPool_WholeDevicesOnly) {
    // Act
    const int slots = qplayer::detail::gpu_svd_slot_count();

    // Assert
    if (!qplayer::detail::gpu_svd_available()) {
        EXPECT_EQ(slots, 0);
        return;
    }
    EXPECT_TRUE(slots >= qplayer::detail::kGpuSvdSlotsPerDevice);
    EXPECT_EQ(slots % qplayer::detail::kGpuSvdSlotsPerDevice, 0);
}

// More threads than slots, both matrix orientations at once: every caller must
// get its own correct factorisation. A slot handed to two threads, a stream
// synchronised on the wrong device, or a lease that never returns (deadlock)
// all fail here -- the last one by hanging, which ctest's timeout reports.
TEST(TnGpuSvd, ConcurrentCallsBeyondSlotCount_EachFactorisationCorrect) {
    // Arrange
    constexpr int kCalls = 48;
    const int threads = std::max(2 * qplayer::detail::gpu_svd_slot_count(), 8);
    std::vector<double> residual(kCalls, 0.0);
    std::vector<int>    taken(kCalls, 0);

    // Act
#pragma omp parallel for schedule(dynamic) num_threads(threads)
    for (int call = 0; call < kCalls; ++call) {
        // Odd calls take the rows < cols branch (factorise A^H, swap back).
        const int rows = (call % 2 == 0) ? 160 : 96;
        const int cols = (call % 2 == 0) ? 96 : 160;
        const int k    = std::min(rows, cols);
        std::vector<std::complex<double>> a(static_cast<std::size_t>(rows) * cols);
        for (int j = 0; j < cols; ++j)
            for (int i = 0; i < rows; ++i) {
                const double p = 0.013 * (i + 1) * (call + 3) + 0.029 * (j + 1);
                a[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * rows] =
                    {std::cos(p), std::sin(1.3 * p)};
            }
        std::vector<double> s(static_cast<std::size_t>(k));
        std::vector<std::complex<double>> u(static_cast<std::size_t>(rows) * k);
        std::vector<std::complex<double>> vt(static_cast<std::size_t>(k) * cols);
        taken[call] = qplayer::detail::gpu_zgesvd_thin(rows, cols, a.data(), s.data(),
                                                       u.data(), vt.data()) ? 1 : 0;
        if (taken[call] == 0) continue;
        double err = 0.0, norm = 0.0;
        for (int j = 0; j < cols; ++j)
            for (int i = 0; i < rows; ++i) {
                std::complex<double> acc{0.0, 0.0};
                for (int t = 0; t < k; ++t)
                    acc += u[static_cast<std::size_t>(i) + static_cast<std::size_t>(t) * rows] *
                           s[static_cast<std::size_t>(t)] *
                           vt[static_cast<std::size_t>(t) + static_cast<std::size_t>(j) * k];
                const std::complex<double> ref =
                    a[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * rows];
                err  += std::norm(acc - ref);
                norm += std::norm(ref);
            }
        residual[call] = std::sqrt(err / norm);
    }

    // Assert
    int n_taken = 0;
    for (int t : taken) n_taken += t;
    EXPECT_EQ(n_taken, qplayer::detail::gpu_svd_available() ? kCalls : 0);
    // 1e-10: a full-rank reconstruction in double lands near 1e-14; a factor
    // from another call's buffers is O(1) off.
    EXPECT_NEAR(*std::max_element(residual.begin(), residual.end()), 0.0, 1e-10);
}

namespace {

// Noisy brickwork whose bonds reach chi 64, so the trajectory path sends its
// 128x128 SVDs to the pool from as many threads as there are shots.
std::map<std::string, std::uint64_t> noisy_counts(ProcType proc, std::uint64_t seed) {
    const Circuit c = brickwork(12, 12, /*measure=*/true);
    NoiseModel nm;
    nm.add_depolarizing("cz", 0.01);
    QRegister qr(SimType::TensorNetwork, proc);
    qr.set_noise_model(nm);
    qr.set_seed(seed);
    return qr.run(c, 256).counts();
}

}  // namespace

// Which slot -- and so which device -- a trajectory draws depends on thread
// timing. The pool admits only devices of one model so that this cannot reach
// the answer; a seeded run must repeat exactly (C).
TEST(TnGpuSvd, NoisyTrajectoriesOnPool_SeededRunRepeats) {
    // Act
    const auto first  = noisy_counts(ProcType::GPU, 20260929);
    const auto second = noisy_counts(ProcType::GPU, 20260929);

    // Assert
    EXPECT_TRUE(first == second);
}

// The concurrent trajectory path must sample the same distribution as the CPU.
// The pure-state tests above run one trajectory, so they never exercise the
// pool with more than one caller.
TEST(TnGpuSvd, NoisyTrajectoriesOnPool_MatchCpuDistribution) {
    // Act
    const auto cpu = noisy_counts(ProcType::CPU, 20260929);
    const auto gpu = noisy_counts(ProcType::GPU, 20260929);

    // Assert
    double tvd = 0.0;
    std::map<std::string, std::uint64_t> keys = cpu;
    keys.insert(gpu.begin(), gpu.end());
    for (const auto& [key, unused] : keys) {
        const auto a = cpu.find(key);
        const auto b = gpu.find(key);
        const double pa = (a == cpu.end()) ? 0.0 : static_cast<double>(a->second);
        const double pb = (b == gpu.end()) ? 0.0 : static_cast<double>(b->second);
        tvd += std::abs(pa - pb);
    }
    tvd /= 2.0 * 256.0;
    // Same seed, amplitudes equal to ~1e-15 (F): the draws coincide
    // practice, so anything beyond a stray flipped shot is a real divergence.
    EXPECT_NEAR(tvd, 0.0, 0.02);
}
