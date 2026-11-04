// test_reset_routing.cpp — Reset as a per-shot trigger.
//
// A reset is a measurement plus a conditional X, so it is stochastic. Evolving
// it once and drawing nshots from the result reports ONE collapse branch nshots
// times — invisible on an unentangled qubit and a plain wrong answer on an
// entangled one. Circuit::requires_per_shot() is what keeps that from happening,
// and it is deliberately narrower than "any Reset": a leading `reset q[i];` on an
// untouched qubit is deterministic and must stay on the fast path.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <complex>
#include <cstdlib>
#include <map>
#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/qregister.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::SimType;

namespace {

using Counts = std::map<std::string, std::uint64_t>;

Counts run_on(SimType sim, const Circuit& circ, std::uint32_t shots, std::uint64_t seed) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    return qr.run(circ, shots).counts();
}

double prob_of(const Counts& c, const std::string& key, std::uint32_t shots) {
    const auto it = c.find(key);
    return (it == c.end()) ? 0.0 : static_cast<double>(it->second) / shots;
}

}  // namespace

// The predicate itself: a Reset on an untouched qubit is deterministic and must
// NOT force per-shot, or the `reset q[0]; reset q[1]; ...` prologue that opens a
// great many QASM files would cost an nshots-fold slowdown for nothing.
TEST(ResetRouting, ResetOnUntouchedQubit_StaysOnTheFastPath) {
    // Arrange
    Circuit circ(2, 2);
    circ.reset(0).reset(1).h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act + Assert
    EXPECT_FALSE(circ.requires_per_shot());
    EXPECT_FALSE(circ.has_classical_control());
}

// A Reset after a gate on the same qubit is the trigger.
TEST(ResetRouting, ResetAfterGateOnSameQubit_RequiresPerShot) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).reset(0).measure(1, 0);

    // Act + Assert
    EXPECT_TRUE(circ.requires_per_shot());
    // ...and the literal predicate is unchanged, which is the point of keeping
    // the two apart: the GPU's "classical-control not supported" rejection reads
    // this one and must not fire on a plain reset circuit.
    EXPECT_FALSE(circ.has_classical_control());
}

// Everything has_classical_control() catches is still caught.
TEST(ResetRouting, ClassicalControlStillImpliesPerShot) {
    // Arrange
    Circuit with_if(2, 2);
    with_if.h(0).measure(0, 0);
    GateOp body;
    body.type   = GateType::X;
    body.name   = "x";
    body.qubits = {1};
    with_if.if_then(0, 1, body);

    Circuit reuse(1, 2);
    reuse.h(0).measure(0, 0).h(0).measure(0, 1);

    // Act + Assert
    EXPECT_TRUE(with_if.has_classical_control());
    EXPECT_TRUE(with_if.requires_per_shot());
    EXPECT_TRUE(reuse.has_classical_control());
    EXPECT_TRUE(reuse.requires_per_shot());
}

// The physics the routing exists for. Resetting one half of a Bell pair leaves
// the partner unbiased over shots; a once-evolved reset puts every shot in one
// branch. Measured before : FullState CPU, FullState GPU and
// SparseState all returned "1" 2000 times out of 2000.
//
// shots is 1000, not the 20000 this test first carried. The per-shot path opens
// one parallel region per gate per shot with no dimension floor, so a 2-qubit
// circuit costs ~5.3 us/shot/backend on a 144-thread host (20000 shots measured
// 106 s at OMP_NUM_THREADS=144 vs 4.8 s at 1 — the cost is region entry, not
// work). 1000 shots keeps the whole suite near 5 s while the band below still
// misses the regression by 10x.
TEST(ResetRouting, EntangledReset_LeavesPartnerUnbiased_OnEveryBackend) {
    // Arrange
    Circuit circ(2, 1);
    circ.h(0).cx(0, 1).reset(0).measure(1, 0);
    const std::uint32_t shots = 1000;

    // Act
    const Counts fs = run_on(SimType::FullState,     circ, shots, 4242);
    const Counts sp = run_on(SimType::SparseState,   circ, shots, 4242);
    const Counts tn = run_on(SimType::TensorNetwork, circ, shots, 4242);

    // Assert: 0.05 is the nshots=1000 statistical band (3.2 sigma; the fixed seed
    // makes the draw deterministic and it lands at 0.510). The old behaviour put
    // 1.0 on a single key, so this fails by a mile if the routing regresses.
    for (const Counts* c : {&fs, &sp, &tn}) {
        EXPECT_NEAR(prob_of(*c, "0", shots), 0.5, 0.05);
        EXPECT_NEAR(prob_of(*c, "1", shots), 0.5, 0.05);
    }

    // Cross-backend bit-identity is the sharper half of the guard and costs
    // nothing: aligned the measurement draw convention, so per-shot
    // circuits agree key for key at a fixed seed. A backend that regresses to
    // a once-evaluated reset breaks this before it breaks the band above, and
    // this catches it at any shot count.
    EXPECT_TRUE(fs == sp);
    EXPECT_TRUE(fs == tn);
}

// Deterministic resets keep giving deterministic answers — the routing change
// must not perturb the circuits it newly sends down the per-shot path.
TEST(ResetRouting, DeterministicResetIsUnaffectedByRouting) {
    // Arrange: `x q0; reset q0` is caught by the conservative rule (q0 was
    // touched) even though its outcome is certain.
    Circuit circ(1, 1);
    circ.x(0).reset(0).measure(0, 0);

    // Act
    const Counts fs = run_on(SimType::FullState, circ, 512, 6);

    // Assert
    EXPECT_TRUE(circ.requires_per_shot());
    EXPECT_EQ(fs.size(), std::size_t{1});
    EXPECT_EQ(fs.at("0"), std::uint64_t{512});
}

// ── Shot-axis parallelism ─────────────────────────────────────────
// The per-shot path runs shots in parallel, so what used to be one sequential
// stream of state evolution is now N of them. Two properties must survive that,
// and neither is implied by "the distribution still looks right":
//   1. counts are IDENTICAL, not merely close, across thread counts — the shot
//      seed is derived from the shot index, so thread count must not enter.
// (Before the Born probability was summed with reduction(+:p1),
//      which made its low bits thread-count dependent.)
// 2. the statevector is still the LAST shot's state (contract), which a
//      parallel loop only preserves because shot nshots-1 is named explicitly.
TEST(ResetRouting, PerShotCountsAreIdenticalAcrossThreadCounts) {
    // Arrange: mid-circuit measurement + conditional + reset — every construct
    // the per-shot path exists for, so all of them are covered by one circuit.
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0);
    GateOp body;
    body.type   = GateType::X;
    body.name   = "x";
    body.qubits = {1};
    circ.if_then(0, 1, body);
    circ.reset(0).h(0).measure(0, 1);
    EXPECT_TRUE(circ.requires_per_shot());

    // Act: same seed, four team sizes spanning serial to the full host.
    Counts baseline;
    for (int threads : {1, 3, 16, 0}) {  // 0 = restore the runtime default
        QRegister qr(SimType::FullState, ProcType::CPU);
        qr.set_omp_threads(threads);
        qr.set_seed(90210);
        const Counts c = qr.run(circ, 400).counts();
        if (baseline.empty()) baseline = c;

        // Assert: bit-identical, not statistically close.
        EXPECT_TRUE(c == baseline);
    }

    // A degenerate all-in-one-key result would satisfy equality trivially, so
    // pin that the circuit actually branches.
    EXPECT_TRUE(baseline.size() > std::size_t{1});
}

// The statevector contract under a parallel shot loop.
TEST(ResetRouting, PerShotStatevectorIsTheLastShotUnderParallelism) {
    // Arrange: after `reset q0; measure q0`, q0 is |0> in every shot, so the
    // last shot's state is determined even though shot order is not.
    Circuit circ(1, 1);
    circ.h(0).reset(0).measure(0, 0);

    // Act
    QRegister qr(SimType::FullState, ProcType::CPU);
    qr.set_seed(31337);
    qr.run(circ, 256);
    const auto sv = qr.get_statevector();

    // Assert: |0> exactly — amplitude 1 on index 0. A parallel loop that lost
    // the last-shot handoff would leave whatever thread finished last, and a
    // loop that skipped the handoff entirely would leave an empty/stale state.
    EXPECT_EQ(sv.size(), std::size_t{2});
    EXPECT_NEAR(sv[0].real(), 1.0, 1e-12);  // exact by construction, not statistical
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(std::abs(sv[1]), 0.0, 1e-12);
}
