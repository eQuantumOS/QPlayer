// test_tn_noise_parity.cpp — TN Monte Carlo trajectory vs FullState CPU
//
// The claim under test is the one rests on: the SAME NoiseModel object,
// handed to two backends, produces the same distribution. Anything weaker (TN
// runs without crashing, or TN's numbers look plausible) would not support it.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <map>
#include <string>

#include "../../../include/qregister.hpp"
#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/backend/tensor_network/tn_mpo.hpp"
#include "../../../include/error.hpp"

#ifdef _OPENMP
#include <omp.h>
#endif

using qplayer::Circuit;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::SimType;
using qplayer::SimulatorError;

namespace {

// Total variation distance between two count maps, as probabilities.
double tvd(const std::map<std::string, std::uint64_t>& a,
           const std::map<std::string, std::uint64_t>& b,
           std::uint64_t shots) {
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

std::map<std::string, std::uint64_t> run_with(SimType sim, const Circuit& circ,
                                              const NoiseModel& nm, std::uint32_t shots,
                                              std::uint64_t seed) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    qr.set_noise_model(nm);
    return qr.run(circ, shots).counts();
}

}  // namespace

TEST(TnNoiseParity, DepolarizingBell_MatchesFullState) {
    // Arrange: the same NoiseModel instance for both backends — the point is that
    // no TN-specific channel construction exists.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.04);
    nm.add_depolarizing("cx", 0.06);
    const std::uint32_t shots = 8000;

    // Act
    const auto fs = run_with(SimType::FullState,     circ, nm, shots, 4242);
    const auto tn = run_with(SimType::TensorNetwork, circ, nm, shots, 4242);

    // Assert: TVD over 4 outcomes at 8000 shots has a sampling floor near 0.01;
    // 0.02 is the T10 completion bound and still fails a broken channel by far.
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.02);
}

TEST(TnNoiseParity, AmplitudeDamping_MatchesFullState) {
    // Arrange: a non-unital channel, where a lost norm-reduction would bias the
    // branch weights rather than just reshuffling them.
    Circuit circ(2, 2);
    circ.x(0).x(1).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_amplitude_damping("x", 0.25);
    const std::uint32_t shots = 8000;

    // Act
    const auto fs = run_with(SimType::FullState,     circ, nm, shots, 77);
    const auto tn = run_with(SimType::TensorNetwork, circ, nm, shots, 77);

    // Assert
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.02);
}

TEST(TnNoiseParity, PhaseFlipOnGhz_MatchesFullState) {
    // Arrange: 4-qubit GHZ with a phase channel — the distribution stays on the
    // two GHZ outcomes, so a channel applied to the wrong site shows up as weight
    // leaking into other bitstrings.
    Circuit circ(4, 4);
    circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3);
    for (QubitIndex q = 0; q < 4; ++q) circ.measure(q, q);
    NoiseModel nm;
    nm.add_phase_flip("cx", 0.08);
    nm.add_bit_flip("h", 0.05);
    const std::uint32_t shots = 8000;

    // Act
    const auto fs = run_with(SimType::FullState,     circ, nm, shots, 999);
    const auto tn = run_with(SimType::TensorNetwork, circ, nm, shots, 999);

    // Assert
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.02);
}

TEST(TnNoiseParity, SeedIsReproducible) {
    // Arrange: trajectory seeds derive from the master seed and the trajectory
    // index, so a repeat run must be identical — not merely close.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.1);

    // Act
    const auto a = run_with(SimType::TensorNetwork, circ, nm, 2000, 31337);
    const auto b = run_with(SimType::TensorNetwork, circ, nm, 2000, 31337);

    // Assert
    EXPECT_TRUE(a == b);
}

TEST(TnNoiseParity, TruncationErrorsAreReportedUnderNoise) {
    // Arrange: a 2Q channel forces an SVD per application, so the ledger must be
    // non-empty — this is the contract extension introduces
    // (entries now cover 2Q CHANNELS as well as 2Q gates).
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.05);
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_seed(5);
    qr.set_noise_model(nm);

    // Act
    const auto& result = qr.run(circ, 64);

    // Assert: 1Q Kraus operators never truncate, so the entries present come from
    // the 2Q gates themselves; the contract is that the ledger stays available.
    EXPECT_TRUE(result.has_truncation_errors());
    for (double e : result.truncation_errors()) EXPECT_TRUE(e >= 0.0 && e <= 1.0);
}

TEST(TnNoiseParity, ResetUnderNoise_RunsAndMatchesFullState) {
    // Arrange: Reset arrived on the noisy path. It used to be
    // refused here, and before that the noiseless path reported "[FullState]" for
    // it because it borrowed a shared helper. Both are gone; what matters now is
    // that the noise route actually carries it, since every QEC round resets its
    // ancilla and FullState GPU still cannot.
    Circuit circ(1, 1);
    circ.h(0).reset(0).measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.05);

    // Act
    const auto tn = run_with(SimType::TensorNetwork, circ, nm, 2000, 909);
    const auto fs = run_with(SimType::FullState,     circ, nm, 2000, 909);

    // Assert: same uniform stream in the same order, so this is bit-identical
    // rather than merely close.
    EXPECT_TRUE(tn == fs);
}

TEST(TnNoiseParity, ClassicalControlUnderNoise_BranchesPerTrajectory) {
    // Arrange: IfOp on the noisy path arrived. The branch
    // must read THIS trajectory's classical bits, so that the Kraus draws and
    // the conditional share one shot context.
    // Noise sits on `h` only, so the conditional's own body is unaffected and
    // FullState stays a valid reference here — unlike a noisy body, where it
    // drops the channel entirely.
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    qplayer::GateOp body;
    body.type   = qplayer::GateType::X;
    body.name   = "x";
    body.qubits = {1};
    circ.if_then(0, 1, body);
    circ.measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.05);

    // Act
    const auto tn = run_with(SimType::TensorNetwork, circ, nm, 4000, 5150);
    const auto fs = run_with(SimType::FullState,     circ, nm, 4000, 5150);

    // Assert: c1 tracks c0 on every trajectory, and the two backends draw the
    // same uniforms in the same order, so this is bit-identical.
    EXPECT_TRUE(tn == fs);
    EXPECT_TRUE(tn.find("01") == tn.end());
    EXPECT_TRUE(tn.find("10") == tn.end());
}

// ── Readout error and per-shot expectation accumulation ──────────────

#ifndef QPLAYER_ACCEPTANCE_DATA_DIR
#define QPLAYER_ACCEPTANCE_DATA_DIR "tests/data"
#endif

TEST(TnNoiseParity, ReadoutError_MatchesFullState) {
    // Arrange: no gate noise at all, so every difference between the two backends
    // has to come from the readout confusion path. x on both qubits
    // makes the true outcome |11>, and asymmetric probabilities mean a swapped
    // p01/p10 shows up as a different distribution rather than a mirror image.
    Circuit circ(2, 2);
    circ.x(0).x(1).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_readout_error(0, /*p_meas_0_given_1=*/0.10, /*p_meas_1_given_0=*/0.03);
    nm.add_readout_error(1, 0.20, 0.05);
    const std::uint32_t shots = 16000;

    // Act
    const auto fs = run_with(SimType::FullState,     circ, nm, shots, 606);
    const auto tn = run_with(SimType::TensorNetwork, circ, nm, shots, 606);

    // Assert
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.02);
}

TEST(TnNoiseParity, ReadoutError_LeavesTheQuantumStateAlone) {
    // Arrange: says the collapse uses the TRUE outcome and only the
    // reported classical bit is confused. With p(report 0 | true 1) = 1 the
    // reported bits all invert while the state stays |1>.
    //
    // The distinguishing observable has to read the STATE, not the bits: applying
    // the confusion to the state would be indistinguishable in counts alone. The
    // registered <Z> is accumulated from each trajectory's chain, so it exposes
    // the difference — counts say "0", <Z> says the qubit is still |1>.
    Circuit circ(1, 1);
    circ.x(0).measure(0, 0).add_expectation("Z");
    NoiseModel nm;
    nm.add_readout_error(0, /*p_meas_0_given_1=*/1.0, /*p_meas_1_given_0=*/0.0);
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_seed(11);
    qr.set_noise_model(nm);

    // Act
    const auto& result = qr.run(circ, 256);

    // Assert: every shot reports 0 despite the state being |1>.
    EXPECT_EQ(result.counts().size(), static_cast<std::size_t>(1));
    EXPECT_EQ(result.counts().begin()->first, std::string("0"));
    EXPECT_NEAR(result.expectation("Z"), -1.0, 1e-12);
}

TEST(TnNoiseParity, StateAccessAfterANoisyRun_Throws) {
    // Arrange: nshots trajectories each end in their own chain, so there is no
    // single state to hand back. Returning the PREPARED |0...0> would be a
    // plausible-looking wrong answer — the exact failure mode this guard exists
    // for (found by the readout test above, whose first version read +1 for a
    // qubit the circuit had flipped to |1>).
    Circuit circ(2, 2);
    circ.x(0).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("x", 0.05);
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_seed(3);
    qr.set_noise_model(nm);
    qr.run(circ, 32);

    // Act + Assert
    EXPECT_THROW(qr.get_statevector(), SimulatorError);
    EXPECT_THROW(qr.expectation_value("ZZ"), SimulatorError);
    EXPECT_THROW(qr.amplitude(0), SimulatorError);
    EXPECT_THROW(qr.reduced_density_matrix({0}), SimulatorError);
}

TEST(TnNoiseParity, ExpectationAccumulatesOverTrajectories) {
    // Arrange: a registered target on a noisy circuit. Before the dict was
    // left empty on this path and Result::expectation() threw;
    // the backend now accumulates the nshot average itself.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).add_expectation("ZZ");
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.08);
    const std::uint32_t shots = 4000;

    QRegister tn(SimType::TensorNetwork, ProcType::CPU);
    tn.set_seed(2024);
    tn.set_noise_model(nm);
    QRegister fs(SimType::FullState, ProcType::CPU);
    fs.set_seed(2024);
    fs.set_noise_model(nm);

    // Act
    const double tn_zz = tn.run(circ, shots).expectation("ZZ");
    const double fs_zz = fs.run(circ, shots).expectation("ZZ");

    // Assert: both are nshot averages of the same channel, so they agree to
    // sampling noise; 0.02 is the T10 completion bound.
    EXPECT_NEAR(tn_zz, fs_zz, 0.02);
}

TEST(TnNoiseParity, ExpectationIsIndependentOfThreadCount) {
    // Arrange: requires the nshot average to be summed
    // trajectory order so the value does not depend on the team size. Running the
    // same seed twice cannot prove that alone, so this checks the stronger
    // property the ordering buys: bit-identical repeats.
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).add_expectation("ZZZ").add_expectation("XXX");
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.05);
    nm.add_amplitude_damping("h", 0.02);

    QRegister a(SimType::TensorNetwork, ProcType::CPU);
    a.set_seed(4321); a.set_noise_model(nm);
    QRegister b(SimType::TensorNetwork, ProcType::CPU);
    b.set_seed(4321); b.set_noise_model(nm);

    // Act
    const auto& ra = a.run(circ, 512);
    const double a_zzz = ra.expectation("ZZZ");
    const double a_xxx = ra.expectation("XXX");
    const auto& rb = b.run(circ, 512);

    // Assert: tolerance 0 on purpose. This is a determinism claim, not a
    // numerical one — the ordered sum must reproduce the same bits, so any
    // non-zero tolerance would weaken exactly what is being tested.
    EXPECT_NEAR(a_zzz, rb.expectation("ZZZ"), 0.0);
    EXPECT_NEAR(a_xxx, rb.expectation("XXX"), 0.0);
}

TEST(TnNoiseParity, ExpectationUnderNonIdentitySiteLayout_IsExact) {
    // Arrange: the trajectory path indexes Pauli factors by qubit but applies them
    // by site, so a missing permutation puts Z on the wrong qubit. Two properties
    // make that observable here and nowhere else in this file:
    //
    // 1. the 0-3-1-2 coupling path, which lays out non-identically, and
    //   2. PARTIAL entanglement with distinct pre-rotations, so each qubit has a
    //      distinct non-zero <Z>. A GHZ-like state leaves every single-qubit RDM
    //      maximally mixed, every single-qubit <P> zero, and the whole assertion
    //      set invariant under ANY permutation — which is how a mutation that
    //      dropped the mapping survived the first version of these tests.
    //
    // The noise model attaches to a gate the circuit never uses, so no channel
    // fires: every trajectory reproduces the exact state and the comparison is
    // sampling-free (tolerance 1e-12, not a statistical bound). That isolates the
    // permutation from Monte Carlo noise.
    Circuit circ(4, 4);
    circ.ry(0.31, 0).ry(0.73, 1).ry(1.14, 2).ry(0.52, 3);
    circ.cx(0, 3).cx(3, 1).cx(1, 2);
    circ.t(0).rx(0.41, 2);
    for (const char* pauli : {"ZIII", "IZII", "IIZI", "IIIZ", "XYZI"}) {
        circ.add_expectation(pauli);
    }
    NoiseModel nm;
    nm.add_depolarizing("h", 0.10);   // no h in the circuit -> no channel fires

    QRegister tn(SimType::TensorNetwork, ProcType::CPU);
    tn.set_seed(17);
    tn.set_noise_model(nm);
    QRegister fs(SimType::FullState, ProcType::CPU);
    fs.set_seed(17);

    // Act
    const auto& tn_result = tn.run(circ, 4);
    fs.run(circ, 1);

    // Assert
    for (const char* pauli : {"ZIII", "IZII", "IIZI", "IIIZ", "XYZI"}) {
        EXPECT_NEAR(tn_result.expectation(pauli), fs.expectation_value(pauli), 1e-12);
    }
}

TEST(TnNoiseParity, ExpectationWorksBeyondTheDenseExportCap) {
    // Arrange: 40 qubits with no measurement. <P> is a contraction, so it reaches
    // past FullState entirely — this was the only wide-circuit output the noisy
    // path had before made counts reachable too.
    const QubitIndex n = 40;
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);
    circ.add_expectation(std::string(n, 'Z'));
    NoiseModel nm;
    nm.add_phase_flip("cx", 0.02);   // phase noise leaves <Z...Z> = 1 on GHZ
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_seed(8);
    qr.set_noise_model(nm);

    // Act
    const double zz = qr.run(circ, 16).expectation(std::string(n, 'Z'));

    // Assert: Z-basis phase flips commute with Z...Z, so the ideal value survives.
    EXPECT_NEAR(zz, 1.0, 1e-9);
}

TEST(TnNoiseParity, DeviceJsonSynthesis_MatchesFullState) {
    // Arrange: the completion condition. load_device() synthesises
    // relaxation + depolarizing + readout from the calibration file,
    // so this is the first test where all three noise families act at once and
    // the readout path is exercised through the device route rather than by hand.
    const std::string device =
        std::string(QPLAYER_ACCEPTANCE_DATA_DIR) + "/device_json/willow_like.json";
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2);
    for (QubitIndex q = 0; q < 3; ++q) circ.measure(q, q);
    const std::uint32_t shots = 8000;

    QRegister fs(SimType::FullState, ProcType::CPU);
    fs.set_seed(31415);
    fs.load_device_model(device);
    QRegister tn(SimType::TensorNetwork, ProcType::CPU);
    tn.set_seed(31415);
    tn.load_device_model(device);

    // Act
    const auto fs_counts = fs.run(circ, shots).counts();
    const auto tn_counts = tn.run(circ, shots).counts();

    // Assert
    EXPECT_NEAR(tvd(fs_counts, tn_counts, shots), 0.0, 0.02);
}

// ── Native sampling on the noisy path (completion condition 3) ─────────
//
// Before every trajectory densified to draw its single shot, which capped
// noisy counts at 25 qubits and cost 2^n per shot. These cover what replaced it.

TEST(TnNoiseParity, NoisyCountsBeyondTheDenseExportCap) {
    // Arrange: 40-qubit GHZ under weak bit-flip noise. The dense vector for one
    // trajectory would be 16 TiB, so the fact that counts come back at all is the
    // result under test. Weak noise keeps the two GHZ branches dominant, which
    // gives the distribution a checkable shape instead of only a shot total.
    const QubitIndex n = 40;
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);
    circ.measure_all();
    NoiseModel nm;
    nm.add_bit_flip("cx", 0.002);
    const std::uint32_t shots = 200;

    // Act
    const auto counts = run_with(SimType::TensorNetwork, circ, nm, shots, 4321);

    // Assert
    std::uint64_t total = 0;
    for (const auto& kv : counts) {
        EXPECT_EQ(kv.first.size(), static_cast<std::size_t>(n));
        total += kv.second;
    }
    EXPECT_EQ(total, static_cast<std::uint64_t>(shots));
    const std::uint64_t clean = counts.count(std::string(n, '0')) +
                                counts.count(std::string(n, '1'));
    EXPECT_EQ(clean, std::uint64_t{2});
}

TEST(TnNoiseParity, NoisyReadoutBeyondTheDenseExportCap) {
    // Arrange: |1...1> at 40 qubits with a readout error on ONE qubit. The true
    // distribution is a point mass, so every deviation in the reported bits is
    // attributable to that qubit — a readout applied to the wrong site (or to the
    // state instead of the report) changes which position varies, not just how
    // often. Gate noise is absent so nothing else can move the bits.
    const QubitIndex n = 40;
    const QubitIndex flipped = 7;
    Circuit circ(n, n);
    for (QubitIndex q = 0; q < n; ++q) circ.x(q);
    circ.measure_all();
    NoiseModel nm;
    nm.add_readout_error(flipped, /*p_meas_0_given_1=*/0.25, /*p_meas_1_given_0=*/0.0);
    const std::uint32_t shots = 2000;

    // Act
    const auto counts = run_with(SimType::TensorNetwork, circ, nm, shots, 99);

    // Assert: exactly two keys — all ones, and all ones with bit `flipped` zeroed.
    // Keys are MSB-first over the classical register, so cubit q sits at n-1-q.
    std::string flipped_key(n, '1');
    flipped_key[static_cast<std::size_t>(n - 1u - flipped)] = '0';
    EXPECT_EQ(counts.size(), std::size_t{2});
    EXPECT_TRUE(counts.count(flipped_key) == 1);
    // 4 sigma at n=2000 for p=0.25 is under 0.04.
    EXPECT_NEAR(static_cast<double>(counts.at(flipped_key)) / shots, 0.25, 0.04);
}

TEST(TnNoiseParity, NativeDrawMatchesFullStateOnTheSameNoiseModel) {
    // Arrange: below the cap the two backends can still be compared directly, so
    // this is where the native draw is checked against a reference distribution
    // rather than against a property. Two things make a dropped site permutation
    // visible: the 0-3-1-2 coupling path, which lays out non-identically,
    // and distinct rotations per qubit, which make the target distribution
    // asymmetric — under a symmetric one any permutation would pass.
    Circuit circ(4, 4);
    circ.ry(0.7, 0).ry(1.9, 1).ry(2.5, 2).ry(1.1, 3);
    circ.cx(0, 3).cx(3, 1).cx(1, 2);
    circ.measure_all();
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.02);
    const std::uint32_t shots = 16000;

    // Act
    const auto fs = run_with(SimType::FullState,     circ, nm, shots, 1234);
    const auto tn = run_with(SimType::TensorNetwork, circ, nm, shots, 1234);

    // Assert
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.02);
}

// ── MPO density-matrix path ──────────────────────
//
// The MPO route carries rho exactly instead of averaging trajectories, so these
// check two different things: that it agrees with the sampled route on the same
// NoiseModel, and that it does NOT inherit the sampling error — the property
// that makes it worth having a second route at all.

namespace {

// Runs on a raw TensorNetwork backend with the MPO route forced on, so the test
// controls the route rather than an environment variable.
std::map<std::string, std::uint64_t> run_mpo(const Circuit& circ, const NoiseModel& nm,
                                             std::uint32_t shots, std::uint64_t seed) {
    qplayer::TensorNetworkBackend backend;
    backend.set_tn_use_mpo(true);
    backend.set_seed(seed);
    backend.prepare(circ);
    return backend.execute(circ, shots, &nm).counts();
}

}  // namespace

// ── MPO sampling is parallel over shots ───────────────────────────

TEST(TnNoiseParity, MpoCounts_ConserveTheShotBudget) {
    // The shot loop draws into per-thread maps and merges them afterwards, so a
    // merge that drops or double-counts a thread is the failure mode that no
    // distribution check would catch (it would still look like a valid
    // distribution). Summing the counts is the invariant that does catch it.
    Circuit circ(3, 3);
    circ.ry(0.7, 0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.05);

    const std::uint32_t shots = 2048;
    const auto counts = run_mpo(circ, nm, shots, 31337);

    std::uint64_t total = 0;
    for (const auto& kv : counts) total += kv.second;
    // A budget claim, so exact equality is the right tolerance.
    EXPECT_EQ(total, static_cast<std::uint64_t>(shots));
}

TEST(TnNoiseParity, MpoCounts_AreIndependentOfThreadCount) {
    // the shot seed is derived from the shot INDEX, not drawn from one
    // sequential stream, so the counts must not depend on how many threads drew
    // them. Running the same seed twice cannot show this -- the team size has to
    // actually change between the two runs.
    Circuit circ(3, 3);
    circ.ry(0.9, 0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.06);
    nm.add_readout_error(0, 0.03, 0.02);

#ifdef _OPENMP
    const int saved = omp_get_max_threads();
    omp_set_num_threads(1);
#endif
    const auto one = run_mpo(circ, nm, 1024, 20260828);
#ifdef _OPENMP
    omp_set_num_threads(8);
#endif
    const auto many = run_mpo(circ, nm, 1024, 20260828);
#ifdef _OPENMP
    omp_set_num_threads(saved);
#endif

    // Determinism claim: bit-identical maps, not a statistical agreement.
    EXPECT_TRUE(one == many);
}

TEST(TnNoiseParity, MpoCounts_SameSeedReproduces) {
    // Guards the parallel merge against order-dependent accumulation: two runs of
    // the same seed must agree exactly even though the threads that produced the
    // per-thread maps may have been scheduled differently.
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    NoiseModel nm;
    nm.add_amplitude_damping("cx", 0.04);

    const auto a = run_mpo(circ, nm, 1024, 777);
    const auto b = run_mpo(circ, nm, 1024, 777);

    EXPECT_TRUE(a == b);
}

TEST(TnNoiseParity, MpoPath_MatchesFullStateDistribution) {
    // Arrange: the same NoiseModel object handed to both engines — the claim is
    // that MPO changes the mechanism, not the physics.
    Circuit circ(3, 3);
    circ.ry(0.7, 0).ry(1.9, 1).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.05);
    nm.add_amplitude_damping("ry", 0.08);
    const std::uint32_t shots = 20000;

    // Act
    const auto fs  = run_with(SimType::FullState, circ, nm, shots, 31);
    const auto mpo = run_mpo(circ, nm, shots, 31);

    // Assert
    EXPECT_NEAR(tvd(fs, mpo, shots), 0.0, 0.02);
}

TEST(TnNoiseParity, MpoPath_MatchesTheMonteCarloRoute) {
    // Arrange: both routes on the same backend type. Any disagreement
    // here is a defect in one of the two, not a modelling difference.
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).measure(0, 0).measure(1, 1).measure(2, 2);
    NoiseModel nm;
    nm.add_phase_flip("cx", 0.05);
    nm.add_bit_flip("h", 0.03);
    const std::uint32_t shots = 20000;

    // Act
    const auto mc  = run_with(SimType::TensorNetwork, circ, nm, shots, 77);
    const auto mpo = run_mpo(circ, nm, shots, 77);

    // Assert
    EXPECT_NEAR(tvd(mc, mpo, shots), 0.0, 0.02);
}

TEST(TnNoiseParity, MpoExpectation_IsExactAndShotIndependent) {
    // Arrange: X then amplitude damping gamma leaves rho = diag(gamma, 1-gamma),
    // so <Z> = 2*gamma - 1 analytically. The MPO route computes Tr(rho Z) — one
    // exact number — which means it must hit that value with NO statistical
    // tolerance, and must not change when nshots does. A trajectory average
    // cannot satisfy either condition, so this is what separates the two routes.
    const double gamma = 0.3;
    Circuit circ(1, 1);
    circ.x(0);
    circ.add_expectation("Z");
    NoiseModel nm;
    nm.add_amplitude_damping("x", gamma);

    qplayer::TensorNetworkBackend a;
    a.set_tn_use_mpo(true);
    a.set_seed(5);
    a.prepare(circ);
    qplayer::TensorNetworkBackend b;
    b.set_tn_use_mpo(true);
    b.set_seed(5);
    b.prepare(circ);

    // Act
    const double one     = a.execute(circ, 1,    &nm).expectation("Z");
    const double thousand = b.execute(circ, 1000, &nm).expectation("Z");

    // Assert
    EXPECT_NEAR(one, 2.0 * gamma - 1.0, 1e-12);
    EXPECT_NEAR(thousand, one, 1e-15);
}

TEST(TnNoiseParity, MpoRoute_IsOffByDefaultAndSelectableBySetter) {
    // Arrange: run() must not change shape when the route does (condition 3).
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.02);

    qplayer::TensorNetworkBackend mc;
    mc.set_seed(3);
    mc.prepare(circ);
    qplayer::TensorNetworkBackend mpo;
    mpo.set_tn_use_mpo(true);
    mpo.set_seed(3);
    mpo.prepare(circ);

    // Act
    const auto counts_mc  = mc.execute(circ, 4000, &nm).counts();
    const auto counts_mpo = mpo.execute(circ, 4000, &nm).counts();

    // Assert: the default is Monte Carlo, so the MPO route only runs when asked.
    // Both produce the same distribution but not the same shots — different
    // mechanisms consume randomness differently.
    EXPECT_NEAR(tvd(counts_mc, counts_mpo, 4000), 0.0, 0.03);
    EXPECT_EQ(mc.last_mpo_bond_dimension(), std::uint32_t{0});
    EXPECT_TRUE(mpo.last_mpo_bond_dimension() > 0);
}

TEST(TnNoiseParity, MpoPath_StrongNoiseBoundsTheBondDimension) {
    // Arrange: 's measurement said the damping-bounds-chi_rho effect is
    // REAL but conditional — it needs a lossy cutoff. So this pins the effect at
    // a cutoff where it exists (1e-3), which is also the honest statement of
    // when the MPO route is worth choosing.
    const auto run_at = [](double p) {
        const QubitIndex n = 8;
        Circuit circ(n, n);
        for (QubitIndex q = 0; q < n; ++q) circ.h(q);
        for (int layer = 0; layer < 10; ++layer) {
            for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < n; q += 2) {
                circ.cx(q, q + 1);
                circ.t(q + 1);
            }
        }
        NoiseModel nm;
        if (p > 0.0) nm.add_depolarizing("cx", p);
        qplayer::TensorNetworkBackend backend;
        backend.set_tn_use_mpo(true);
        backend.set_mpo_truncation(256, 1e-3);
        backend.set_seed(1);
        backend.prepare(circ);
        backend.execute(circ, 1, &nm);
        return backend.last_mpo_bond_dimension();
    };

    // Act
    const std::uint32_t weak   = run_at(0.001);
    const std::uint32_t strong = run_at(0.30);

    // Assert: strong damping kills the long-range correlations the bond has to
    // carry, so it needs strictly less of it.
    EXPECT_TRUE(strong < weak);
}

TEST(TnNoiseParity, StateAccessAfterAnMpoRun_Throws) {
    // Arrange: rho is not a state vector. Handing back the prepared |0...0>
    // would be the silent wrong answer already fixed for trajectories.
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.05);
    qplayer::TensorNetworkBackend backend;
    backend.set_tn_use_mpo(true);
    backend.prepare(circ);

    // Act
    backend.execute(circ, 100, &nm);

    // Assert
    EXPECT_THROW(backend.get_statevector(), SimulatorError);
    EXPECT_THROW(backend.expectation_value("ZZ"), SimulatorError);
}

TEST(TnNoiseParity, MpoTruncationSetter_RejectsUnhonourableValues) {
    qplayer::TensorNetworkBackend backend;
    EXPECT_THROW(backend.set_mpo_truncation(0, 1e-12), qplayer::CircuitError);
    EXPECT_THROW(backend.set_mpo_truncation(qplayer::kMpoMaxBondCeiling + 1, 1e-12),
                 qplayer::CircuitError);
    EXPECT_NO_THROW(backend.set_mpo_truncation(64, 1e-8));
}

TEST(TnNoiseParity, MpoPath_UnderNonIdentitySiteLayout_MatchesFullState) {
    // Arrange: the MPO path indexes channels and reports measurements by qubit
    // but applies them by site, so a missing permutation puts the noise — and
    // the classical bits — on the wrong qubit. Two properties make that visible:
    //
    // 1. the 0-3-1-2 coupling path, which lays out non-identically, and
    //   2. distinct rotations per qubit, so every marginal differs; a symmetric
    //      circuit is invariant under any permutation and lets the bug through.
    //
    // Every other MPO test in this file uses a 0-1-2 chain, whose layout IS the
    // identity — which is how that mutation survived the first version.
    Circuit circ(4, 4);
    circ.ry(0.31, 0).ry(0.73, 1).ry(1.14, 2).ry(0.52, 3);
    circ.cx(0, 3).cx(3, 1).cx(1, 2);
    circ.t(0).rx(0.41, 2);
    circ.measure_all();
    NoiseModel nm;
    nm.add_amplitude_damping("cx", 0.06);
    nm.add_depolarizing("ry", 0.04);
    const std::uint32_t shots = 20000;

    // Act
    const auto fs  = run_with(SimType::FullState, circ, nm, shots, 202);
    const auto mpo = run_mpo(circ, nm, shots, 202);

    // Assert
    EXPECT_NEAR(tvd(fs, mpo, shots), 0.0, 0.02);
}

// ── Three-rung cross-validation ladder ─────────────────
//
// The ladder is FullState -> MC-TN -> MPO-TN. Each rung carries a reference the
// next one does not have: FullState is exact but capped at ~25 qubits, MC-TN
// scales but samples, MPO-TN is exact in the channel but capped by chi_rho. The
// tests below check the two joints of the ladder and then the regime past both
// references, where the only thing standing between a wrong answer and a
// plausible one is the diagnostics.

namespace {

// sqrt(sum of squared step errors) — the budget quantity, recomputed
// from the reported ledger so the test reads what a user would read.
double accumulated_error(const std::vector<double>& steps) {
    double sum_sq = 0.0;
    for (double e : steps) sum_sq += e * e;
    return std::sqrt(sum_sq);
}

// Largest per-qubit marginal gap between two count maps. Keys are MSB-first over
// the classical register, so cubit c sits at width-1-c.
double max_marginal_gap(const std::map<std::string, std::uint64_t>& a,
                        const std::map<std::string, std::uint64_t>& b,
                        std::size_t width, std::uint64_t shots) {
    std::vector<double> pa(width, 0.0), pb(width, 0.0);
    for (const auto& kv : a) {
        for (std::size_t i = 0; i < width; ++i) {
            if (kv.first[i] == '1') pa[i] += static_cast<double>(kv.second) / shots;
        }
    }
    for (const auto& kv : b) {
        for (std::size_t i = 0; i < width; ++i) {
            if (kv.first[i] == '1') pb[i] += static_cast<double>(kv.second) / shots;
        }
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < width; ++i) worst = std::max(worst, std::abs(pa[i] - pb[i]));
    return worst;
}

}  // namespace

TEST(TnNoiseParity, DeviceNoiseAtTwelveQubits_MatchesFullState) {
    // Arrange: rung 1 of the ladder, taken as wide as a FullState reference can
    // still be had cheaply. The device route synthesises relaxation + depol +
    // readout together, so this is the whole noise stack rather than
    // one hand-attached channel, and 12 qubits is 4x the width the device
    // test covered. A GHZ chain keeps the distribution concentrated, which is
    // what makes a joint TVD meaningful at this width: over a spread-out
    // distribution two independent 24k-shot samples of the SAME law differ by
    // more than the tolerance, so the test would be measuring shot noise.
    const std::string device =
        std::string(QPLAYER_ACCEPTANCE_DATA_DIR) + "/device_json/willow_like.json";
    const QubitIndex n = 12;
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);
    circ.measure_all();
    const std::uint32_t shots = 24000;

    QRegister fs(SimType::FullState, ProcType::CPU);
    fs.set_seed(20260826);
    fs.load_device_model(device);
    QRegister tn(SimType::TensorNetwork, ProcType::CPU);
    tn.set_seed(20260826);
    tn.load_device_model(device);

    // Act
    const auto fs_counts = fs.run(circ, shots).counts();
    const auto tn_counts = tn.run(circ, shots).counts();

    // Assert: the joint distribution first, then the per-qubit marginals. The
    // marginals are the tighter statement — each is a binomial at 24k shots, so
    // 0.01 is well outside the ~0.004 four-sigma band — and they are what catches
    // an error localised to one qubit, which a joint TVD spread over many keys
    // can dilute below its own tolerance.
    EXPECT_NEAR(tvd(fs_counts, tn_counts, shots), 0.0, 0.02);
    EXPECT_NEAR(max_marginal_gap(fs_counts, tn_counts, n, shots), 0.0, 0.01);
}

TEST(TnNoiseParity, MonteCarloConvergesToTheMpoExpectation) {
    // Arrange: rung 2. MPO carries rho exactly, so its <P> has no sampling error
    // at all and can serve as the reference the MC average is converging TO.
    // Comparing single runs would only show that both are "close"; the claim that
    // makes the ladder a ladder is that the gap SHRINKS with nshots, which needs
    // two shot levels and several seeds (one seed pair could shrink by luck).
    Circuit circ(8, 8);
    for (QubitIndex q = 0; q < 8; ++q) circ.ry(0.4 + 0.11 * q, q);
    circ.cx(0, 1).cx(2, 3).cx(4, 5).cx(6, 7).cx(1, 2).cx(3, 4).cx(5, 6);
    circ.add_expectation("ZZIIIIII");
    circ.add_expectation("IIIXXIII");
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.03);
    nm.add_amplitude_damping("ry", 0.02);

    qplayer::TensorNetworkBackend mpo;
    mpo.set_tn_use_mpo(true);
    mpo.set_seed(7);
    mpo.prepare(circ);
    const auto exact = mpo.execute(circ, 1, &nm);

    // Act: mean |MC - MPO| over 8 seeds at each shot level.
    const std::uint32_t low = 256, high = 16384;
    double err_low = 0.0, err_high = 0.0;
    const int seeds = 8;
    for (int s = 0; s < seeds; ++s) {
        QRegister a(SimType::TensorNetwork, ProcType::CPU);
        a.set_seed(1000u + static_cast<std::uint64_t>(s));
        a.set_noise_model(nm);
        const auto r_low = a.run(circ, low);
        QRegister b(SimType::TensorNetwork, ProcType::CPU);
        b.set_seed(1000u + static_cast<std::uint64_t>(s));
        b.set_noise_model(nm);
        const auto r_high = b.run(circ, high);
        for (const char* p : {"ZZIIIIII", "IIIXXIII"}) {
            err_low  += std::abs(r_low.expectation(p)  - exact.expectation(p));
            err_high += std::abs(r_high.expectation(p) - exact.expectation(p));
        }
    }
    err_low  /= (2 * seeds);
    err_high /= (2 * seeds);

    // Assert: at 16384 shots one standard error is under 0.008, so 0.03 is a wide
    // band that still excludes a systematic offset between the two routes; and
    // 64x the shots must buy a visibly smaller gap (8x in expectation).
    EXPECT_NEAR(err_high, 0.0, 0.03);
    EXPECT_TRUE(err_high < err_low);
}

TEST(TnNoiseParity, MonteCarloConvergesToTheMpoDistribution) {
    // Arrange: the same rung on counts rather than <P>. Both routes sample here
    // (MPO draws from rho's diagonal), so the reference is not exact and the
    // statement has to be the one that survives that: the gap between the routes
    // must fall to the shot-noise floor, not to zero.
    Circuit circ(6, 6);
    circ.ry(0.9, 0).ry(1.4, 1).ry(0.5, 2).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4).cx(4, 5);
    circ.measure_all();
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.02);

    // Act
    const std::uint32_t shots = 40000;
    const auto mc  = run_with(SimType::TensorNetwork, circ, nm, shots, 555);
    const auto rho = run_mpo(circ, nm, shots, 555);

    // Assert
    EXPECT_NEAR(tvd(mc, rho, shots), 0.0, 0.02);
    EXPECT_NEAR(max_marginal_gap(mc, rho, 6, shots), 0.0, 0.01);
}

TEST(TnNoiseParity, BondCapSaturationIsVisibleInTheReportedDiagnostics) {
    // Arrange: rung 3, the regime with no reference at all. A 60-qubit brickwork
    // under a deliberately small chi cap (16) is forced to truncate hard, which
    // is the situation 4 calls out: past the dense cap a
    // capped, badly truncated run and a clean one look identical to the caller.
    // Both axes must therefore come back — chi says the CAP decided the error
    // (so the accuracy figure is a floor, not a measurement), and the error
    // ledger says how much weight was actually thrown away.
    const QubitIndex n = 60;
    Circuit circ(n, n);
    for (QubitIndex q = 0; q < n; ++q) circ.h(q);
    // Rotations between the CX layers are what makes the entanglement grow: a
    // brickwork of CX alone maps |+...+> to itself, so a Clifford-only ladder
    // would sit at a small chi and never reach the cap this test is about.
    for (int layer = 0; layer < 12; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < n; q += 2) {
            circ.ry(0.63, q).ry(0.41, q + 1);
            circ.cx(q, q + 1);
        }
    }
    circ.measure_all();
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.01);

    qplayer::TensorNetworkBackend backend;
    backend.set_mps_truncation(16, 1e-12);
    backend.set_seed(4242);
    backend.prepare(circ);

    // Act
    const auto r = backend.execute(circ, 8, &nm);

    // Assert: counts exist at a width FullState cannot reach (2^60 amplitudes is
    // 16 EiB), the resource axis shows the cap was hit, and the accuracy axis
    // shows the loss that cap caused rather than staying silently empty.
    std::uint64_t total = 0;
    for (const auto& kv : r.counts()) total += kv.second;
    EXPECT_EQ(total, std::uint64_t{8});
    EXPECT_EQ(backend.last_trajectory_bond_dimension(), std::uint32_t{16});
    EXPECT_TRUE(r.has_truncation_errors());
    EXPECT_TRUE(accumulated_error(r.truncation_errors()) > qplayer::kMpsErrorBudgetWarn);
}

TEST(TnNoiseParity, ReportedBondDimensionTracksTheCircuitsEntanglement) {
    // Arrange: the chi diagnostic is only worth reading if it reflects the state
    // rather than the cap alone. Two runs of the same width and the same cap: a
    // product-state circuit, which cannot raise a bond above 1, and an entangling
    // one, which must. Without this pair a hard-wired "return max_bond" would
    // pass the saturation test above.
    const QubitIndex n = 10;
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.01);
    nm.add_bit_flip("x", 0.01);

    Circuit product(n, n);
    for (QubitIndex q = 0; q < n; ++q) product.x(q);
    product.measure_all();

    Circuit entangling(n, n);
    entangling.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) entangling.cx(q, q + 1);
    for (QubitIndex q = 0; q + 2 < n; ++q) entangling.cx(q, q + 2);
    entangling.measure_all();

    // Act
    qplayer::TensorNetworkBackend flat;
    flat.set_seed(11);
    flat.prepare(product);
    flat.execute(product, 4, &nm);
    const std::uint32_t chi_flat = flat.last_trajectory_bond_dimension();

    qplayer::TensorNetworkBackend deep;
    deep.set_seed(11);
    deep.prepare(entangling);
    deep.execute(entangling, 4, &nm);
    const std::uint32_t chi_deep = deep.last_trajectory_bond_dimension();

    // Assert: 1Q channels cannot entangle, so the product run stays at a
    // single bond; the CX ladder plus its skip connections must exceed it.
    EXPECT_EQ(chi_flat, std::uint32_t{1});
    EXPECT_TRUE(chi_deep > chi_flat);
}

TEST(TnNoiseParity, HeavyCappingKeepsExpectationsPhysical) {
    // Arrange: the drift's real damage is on <P>, which reads the state as stored
    // (mps.hpp) — an unnormalised chain reports every expectation scaled by
    // |psi|^2. The all-identity string is the direct probe: <I...I> IS <psi|psi>,
    // so it must come back as exactly 1 no matter how hard the run truncated.
    // Bounded-Pauli checks are too weak here — at a drift of 1.05 a value of
    // -0.51 scales to -0.53 and stays inside [-1, 1], so nothing fires.
    //
    // The width and depth are chosen to make the drift large: 40 qubits of
    // brickwork at chi_max 8 measured |psi|^2 = 1.4.
    const QubitIndex n = 40;
    Circuit circ(n, n);
    for (int layer = 0; layer < 12; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < n; q += 2) {
            circ.ry(0.63, q).ry(0.41, q + 1).cx(q, q + 1);
        }
    }
    circ.add_expectation(std::string(n, 'I'));            // = <psi|psi>
    circ.add_expectation("ZZ" + std::string(n - 2, 'I'));
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.01);

    qplayer::TensorNetworkBackend capped;
    capped.set_mps_truncation(8, 1e-12);
    capped.set_mps_gauge(qplayer::MpsGauge::None);   // the drifting split; removes the drift itself
    capped.set_seed(8080);
    capped.prepare(circ);

    // Act
    const auto tight = capped.execute(circ, 4, &nm);

    // Assert: the run truncated hard and the pre-normalisation drift is reported,
    // yet the state the observables saw was normalised — so <I...I> is 1 and the
    // Pauli value is inside its physical range.
    EXPECT_TRUE(capped.last_trajectory_norm_drift() > 0.1);
    EXPECT_NEAR(tight.expectation(std::string(n, 'I')), 1.0, 1e-9);
    EXPECT_TRUE(std::abs(tight.expectation("ZZ" + std::string(n - 2, 'I'))) <= 1.0 + 1e-12);
}

TEST(TnNoiseParity, HeavyCappingWithCanonicalGauge_ReportsNoNormDrift) {
    // Arrange: the circuit above, split at the orthogonality centre.
    // There every rescale is the global one and every Kraus branch is folded in
    // norm-preserving, so the hard cap no longer moves |psi|^2 at all.
    const QubitIndex n = 40;
    Circuit circ(n, n);
    for (int layer = 0; layer < 12; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < n; q += 2) {
            circ.ry(0.63, q).ry(0.41, q + 1).cx(q, q + 1);
        }
    }
    circ.add_expectation(std::string(n, 'I'));
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.01);

    qplayer::TensorNetworkBackend capped;
    capped.set_mps_truncation(8, 1e-12);
    // Default Auto: the first lossy split arms the gauge, and from there on the
    // run behaves as Canonical.
    capped.set_mps_gauge(qplayer::MpsGauge::Auto);
    capped.set_seed(8080);
    capped.prepare(circ);

    // Act
    const auto tight = capped.execute(circ, 4, &nm);

    // Assert: weight was discarded, yet the norm stayed at 1 to rounding.
    EXPECT_TRUE(capped.last_trajectory_norm_drift() < 1e-9);
    EXPECT_NEAR(tight.expectation(std::string(n, 'I')), 1.0, 1e-9);
}

TEST(TnNoiseParity, LosslessRunReportsNoNormDrift) {
    // Arrange: the control for the test above. Without it "drift is reported"
    // could be satisfied by a constant, and the tightened lossless branch of the
    // guard (still 1e-6) would have nothing pinning it.
    Circuit circ(10, 10);
    for (int layer = 0; layer < 6; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < 10; q += 2) {
            circ.ry(0.63, q).ry(0.41, q + 1).cx(q, q + 1);
        }
    }
    circ.add_expectation(std::string(10, 'I'));
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.01);

    qplayer::TensorNetworkBackend roomy;
    roomy.set_mps_truncation(4096, 1e-12);
    roomy.set_seed(8080);
    roomy.prepare(circ);

    // Act
    const auto loose = roomy.execute(circ, 4, &nm);

    // Assert
    EXPECT_NEAR(roomy.last_trajectory_norm_drift(), 0.0, 1e-12);
    EXPECT_FALSE(loose.has_truncation_errors() &&
                 accumulated_error(loose.truncation_errors()) > 0.0);
    EXPECT_NEAR(loose.expectation(std::string(10, 'I')), 1.0, 1e-12);
}

// ── MPO positivity regime guard ────────────────────────────
//
// Operator Schmidt truncation does not preserve positivity, and measured
// where that bites: cap saturation with weak noise puts site conditionals at
// -1.95 to -6.99, which is not a perturbed distribution but a broken one. These
// pin the boundary and both documented ways off it.

namespace {

// n=8 brickwork, the shape the sweep used. The NoiseModel attaches a
// channel to a gate the circuit never contains, so the MPO route is taken with
// no channel firing — weak noise is the regime the guard is about, and "none at
// all" is its limit.
Circuit mpo_positivity_circuit() {
    Circuit circ(8, 8);
    for (int layer = 0; layer < 8; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < 8; q += 2) {
            circ.ry(0.63, q).ry(0.41, q + 1).cx(q, q + 1);
        }
    }
    circ.measure_all();
    return circ;
}

}  // namespace

TEST(TnNoiseParity, MpoBrokenPositivity_RefusesToReturnCounts) {
    // Arrange
    const Circuit circ = mpo_positivity_circuit();
    NoiseModel nm;
    nm.add_depolarizing("rz", 0.05);   // no rz in the circuit — nothing fires

    qplayer::TensorNetworkBackend be;
    be.set_tn_use_mpo(true);
    be.set_mpo_truncation(32, 1e-12);
    be.set_seed(5);
    be.prepare(circ);

    // Act + Assert: counts drawn from clamped negative weights would look exactly
    // like healthy counts, and above the dense cap there is nothing to compare
    // them against — so the run fails instead.
    EXPECT_THROW(be.execute(circ, 64, &nm), SimulatorError);
}

TEST(TnNoiseParity, MpoBrokenPositivity_RaisingTheBondCapIsAWayOut) {
    // Arrange: the same circuit with room for the operator Schmidt spectrum.
    const Circuit circ = mpo_positivity_circuit();
    NoiseModel nm;
    nm.add_depolarizing("rz", 0.05);

    qplayer::TensorNetworkBackend be;
    be.set_tn_use_mpo(true);
    be.set_mpo_truncation(1024, 1e-12);
    be.set_seed(5);
    be.prepare(circ);

    // Act
    const auto r = be.execute(circ, 64, &nm);

    // Assert
    std::uint64_t total = 0;
    for (const auto& kv : r.counts()) total += kv.second;
    EXPECT_EQ(total, std::uint64_t{64});
}

TEST(TnNoiseParity, MpoBrokenPositivity_StrongNoiseIsTheOtherWayOut) {
    // Arrange: identical cap, but now the noise MPO is actually for. Damping is
    // what regularises the spectrum, so the same truncation that broke
    // positivity above stays inside it here — which is the whole argument for
    // guarding the regime instead of changing the representation to LPDO.
    const Circuit circ = mpo_positivity_circuit();
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.3);

    qplayer::TensorNetworkBackend be;
    be.set_tn_use_mpo(true);
    be.set_mpo_truncation(32, 1e-12);
    be.set_seed(5);
    be.prepare(circ);

    // Act
    const auto r = be.execute(circ, 64, &nm);

    // Assert
    std::uint64_t total = 0;
    for (const auto& kv : r.counts()) total += kv.second;
    EXPECT_EQ(total, std::uint64_t{64});
}

TEST(TnNoiseParity, MildNegativity_StillReturnsCounts) {
    // Arrange: the other side of the boundary. A raised cutoff truncates
    // gently, and the measured worst conditional there was -0.46 / -0.49 — inside
    // the tolerated band. The run must therefore COMPLETE: a guard that threw on
    // any negative would take away the cutoff knob that buys MPO its compression
    // in the first place.
    const Circuit circ = mpo_positivity_circuit();
    NoiseModel nm;
    nm.add_depolarizing("rz", 0.05);   // nothing fires — same weak-noise limit

    qplayer::TensorNetworkBackend be;
    be.set_tn_use_mpo(true);
    be.set_mpo_truncation(128, 1e-3);
    be.set_seed(5);
    be.prepare(circ);

    // Act
    const auto r = be.execute(circ, 64, &nm);

    // Assert
    std::uint64_t total = 0;
    for (const auto& kv : r.counts()) total += kv.second;
    EXPECT_EQ(total, std::uint64_t{64});
}
