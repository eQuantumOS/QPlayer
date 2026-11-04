// test_tn_midcircuit.cpp — Mid-circuit measurement collapse on TensorNetwork
// 1.1, 1.3.
//
// The failure this file defends against is silent, not loud: deferring a
// measurement produces well-formed counts, so a circuit that reuses a measured
// qubit used to return a plausible WRONG distribution rather than an error. The
// fixtures below are therefore built so that "no collapse" and "collapse" give
// visibly different answers — `h; measure; h; measure` is deterministic without
// collapse and uniform with it — and every distribution is checked against
// FullState CPU, which has had the per-shot path since T5.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <string>

#include "../../../include/qregister.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::SimType;
using qplayer::SimulatorError;

namespace {

using Counts = std::map<std::string, std::uint64_t>;

Counts run_on(SimType sim, const Circuit& circ, std::uint32_t shots, std::uint64_t seed) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    return qr.run(circ, shots).counts();
}

Counts run_noisy_on(SimType sim, const Circuit& circ, const NoiseModel& nm,
                    std::uint32_t shots, std::uint64_t seed) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    qr.set_noise_model(nm);
    return qr.run(circ, shots).counts();
}

double prob_of(const Counts& c, const std::string& key, std::uint32_t shots) {
    const auto it = c.find(key);
    return (it == c.end()) ? 0.0 : static_cast<double>(it->second) / shots;
}

// Total variation distance, so a whole distribution is one number.
double tvd(const Counts& a, const Counts& b, std::uint32_t shots) {
    double d = 0.0;
    for (const auto& kv : a) d += std::abs(static_cast<double>(kv.second) / shots -
                                           prob_of(b, kv.first, shots));
    for (const auto& kv : b) {
        if (a.find(kv.first) == a.end()) d += static_cast<double>(kv.second) / shots;
    }
    return 0.5 * d;
}

}  // namespace

// The decisive case: without collapse the second H undoes the first and c1 is
// always 0; with collapse the two bits are independent and uniform.
TEST(TnMidcircuit, MeasureThenHadamard_SecondBitIsUnbiased) {
    // Arrange
    Circuit circ(1, 2);
    circ.h(0).measure(0, 0).h(0).measure(0, 1);
    const std::uint32_t shots = 20000;

    // Act
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 7717);

    // Assert: four outcomes at 25% each. Key is MSB-first over cubits, so the
    // leading character is c1. Tolerance 0.02 is the nshots=20000 statistical
    // band (~3.5 sigma); a deferred measurement would put 1.0 on the "0*" keys.
    EXPECT_NEAR(prob_of(tn, "00", shots), 0.25, 0.02);
    EXPECT_NEAR(prob_of(tn, "01", shots), 0.25, 0.02);
    EXPECT_NEAR(prob_of(tn, "10", shots), 0.25, 0.02);
    EXPECT_NEAR(prob_of(tn, "11", shots), 0.25, 0.02);
}

// The completion condition, on a superposed single qubit.
TEST(TnMidcircuit, MeasureThenHadamard_MatchesFullState) {
    // Arrange
    Circuit circ(1, 2);
    circ.h(0).measure(0, 0).h(0).measure(0, 1);
    const std::uint32_t shots = 20000;

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, shots, 991);
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 991);

    // Assert: 0.01 is the bound; the sampling floor over 4 outcomes
    // 20000 shots is about a third of it.
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.01);
}

// A gate that anti-correlates the two reads. If the projection were dropped the
// state would still be |+>, and "00"/"11" would appear at 25% each.
TEST(TnMidcircuit, MeasureThenX_SecondBitIsComplement) {
    // Arrange
    Circuit circ(1, 2);
    circ.h(0).measure(0, 0).x(0).measure(0, 1);
    const std::uint32_t shots = 4000;

    // Act
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 5150);

    // Assert: exactly two keys, both at 50%. Exact zero for the others, since
    // an anti-correlation violation is structural rather than statistical.
    EXPECT_NEAR(prob_of(tn, "10", shots), 0.5, 0.03);
    EXPECT_NEAR(prob_of(tn, "01", shots), 0.5, 0.03);
    EXPECT_TRUE(tn.find("00") == tn.end());
    EXPECT_TRUE(tn.find("11") == tn.end());
}

// Collapse on an ENTANGLED chain, where the projected site has bond dimension 2
// on both sides and the partner qubit's state depends on the outcome. A
// one-site projector that forgot to renormalise would still pass the marginal
// test above but skew this one.
TEST(TnMidcircuit, BellMeasureThenHadamard_MatchesFullState) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).h(1).measure(1, 1);
    const std::uint32_t shots = 20000;

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, shots, 30313);
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 30313);

    // Assert
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.01);
    // And the physics: collapsing q0 leaves q1 in a basis state, so the H makes
    // c1 unbiased and independent of c0 — 25% on each of four keys.
    EXPECT_NEAR(prob_of(tn, "11", shots), 0.25, 0.02);
}

// Boundary: a measurement whose outcome is already certain. p0 or p1 is exactly
// zero here, which is the branch the degeneracy guard has to let through.
TEST(TnMidcircuit, DeterministicQubit_CollapseIsExact) {
    // Arrange
    Circuit circ(1, 2);
    circ.x(0).measure(0, 0).x(0).measure(0, 1);
    const std::uint32_t shots = 512;

    // Act
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 8);

    // Assert: c0 = 1, c1 = 0 on every shot, so one key holds all of them.
    EXPECT_EQ(tn.size(), std::size_t{1});
    EXPECT_EQ(tn.at("01"), static_cast<std::uint64_t>(shots));
}

// Three rounds on three qubits, the shape a syndrome-extraction round has
// (measure an ancilla, use it again, measure again). Chain long enough that the
// site permutation is exercised too.
TEST(TnMidcircuit, RepeatedRounds_MatchesFullState) {
    // Arrange
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).measure(1, 0)
        .h(1).cx(1, 2).measure(2, 1)
        .h(2).measure(0, 2);
    const std::uint32_t shots = 20000;

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, shots, 60607);
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 60607);

    // Assert: 8 outcomes, so the sampling floor is higher than the 2-cubit cases.
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.015);
}

// Same seed, same counts. The per-shot loop is OpenMP-parallel, so this fails
// if the shot RNG were taken from one shared stream instead of being derived
// from the shot index.
TEST(TnMidcircuit, FixedSeed_ReproducesCountsExactly) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).h(1).measure(1, 1);

    // Act
    const Counts a = run_on(SimType::TensorNetwork, circ, 2000, 123456);
    const Counts b = run_on(SimType::TensorNetwork, circ, 2000, 123456);

    // Assert
    EXPECT_TRUE(a == b);
}

// The noisy route collapses in place too — it was deferring, and therefore
// silently wrong, on exactly the same circuits.
TEST(TnMidcircuit, NoisyTrajectory_MatchesFullState) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).h(1).measure(1, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.03);
    nm.add_depolarizing("cx", 0.05);
    const std::uint32_t shots = 12000;

    // Act
    const Counts fs = run_noisy_on(SimType::FullState,     circ, nm, shots, 24680);
    const Counts tn = run_noisy_on(SimType::TensorNetwork, circ, nm, shots, 24680);

    // Assert: two independent Monte Carlo runs, so the bound is the trajectory
    // sampling floor rather than the 0.01 of the noiseless comparison.
    EXPECT_NEAR(tvd(fs, tn, shots), 0.0, 0.02);
}

// State access after the per-shot path: nshots chains evolved and none is "the"
// state, so a plausible-looking |0...0> must not be returned.
TEST(TnMidcircuit, StateAccessAfterPerShot_Throws) {
    // Arrange
    Circuit circ(1, 2);
    circ.h(0).measure(0, 0).h(0).measure(0, 1);
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_seed(11);

    // Act
    qr.run(circ, 64);

    // Assert
    EXPECT_THROW(qr.get_statevector(), SimulatorError);
}

// The MPO route cannot project rho, so it refuses instead of deferring.
TEST(TnMidcircuit, MpoRoute_RejectsMidCircuitMeasurement) {
    // Arrange
    Circuit circ(1, 2);
    circ.h(0).measure(0, 0).h(0).measure(0, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.02);
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_noise_model(nm);
    qr.set_tn_noise_path("mpo");

    // Act + Assert
    EXPECT_THROW(qr.run(circ, 16), SimulatorError);
}
