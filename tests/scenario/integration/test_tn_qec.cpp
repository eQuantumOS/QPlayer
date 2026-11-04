// test_tn_qec.cpp — Surface-code syndrome extraction on TensorNetwork
// 1.1, 1.3.
//
// This is the circuit family Phase 10-A exists for. A surface-code round
// measures every ancilla and resets it for reuse, so it needs all three of
// (mid-circuit collapse), (Reset) and (conditionals'
// infrastructure) at once — before them not one round would run on TN.
//
// Two claims are under test, and they are different in kind:
//   1. At d=3 (17 qubits) TN reproduces FullState CPU exactly, so the machinery
//      is right where a reference exists.
//   2. At d=5 (49 qubits) TN runs at all, which FullState cannot — its host
//      budget tops out around 34 qubits. There is no reference there, so the
//      assertions are the code's own structure: Z-stabilizers of |0_L> are
//      deterministically 0, and the logical parity is 0 on every shot.
//
// The measured chi/truncation table is asserted rather than
// merely printed: chi is bounded and the truncation error is EXACTLY zero at
// every distance tried, because a surface-code state is a stabilizer state whose
// Schmidt rank across a chain cut is bounded by the code. That is what makes
// this family a good fit for MPS despite its 2D interaction graph, and it is a
// property a future change could silently lose.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cstdint>
#include <omp.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::ClassicalBitIndex;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::SimType;
using qplayer::TensorNetworkBackend;

namespace {

using Counts = std::map<std::string, std::uint64_t>;

// Rotated surface code of odd distance d: d^2 data + (d^2 - 1) ancilla.
//
// Plaquette (pr, pc) over [0, d]^2 covers data (pr-1, pc-1), (pr-1, pc),
// (pr, pc-1), (pr, pc) clipped to the grid; its type is Z when (pr + pc) is even
// and X when odd. Corners are dropped, and an edge keeps only the type that
// belongs to it — top/bottom Z, left/right X — which is what leaves exactly
// d^2 - 1 stabilizers and makes Z_L (left column) commute with every X-stab.
// At d = 3 this reproduces the surface-17 layout of gen_surface17.py.
struct SurfaceCode {
    int d = 3;
    std::vector<std::vector<QubitIndex>> x_stab, z_stab;
    std::vector<QubitIndex>              x_anc, z_anc, z_logical;
    QubitIndex                           num_qubits = 0;

    explicit SurfaceCode(int distance) : d(distance) {
        const auto data_at = [this](int r, int c) {
            return static_cast<QubitIndex>(r * d + c);
        };
        for (int pr = 0; pr <= d; ++pr) {
            for (int pc = 0; pc <= d; ++pc) {
                const bool z_type      = ((pr + pc) % 2 == 0);
                const bool on_row_edge = (pr == 0 || pr == d);
                const bool on_col_edge = (pc == 0 || pc == d);
                if (on_row_edge && on_col_edge) continue;   // corners carry nothing
                if (on_row_edge && !z_type)     continue;   // top/bottom carry Z only
                if (on_col_edge &&  z_type)     continue;   // left/right carry X only
                std::vector<QubitIndex> support;
                for (int dr = -1; dr <= 0; ++dr) {
                    for (int dc = -1; dc <= 0; ++dc) {
                        const int r = pr + dr;
                        const int c = pc + dc;
                        if (r >= 0 && r < d && c >= 0 && c < d) support.push_back(data_at(r, c));
                    }
                }
                if (support.empty()) continue;
                (z_type ? z_stab : x_stab).push_back(std::move(support));
            }
        }
        const int n_data = d * d;
        num_qubits = static_cast<QubitIndex>(n_data + x_stab.size() + z_stab.size());
        for (std::size_t i = 0; i < x_stab.size(); ++i) {
            x_anc.push_back(static_cast<QubitIndex>(n_data + i));
        }
        for (std::size_t i = 0; i < z_stab.size(); ++i) {
            z_anc.push_back(static_cast<QubitIndex>(n_data + x_stab.size() + i));
        }
        for (int r = 0; r < d; ++r) z_logical.push_back(data_at(r, 0));  // Z_L: left column
    }

    std::size_t n_stab() const { return x_stab.size() + z_stab.size(); }

    // |0_L> Z-memory experiment: `rounds` syndrome extractions, then read the
    // data. Extraction is CZ + H and runs one stabilizer at a time — sequential
    // rather than hardware-parallel, which is a simulation simplification that
    // leaves the noiseless gate exact.
    Circuit build(int rounds) const {
        const int n_data = d * d;
        Circuit c(num_qubits,
                  static_cast<ClassicalBitIndex>(rounds * n_stab() + n_data));
        for (int r = 0; r < rounds; ++r) {
            const auto base = static_cast<ClassicalBitIndex>(r * n_stab());
            for (std::size_t j = 0; j < x_stab.size(); ++j) {
                const QubitIndex a = x_anc[j];
                c.h(a);
                for (QubitIndex dq : x_stab[j]) { c.h(dq); c.cz(a, dq); c.h(dq); }
                c.h(a);
                c.measure(a, base + static_cast<ClassicalBitIndex>(j));
                c.reset(a);                       // reused next round — needs
            }
            for (std::size_t j = 0; j < z_stab.size(); ++j) {
                const QubitIndex a = z_anc[j];
                c.h(a);
                for (QubitIndex dq : z_stab[j]) c.cz(dq, a);
                c.h(a);
                c.measure(a, base + static_cast<ClassicalBitIndex>(x_stab.size() + j));
                c.reset(a);
            }
            c.barrier();                          // round boundary; no fusion across
        }
        for (int i = 0; i < n_data; ++i) {
            c.measure(static_cast<QubitIndex>(i),
                      static_cast<ClassicalBitIndex>(rounds * n_stab() + i));
        }
        return c;
    }
};

// counts() keys are MSB-first over cubits, so cubit k sits at size-1-k.
int bit_at(const std::string& key, ClassicalBitIndex cubit) {
    return key[key.size() - 1u - static_cast<std::size_t>(cubit)] == '1' ? 1 : 0;
}

// Fraction of shots whose final Z_L parity is 1 — the logical error rate of a
// |0_L> memory experiment, read straight off the data register. No decoder: the
// decoder would be identical post-processing on both backends, so it cannot
// distinguish them, and this quantity can.
double logical_error_rate(const SurfaceCode& sc, int rounds, const Counts& counts,
                          std::uint32_t shots) {
    const auto dbase = static_cast<ClassicalBitIndex>(rounds * sc.n_stab());
    std::uint64_t wrong = 0;
    for (const auto& kv : counts) {
        int parity = 0;
        for (QubitIndex q : sc.z_logical) {
            parity ^= bit_at(kv.first, dbase + static_cast<ClassicalBitIndex>(q));
        }
        if (parity != 0) wrong += kv.second;
    }
    return static_cast<double>(wrong) / shots;
}

// Total Z-syndrome weight over the run: |0_L> is a +1 eigenstate of every
// Z-stabilizer, so this is 0 without noise and grows with it.
std::uint64_t z_syndrome_weight(const SurfaceCode& sc, int rounds, const Counts& counts) {
    std::uint64_t total = 0;
    for (const auto& kv : counts) {
        for (int r = 0; r < rounds; ++r) {
            for (std::size_t j = 0; j < sc.z_stab.size(); ++j) {
                const auto cb = static_cast<ClassicalBitIndex>(
                    r * sc.n_stab() + sc.x_stab.size() + j);
                if (bit_at(kv.first, cb) != 0) total += kv.second;
            }
        }
    }
    return total;
}

double tvd(const Counts& a, const Counts& b, std::uint32_t shots) {
    double d = 0.0;
    for (const auto& kv : a) {
        const auto it = b.find(kv.first);
        const double pb = (it == b.end()) ? 0.0 : static_cast<double>(it->second) / shots;
        d += std::abs(static_cast<double>(kv.second) / shots - pb);
    }
    for (const auto& kv : b) {
        if (a.find(kv.first) == a.end()) d += static_cast<double>(kv.second) / shots;
    }
    return 0.5 * d;
}

// Runs on a raw TN backend so the chi and truncation diagnostics are reachable.
struct TnRun {
    Counts        counts;
    std::uint32_t max_bond   = 0;
    double        worst_step = 0.0;
    double        drift      = 0.0;
};

TnRun run_tn(const Circuit& circ, std::uint32_t shots, std::uint64_t seed,
             const NoiseModel* nm) {
    TensorNetworkBackend backend;
    backend.set_seed(seed);
    backend.prepare(circ);
    qplayer::Result r = backend.execute(circ, shots, nm);
    TnRun out;
    out.counts     = r.counts();
    out.max_bond   = backend.last_trajectory_bond_dimension();
    out.drift      = backend.last_trajectory_norm_drift();
    for (double e : r.truncation_errors()) out.worst_step = std::max(out.worst_step, e);
    return out;
}

Counts run_full_state(const Circuit& circ, std::uint32_t shots, std::uint64_t seed,
                      const NoiseModel* nm) {
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(seed);
    if (nm != nullptr) qr.set_noise_model(*nm);
    return qr.run(circ, shots).counts();
}

constexpr std::uint64_t kSeed = 20260826;

}  // namespace

// d=3, no noise. Exact equality is the right assertion here rather than a
// tolerance: with no channels the only random draws are measurements, and both
// backends consume one uniform per measurement with the same comparison
// so the shots agree shot by shot.
TEST(TnQec, SurfaceD3_Noiseless_MatchesFullStateExactly) {
    // Arrange
    const SurfaceCode sc(3);
    const Circuit circ = sc.build(2);
    const std::uint32_t shots = 256;

    // Act
    const TnRun  tn = run_tn(circ, shots, kSeed, nullptr);
    const Counts fs = run_full_state(circ, shots, kSeed, nullptr);

    // Assert
    EXPECT_EQ(sc.num_qubits, QubitIndex{17});
    EXPECT_TRUE(tn.counts == fs);
    // The noiseless gate: |0_L> is a +1 eigenstate of every Z-stabilizer, and
    // its logical parity never flips.
    EXPECT_EQ(z_syndrome_weight(sc, 2, tn.counts), std::uint64_t{0});
    EXPECT_NEAR(logical_error_rate(sc, 2, tn.counts, shots), 0.0, 1e-12);
}

// d=3 under noise — the completion condition. The tolerance is real here
// even though the runs came out identical in practice: the branch weights come
// from a chain contraction on one side and a statevector sum on the other, so
// agreeing to the last bit is not something the implementation guarantees.
TEST(TnQec, SurfaceD3_UnderNoise_LogicalErrorRateMatchesFullState) {
    // Arrange
    const SurfaceCode sc(3);
    const Circuit circ = sc.build(2);
    const std::uint32_t shots = 256;
    NoiseModel nm;
    nm.add_depolarizing("h",  0.005);
    nm.add_depolarizing("cz", 0.010);

    // Act
    const TnRun  tn = run_tn(circ, shots, kSeed, &nm);
    const Counts fs = run_full_state(circ, shots, kSeed, &nm);

    // Assert: the named completion condition, plus the whole distribution.
    const double p_tn = logical_error_rate(sc, 2, tn.counts, shots);
    const double p_fs = logical_error_rate(sc, 2, fs, shots);
    EXPECT_NEAR(p_tn, p_fs, 0.01);
    EXPECT_NEAR(tvd(tn.counts, fs, shots), 0.0, 0.01);
    // And the noise is actually reaching the syndrome — a run that silently
    // dropped its channels would pass the two comparisons above by matching a
    // FullState that dropped them too.
    EXPECT_TRUE(z_syndrome_weight(sc, 2, tn.counts) > 0);
    EXPECT_TRUE(p_tn > 0.0);
}

// 49 qubits is past what a statevector can hold on this host, which is the point
// of running this family on TN at all.
//
// Ask the FullState register, not the TN one: max_qubits() reports where each
// backend's own bound comes from, and the two bounds are different kinds of
// thing — FullState's is the host memory budget, TN's is the structural 128
// Asking TN here would be asking the wrong register.
TEST(TnQec, SurfaceD5_IsBeyondFullStateReach) {
    // Arrange
    const SurfaceCode sc(5);
    QRegister fs(SimType::FullState,     ProcType::CPU);
    QRegister tn(SimType::TensorNetwork, ProcType::CPU);

    // Act + Assert
    EXPECT_EQ(sc.num_qubits, QubitIndex{49});
    EXPECT_TRUE(fs.max_qubits() < sc.num_qubits);
    // And TN says it accepts this width — the two registers must not give the
    // same answer, or the width question has no backend-specific meaning.
    EXPECT_TRUE(tn.max_qubits() >= sc.num_qubits);
}

// d=5 with no reference to compare against: the assertions are the code's own
// structure instead. A backend that mishandled the per-round reset would break
// the Z-stabilizer determinism immediately, because round 2 would measure an
// ancilla that still carried round 1's outcome.
TEST(TnQec, SurfaceD5_NoiselessGateHoldsAt49Qubits) {
    // Arrange
    const SurfaceCode sc(5);
    const Circuit circ = sc.build(2);
    const std::uint32_t shots = 32;

    // Act
    const TnRun tn = run_tn(circ, shots, kSeed, nullptr);

    // Assert
    EXPECT_EQ(sc.n_stab(), std::size_t{24});
    EXPECT_EQ(z_syndrome_weight(sc, 2, tn.counts), std::uint64_t{0});
    EXPECT_NEAR(logical_error_rate(sc, 2, tn.counts, shots), 0.0, 1e-12);
}

// The same width under noise: the trajectories must run and the syndrome must
// light up. This is the configuration verify-sc17-willow cannot reach on
// FullState GPU at all, because that backend has no noisy Reset.
TEST(TnQec, SurfaceD5_UnderNoise_ProducesSyndromes) {
    // Arrange
    const SurfaceCode sc(5);
    const Circuit circ = sc.build(2);
    const std::uint32_t shots = 32;
    NoiseModel nm;
    nm.add_depolarizing("h",  0.005);
    nm.add_depolarizing("cz", 0.010);

    // Act
    const TnRun tn = run_tn(circ, shots, kSeed, &nm);

    // Assert
    EXPECT_TRUE(z_syndrome_weight(sc, 2, tn.counts) > 0);
    EXPECT_TRUE(logical_error_rate(sc, 2, tn.counts, shots) > 0.0);
}

// The chi/truncation table, asserted rather than printed.
//
// Measured on the reference host: the PEAK chi is 8 at d=3 and 32
// d=5, unchanged by round count (1..3) and by noise, and the truncation error is
// EXACTLY zero at every point. The numbers in are half these,
// because they are the bond at the END of the run -- a SWAP round trip and
// syndrome extraction both raise a bond and lower it again, so the two
// definitions differ by 2x and last_trajectory_bond_dimension() now reports the
// peak, which is the one that answers "how close did we get to the cap". Zero is not a tolerance being met — nothing is ever discarded,
// because a surface-code state is a stabilizer state whose Schmidt rank across a
// chain cut is bounded by the code rather than by the circuit depth. Losing that
// would show up here as a nonzero error long before it showed up as a wrong
// answer, which is why the bound is asserted tightly.
TEST(TnQec, BondDimensionIsBoundedAndTruncationIsExact) {
    // Arrange
    const SurfaceCode d3(3);
    const SurfaceCode d5(5);
    NoiseModel nm;
    nm.add_depolarizing("h",  0.005);
    nm.add_depolarizing("cz", 0.010);

    // Act
    const TnRun a = run_tn(d3.build(3), 32, kSeed, nullptr);
    const TnRun b = run_tn(d3.build(3), 32, kSeed, &nm);
    const TnRun c = run_tn(d5.build(2), 32, kSeed, nullptr);

    // Assert: measured 8 / 8 / 32 (peak) under the identity layout (the default,
    //); 4 / 4 / 16 with QPLAYER_TN_LAYOUT=minla. The bounds are loose enough to survive
    // a different site layout and tight enough to fail an exponential blow-up --
    // and they were already written wide enough that correcting the read from
    // end-of-run to peak did not move them.
    EXPECT_TRUE(a.max_bond <= 8);
    EXPECT_TRUE(b.max_bond <= 8);
    EXPECT_TRUE(c.max_bond <= 32);
    // Nothing discarded, on any of the three.
    EXPECT_NEAR(a.worst_step, 0.0, 1e-15);
    EXPECT_NEAR(b.worst_step, 0.0, 1e-15);
    EXPECT_NEAR(c.worst_step, 0.0, 1e-15);
    // And with no truncation the norm cannot drift beyond rounding.
    EXPECT_NEAR(a.drift, 0.0, 1e-9);
    EXPECT_NEAR(b.drift, 0.0, 1e-9);
    EXPECT_NEAR(c.drift, 0.0, 1e-9);
}

// ── Result-level trajectory diagnostics ───────────────────────────

TEST(TnQec, TrajectoryDiagnostics_AreRecordedOncePerShot) {
    // Arrange
    const SurfaceCode d3(3);
    const Circuit circ = d3.build(3);
    const std::uint32_t shots = 8;
    TensorNetworkBackend backend;
    backend.set_seed(kSeed);
    backend.prepare(circ);

    // Act
    const qplayer::Result r = backend.execute(circ, shots, nullptr);

    // Assert: one entry per shot in each vector, and the pair is reported
    // together so their lengths cannot drift apart.
    EXPECT_TRUE(r.has_trajectory_diagnostics());
    EXPECT_EQ(r.bond_dimensions().size(), static_cast<std::size_t>(shots));
    EXPECT_EQ(r.norm_drifts().size(), static_cast<std::size_t>(shots));
    EXPECT_EQ(r.truncation_totals().size(), static_cast<std::size_t>(shots));
}

TEST(TnQec, TrajectoryDiagnostics_ReportThePeakBondNotTheFinalOne) {
    // Arrange: a Bell pair lifts the bond to 2; measuring q0 collapses the state,
    // the reset of a used qubit routes the run to the per-shot path that records
    // diagnostics, and the second cx re-splits the bond at rank 1 (a projector
    // alone leaves the stored dimension at 2 until the next two-site SVD).
    // Peak 2, end-of-run 1 -- by construction, so no site layout can move either
    // number. (This test used to rely on d=3 surface code reading 8 against an
    // end-of-run 4 under the identity layout, which the MinLA layout -- opt-in
    // since -- turns into a peak of 4.)
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).reset(0).cx(0, 1).measure(1, 1);
    TensorNetworkBackend backend;
    backend.set_seed(kSeed);
    backend.prepare(circ);

    // Act
    const qplayer::Result r = backend.execute(circ, 8, nullptr);
    std::uint32_t worst = 0;
    for (std::uint32_t b : r.bond_dimensions()) worst = std::max(worst, b);

    // Assert: 2 is the peak; the end-of-run read this corrects would give 1.
    EXPECT_TRUE(r.has_trajectory_diagnostics());
    EXPECT_EQ(worst, static_cast<std::uint32_t>(2));
}

TEST(TnQec, TrajectoryDiagnostics_NoiselessRunDoesNotDriftTheNorm) {
    // Arrange
    const SurfaceCode d3(3);
    const Circuit circ = d3.build(3);
    TensorNetworkBackend backend;
    backend.set_seed(kSeed);
    backend.prepare(circ);

    // Act
    const qplayer::Result r = backend.execute(circ, 8, nullptr);
    double worst = 0.0;
    for (double d : r.norm_drifts()) worst = std::max(worst, std::abs(d));

    // Assert: nothing was discarded, so the local rescale cannot have moved the
    // global norm beyond rounding.
    EXPECT_NEAR(worst, 0.0, 1e-9);
}

TEST(TnQec, TrajectoryDiagnostics_AreAbsentOnFullState) {
    // Arrange
    const SurfaceCode d3(3);
    const Circuit circ = d3.build(3);
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(kSeed);

    // Act
    const qplayer::Result r = qr.run(circ, 8);

    // Assert: there is no per-shot MPS chain to report on, so the vectors stay
    // empty rather than being filled with a placeholder.
    EXPECT_FALSE(r.has_trajectory_diagnostics());
    EXPECT_TRUE(r.bond_dimensions().empty());
    EXPECT_TRUE(r.norm_drifts().empty());
    EXPECT_TRUE(r.truncation_totals().empty());
}

// ── Per-shot cumulative truncation weight ─────────────────────────

TEST(TnQec, TruncationTotals_AreZeroWhenNothingIsDiscarded) {
    // Arrange: surface code needs chi 8 here and the cap is the 256 default, so
    // no SVD throws anything away.
    const SurfaceCode d3(3);
    const Circuit circ = d3.build(3);
    TensorNetworkBackend backend;
    backend.set_seed(kSeed);
    backend.prepare(circ);

    // Act
    const qplayer::Result r = backend.execute(circ, 8, nullptr);

    // Assert
    EXPECT_TRUE(r.has_trajectory_diagnostics());
    for (double t : r.truncation_totals()) EXPECT_NEAR(t, 0.0, 1e-15);
}

TEST(TnQec, TruncationTotals_DominateTheL2FormWhenTruncationHappens) {
    // Arrange: surface code d=3 peaks at chi 8, so a cap of 6 truncates on every
    // shot -- the only regime where the two norms can be told apart. The cap is 6
    // and not lower because below that the divergence guard fires and
    // the run throws instead of returning a record to inspect.
    const SurfaceCode d3(3);
    const Circuit circ = d3.build(3);
    TensorNetworkBackend backend;
    backend.set_seed(kSeed);
    backend.set_mps_truncation(6, 1e-12);
    backend.prepare(circ);

    // Act
    const qplayer::Result r = backend.execute(circ, 4, nullptr);

    // Assert: something was discarded, and the largest per-shot L1 sum is at
    // least the largest single step of the worst shot. That per-step history is
    // the only one Result carries, and max(step) <= L2 <= L1 holds for every
    // shot, so this is the comparison the two accessors together can support.
    const std::vector<double>& totals = r.truncation_totals();
    EXPECT_EQ(totals.size(), static_cast<std::size_t>(4));
    double worst_step = 0.0;
    for (double e : r.truncation_errors()) worst_step = std::max(worst_step, e);
    EXPECT_TRUE(worst_step > 0.0);
    double worst_total = 0.0;
    for (double t : totals) {
        EXPECT_TRUE(t >= 0.0);
        worst_total = std::max(worst_total, t);
    }
    // The worst trajectory by L2 is one of these shots, so its single largest
    // step cannot exceed the largest L1 sum.
    EXPECT_TRUE(worst_total >= worst_step);
}

TEST(TnQec, TruncationTotals_AreThreadCountIndependent) {
    // Arrange: the vector is indexed by shot and each shot derives its own RNG
    // from the shot index (D), so the record must not depend on how the
    // trajectories were spread over threads. Cap 6 on d=3 keeps truncation on --
    // with nothing discarded every entry would be 0 and the test would pass for
    // the wrong reason.
    const SurfaceCode d3(3);
    const Circuit circ = d3.build(3);
    NoiseModel nm;
    nm.add_depolarizing("cz", 0.02);

    auto run_with = [&](int threads) {
        omp_set_num_threads(threads);
        TensorNetworkBackend backend;
        backend.set_seed(kSeed);
        backend.set_mps_truncation(6, 1e-12);
        backend.prepare(circ);
        return backend.execute(circ, 8, &nm).truncation_totals();
    };

    // Act
    const int saved = omp_get_max_threads();
    const std::vector<double> one  = run_with(1);
    const std::vector<double> many = run_with(4);
    omp_set_num_threads(saved);

    // Assert: bit-identical, not merely close. EXPECT_EQ refuses doubles by
    // design (1.3), and EXPECT_NEAR would accept exactly what
    // this test exists to reject, so the comparison is spelled out.
    EXPECT_EQ(one.size(), many.size());
    bool identical = one.size() == many.size();
    for (std::size_t i = 0; identical && i < one.size(); ++i) {
        identical = (one[i] == many[i]);
    }
    EXPECT_TRUE(identical);
}
