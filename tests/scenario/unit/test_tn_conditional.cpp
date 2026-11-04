// test_tn_conditional.cpp — Classical conditionals (IfOp) on TensorNetwork
//
// A conditional cannot be decided ahead of the shot: the register only has a
// value once that shot's measurements have run, which is why these circuits
// route to the per-shot path at all. The cases below therefore check
// two different things — that the branch fires on the right register value, and
// that everything downstream of the branch (noise channels, Reset bodies, bit
// ordering) behaves as it does for an unconditional gate.
//
// Where FullState CPU is a valid reference the comparison is exact: same seed,
// same uniform stream, same register reconstruction. The one place the two
// backends disagree is noise on a conditional body, and that case is asserted
// against an equivalent unconditional circuit instead.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::SimType;
using qplayer::SimulatorError;

namespace {

using Counts = std::map<std::string, std::uint64_t>;

GateOp body_of(GateType type, const char* name, std::vector<QubitIndex> qubits) {
    GateOp op;
    op.type   = type;
    op.name   = name;
    op.qubits = std::move(qubits);
    return op;
}

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

// The teleport-style correction: measure one qubit, conditionally flip another.
// Used to die at tn_fuse() with "classical conditionals are not supported".
TEST(TnConditional, OneQubitBody_MatchesFullStateExactly) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    circ.if_then(0, 1, body_of(GateType::X, "x", {1}));
    circ.measure(1, 1);

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, 4000, 4242);
    const Counts tn = run_on(SimType::TensorNetwork, circ, 4000, 4242);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// A 2Q body, which takes the merge/SVD/split path rather than a single site.
TEST(TnConditional, TwoQubitBody_MatchesFullStateExactly) {
    // Arrange
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).measure(0, 0);
    circ.if_then(0, 1, body_of(GateType::CX, "cx", {1, 2}));
    circ.measure(1, 1).measure(2, 2);

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, 4000, 30313);
    const Counts tn = run_on(SimType::TensorNetwork, circ, 4000, 30313);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// A Reset body — the primitive reached through a conditional.
TEST(TnConditional, ResetBody_MatchesFullStateExactly) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).x(1).measure(0, 0);
    circ.if_then(0, 1, body_of(GateType::Reset, "reset", {1}));
    circ.measure(1, 1);

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, 4000, 60607);
    const Counts tn = run_on(SimType::TensorNetwork, circ, 4000, 60607);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// The branch must fire on the register VALUE, not merely on "something was
// measured". Condition 0 is the complement of condition 1, so a routine that
// ignored `value` would produce the same counts for both.
TEST(TnConditional, BranchesOnRegisterValue_NotJustOnMeasurement) {
    // Arrange
    Circuit on_one(2, 2);
    on_one.h(0).measure(0, 0);
    on_one.if_then(0, 1, body_of(GateType::X, "x", {1}));
    on_one.measure(1, 1);

    Circuit on_zero(2, 2);
    on_zero.h(0).measure(0, 0);
    on_zero.if_then(0, 0, body_of(GateType::X, "x", {1}));
    on_zero.measure(1, 1);

    // Act
    const Counts one  = run_on(SimType::TensorNetwork, on_one,  4000, 11);
    const Counts zero = run_on(SimType::TensorNetwork, on_zero, 4000, 11);

    // Assert: c1 == c0 for `== 1`, c1 == !c0 for `== 0`. The two key sets are
    // disjoint, which no value-blind implementation can produce.
    EXPECT_TRUE(one.find("01") == one.end());
    EXPECT_TRUE(one.find("10") == one.end());
    EXPECT_TRUE(zero.find("00") == zero.end());
    EXPECT_TRUE(zero.find("11") == zero.end());
}

// Multi-bit register: the condition is an integer over reg.width bits, LSB
// first. A reversed reconstruction would compare against a different number and
// fire on the wrong shots.
TEST(TnConditional, MultiBitRegisterValue_MatchesFullStateExactly) {
    // Arrange: c holds two bits, and the body fires only on c == 0b10 = 2,
    // i.e. c1 = 1 and c0 = 0 — an asymmetric target, so bit order matters.
    Circuit circ(3, 3);
    circ.h(0).h(1).measure(0, 0).measure(1, 1);
    circ.if_then(0, 2, body_of(GateType::X, "x", {2}));
    circ.measure(2, 2);

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, 8000, 909);
    const Counts tn = run_on(SimType::TensorNetwork, circ, 8000, 909);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// Noise on a conditional body. FullState skips the body's channels entirely
// so the reference here is an EQUIVALENT UNCONDITIONAL circuit:
// q0 deterministically 1 the branch always fires, and the two circuits are then
// physically indistinguishable as far as gate noise is concerned.
TEST(TnConditional, NoiseOnAlwaysTrueBody_MatchesUnconditionalCircuit) {
    // Arrange
    NoiseModel nm;
    nm.add_depolarizing("x", 0.06);
    const std::uint32_t shots = 20000;

    Circuit conditional(2, 2);
    conditional.x(0).measure(0, 0);
    conditional.if_then(0, 1, body_of(GateType::X, "x", {1}));
    conditional.measure(1, 1);

    Circuit unconditional(2, 2);
    unconditional.x(0).measure(0, 0).x(1).measure(1, 1);

    // Act
    const Counts cond   = run_on(SimType::TensorNetwork, conditional,   shots, 77, &nm);
    const Counts uncond = run_on(SimType::TensorNetwork, unconditional, shots, 77, &nm);

    // Assert: the body's error rate must be there. Both circuits put roughly the
    // same weight on "q1 came out wrong while q0 read 1" — key "01". Tolerance
    // 0.01 covers the shot noise on a ~2.6% branch at 20000 shots. Dropping the
    // body's channel would make the conditional's "01" exactly zero.
    EXPECT_NEAR(prob_of(cond, "01", shots), prob_of(uncond, "01", shots), 0.01);
    EXPECT_TRUE(prob_of(cond, "01", shots) > 0.01);
}

// Same seed, same counts, on the OpenMP-parallel per-shot loop.
TEST(TnConditional, FixedSeed_ReproducesCountsExactly) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    circ.if_then(0, 1, body_of(GateType::X, "x", {1}));
    circ.measure(1, 1);

    // Act
    const Counts a = run_on(SimType::TensorNetwork, circ, 2000, 123456);
    const Counts b = run_on(SimType::TensorNetwork, circ, 2000, 123456);

    // Assert
    EXPECT_TRUE(a == b);
}

// An unsupported body must be refused before any shot runs, not on the first
// shot whose condition happens to be true. The condition
// here is never satisfiable, so a per-shot check would let the circuit pass.
TEST(TnConditional, UnsupportedBodyIsRejectedEvenIfNeverTaken) {
    // Arrange: c is 2 bits wide but only c[0] is ever written, so c == 2
    // (which needs c[1] = 1) cannot hold on any shot.
    Circuit circ(3, 2);
    circ.h(0).measure(0, 0);
    circ.if_then(0, 2, body_of(GateType::CCX, "ccx", {0, 1, 2}));
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);

    // Act + Assert
    bool threw = false;
    try {
        qr.run(circ, 8);
    } catch (const SimulatorError& e) {
        threw = true;
        const std::string msg = e.what();
        EXPECT_TRUE(msg.find("[TensorNetwork]") != std::string::npos);
    }
    EXPECT_TRUE(threw);
}

// rho carries no per-shot classical register, so the MPO route refuses.
TEST(TnConditional, MpoRouteRejectsConditional_WithTensorNetworkTag) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    circ.if_then(0, 1, body_of(GateType::X, "x", {1}));
    circ.measure(1, 1);
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
