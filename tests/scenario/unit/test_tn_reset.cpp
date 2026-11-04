// test_tn_reset.cpp — Reset on TensorNetwork.
// 1.1, 1.3.
//
// Reset is a measurement plus a conditional X, so it is stochastic even though
// its result is not. Two things follow, and both are tested here: it must not be
// routed through gate_full_matrix() (which has no matrix to give and reported a
// [FullState]-tagged error from a TensorNetwork run), and it must be drawn PER
// SHOT (evolving it once and sampling nshots reports a single collapse branch
// nshots times, which on an entangled qubit is a visible error).
//
// Where FullState CPU is itself sound the comparison here is exact rather than
// statistical: the two backends draw the same uniforms in the same order, so a
// fixed seed gives bit-identical counts. The one circuit where they disagree is
// called out explicitly below.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

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

Counts run_on(SimType sim, const Circuit& circ, std::uint32_t shots, std::uint64_t seed,
              const NoiseModel* nm = nullptr) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    if (nm != nullptr) qr.set_noise_model(*nm);
    return qr.run(circ, shots).counts();
}

double prob_of(const Counts& c, const std::string& key, std::uint32_t shots) {
    const auto it = c.find(key);
    return (it == c.end()) ? 0.0 : static_cast<double>(it->second) / shots;
}

}  // namespace

// Reset used to die here, and with the wrong backend name on the message.
TEST(TnReset, PlainResetCircuit_RunsInsteadOfThrowing) {
    // Arrange
    Circuit circ(1, 1);
    circ.h(0).reset(0).measure(0, 0);

    // Act
    const Counts tn = run_on(SimType::TensorNetwork, circ, 512, 8);

    // Assert: reset forces |0>, so one key holds every shot.
    EXPECT_EQ(tn.size(), std::size_t{1});
    EXPECT_EQ(tn.at("0"), std::uint64_t{512});
}

// The completion condition, on the circuits where FullState CPU is a
// valid reference. Bit-identical, not merely within 0.01 — same seed, same
// uniform stream, same comparison.
TEST(TnReset, ResetThenReprepare_MatchesFullStateExactly) {
    // Arrange
    Circuit circ(1, 2);
    circ.h(0).measure(0, 0).reset(0).h(0).measure(0, 1);

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, 4000, 4242);
    const Counts tn = run_on(SimType::TensorNetwork, circ, 4000, 4242);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// A syndrome-extraction round in miniature: entangle, read the ancilla, reset
// it, reuse it. This is the shape Phase 10-A exists for.
TEST(TnReset, AncillaReuseRound_MatchesFullStateExactly) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).reset(0).h(0).measure(0, 1);

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, 4000, 30313);
    const Counts tn = run_on(SimType::TensorNetwork, circ, 4000, 30313);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// Reset on the noise route — the op every QEC round needs and the one FullState
// GPU still cannot carry, which is why verify-sc17-willow runs on CPU.
TEST(TnReset, NoisyAncillaReuse_MatchesFullStateExactly) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).reset(0).h(0).measure(0, 1);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.03);
    nm.add_depolarizing("cx", 0.05);

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, 8000, 24680, &nm);
    const Counts tn = run_on(SimType::TensorNetwork, circ, 8000, 24680, &nm);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// Resetting one half of a Bell pair. Per-shot physics says the partner is
// unbiased; evolving the reset once and sampling would report one branch every
// time. FullState CPU does exactly that today, so this case
// is asserted against the physics rather than against the other backend.
TEST(TnReset, EntangledReset_LeavesPartnerUnbiased) {
    // Arrange
    Circuit circ(2, 1);
    circ.h(0).cx(0, 1).reset(0).measure(1, 0);
    const std::uint32_t shots = 20000;

    // Act
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 4242);

    // Assert: 0.02 is the nshots=20000 statistical band; a once-evolved reset
    // would put 1.0 on one key.
    EXPECT_NEAR(prob_of(tn, "0", shots), 0.5, 0.02);
    EXPECT_NEAR(prob_of(tn, "1", shots), 0.5, 0.02);
}

// Reset of a qubit that is already |0>: p1 is exactly zero, the branch the
// degeneracy guard has to let through, and the X must not fire.
TEST(TnReset, ResetOfGroundState_IsIdentity) {
    // Arrange
    Circuit circ(2, 2);
    circ.x(1).reset(0).measure(0, 0).measure(1, 1);

    // Act
    const Counts tn = run_on(SimType::TensorNetwork, circ, 256, 5);

    // Assert: q0 = 0, q1 = 1 on every shot.
    EXPECT_EQ(tn.size(), std::size_t{1});
    EXPECT_EQ(tn.at("10"), std::uint64_t{256});
}

// Reset of a qubit already at |1>: the conditional X is the part under test.
TEST(TnReset, ResetOfExcitedState_ForcesZero) {
    // Arrange
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);

    // Act
    const Counts tn = run_on(SimType::TensorNetwork, circ, 256, 6);

    // Assert
    EXPECT_EQ(tn.size(), std::size_t{1});
    EXPECT_EQ(tn.at("0"), std::uint64_t{256});
}

// Every remaining TN rejection must name TensorNetwork. The Reset path used to
// surface "[FullState] fusion: gate_full_matrix called for non-unitary gate
// type" from a TensorNetwork run (completion condition).
TEST(TnReset, MpoRouteRejectsReset_WithTensorNetworkTag) {
    // Arrange
    Circuit circ(1, 1);
    circ.h(0).reset(0).measure(0, 0);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.02);
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_noise_model(nm);
    qr.set_tn_noise_path("mpo");

    // Act + Assert
    bool threw = false;
    try {
        qr.run(circ, 16);
    } catch (const SimulatorError& e) {
        threw = true;
        const std::string msg = e.what();
        EXPECT_TRUE(msg.find("[TensorNetwork]") != std::string::npos);
        EXPECT_TRUE(msg.find("[FullState]") == std::string::npos);
    }
    EXPECT_TRUE(threw);
}
