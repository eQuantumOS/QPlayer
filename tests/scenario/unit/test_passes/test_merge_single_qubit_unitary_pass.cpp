// test_merge_single_qubit_unitary_pass.cpp — Unit tests for
// MergeSingleQubitUnitaryPass (extension).
//
// Correctness is verified by:
//   1. Op count reduction (structural).
//   2. Quantum-state equivalence: the original and merged circuits produce the
//      same statevector up to global phase.  We measure |<psi_orig|psi_merged>|
//      (overlap magnitude) and require it >= 1 - 1e-10.  This is safe for
// global-phase-discarding decompositions.
//
// The pass processes all 1Q gate runs of length >= 2 (including rotations).
// Boundaries: 2Q+ gate, Measure, Barrier, If, Idle, Opaque, Reset.
//
// : Arrange / Act / Assert.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <vector>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/qregister.hpp"
#include "../../../../include/transpiler/passes/optimization/merge_single_qubit_unitary_pass.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::MergeSingleQubitUnitaryPass;
using qplayer::Op;
using qplayer::QRegister;
using qplayer::SimType;
using qplayer::StateVector;

namespace {

// Compute the inner product magnitude |<sv_a|sv_b>| between two statevectors.
// Both must have the same length (same qubit count).
double inner_product_magnitude(const StateVector& a, const StateVector& b) {
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t i = 0; i < a.size(); ++i) {
        // conj(a[i]) * b[i]
        acc += std::conj(a[i]) * b[i];
    }
    return std::abs(acc);
}

// Run a circuit on FullState CPU and return the statevector.
// nshots=1 is sufficient to prepare the statevector (no measurement).
StateVector run_statevector(Circuit c) {
    QRegister qr{SimType::FullState};
    qr.set_seed(42);
    qr.run(c, 1);
    return qr.get_statevector();
}

}  // namespace

// ── Case 1: H·RZ(0.3)·H runs merged to single U3 ───────────────────────────

TEST(MergeSingleQubitUnitaryPass, HRzH_MergedToU3) {
    // Arrange
    Circuit orig(1);
    orig.h(0).rz(0.3, 0).h(0);

    Circuit merged_in(1);
    merged_in.h(0).rz(0.3, 0).h(0);

    // Act: apply pass
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(merged_in));

    // Assert: structural — 3 gates collapsed to 1
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());

    const auto& ops = out.ops();
    // The surviving gate must be U3.
    EXPECT_TRUE(std::get<GateOp>(ops[0]).type == GateType::U3);

    // Assert: quantum equivalence — overlap magnitude >= 1 - 1e-10.
    const StateVector sv_orig   = run_statevector(std::move(orig));
    const StateVector sv_merged = run_statevector(std::move(out));
    const double overlap = inner_product_magnitude(sv_orig, sv_merged);
    EXPECT_NEAR(overlap, 1.0, 1e-10);  // global phase ignored
}

// ── Case 2: RZ·RZ pair merged to single U3 (two consecutive rotations) ──────

TEST(MergeSingleQubitUnitaryPass, RzRz_MergedToU3) {
    // Arrange
    const double a1 = 0.5, a2 = 1.2;
    Circuit orig(1);
    orig.rz(a1, 0).rz(a2, 0);

    Circuit merged_in(1);
    merged_in.rz(a1, 0).rz(a2, 0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(merged_in));

    // Assert: 2 gates -> 1 gate.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());

    // Quantum equivalence.
    const StateVector sv_orig   = run_statevector(std::move(orig));
    const StateVector sv_merged = run_statevector(std::move(out));
    EXPECT_NEAR(inner_product_magnitude(sv_orig, sv_merged), 1.0, 1e-10);
}

// ── Case 3: RX·RY·RZ merged to single U3 ────────────────────────────────────

TEST(MergeSingleQubitUnitaryPass, RxRyRz_MergedToSingleU3) {
    // Arrange: three different rotation gates in sequence.
    const double a1 = 0.3, a2 = 0.7, a3 = 1.1;
    Circuit orig(1);
    orig.rx(a1, 0).ry(a2, 0).rz(a3, 0);

    Circuit merged_in(1);
    merged_in.rx(a1, 0).ry(a2, 0).rz(a3, 0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(merged_in));

    // Assert: 3 gates -> 1 gate.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());

    // Quantum equivalence.
    const StateVector sv_orig   = run_statevector(std::move(orig));
    const StateVector sv_merged = run_statevector(std::move(out));
    EXPECT_NEAR(inner_product_magnitude(sv_orig, sv_merged), 1.0, 1e-10);
}

// ── Case 4: Mixed Clifford+rotation run (H,RZ,X) merged to U3 ───────────────

TEST(MergeSingleQubitUnitaryPass, HRzX_MixedRunMergedToU3) {
    // Arrange
    Circuit orig(1);
    orig.h(0).rz(0.9, 0).x(0);

    Circuit merged_in(1);
    merged_in.h(0).rz(0.9, 0).x(0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(merged_in));

    // Assert: 3 gates -> 1 gate.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::U3);

    // Quantum equivalence.
    const StateVector sv_orig   = run_statevector(std::move(orig));
    const StateVector sv_merged = run_statevector(std::move(out));
    EXPECT_NEAR(inner_product_magnitude(sv_orig, sv_merged), 1.0, 1e-10);
}

// ── Case 5: CX gate breaks window — H on q0 before/after CX not merged ───────

TEST(MergeSingleQubitUnitaryPass, CXGate_BreaksWindowForBothQubits) {
    // Arrange: h(0) [cx(0,1)] h(0) -> not merged because CX separates them.
    Circuit c(2);
    c.h(0).cx(0, 1).h(0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert: 3 ops remain (h, cx, h — no merge across cx).
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 6: Barrier breaks window ────────────────────────────────────────────

TEST(MergeSingleQubitUnitaryPass, Barrier_BreaksRun) {
    // Arrange: two separate RZ ops separated by a barrier.
    Circuit c(1);
    c.rz(0.3, 0).barrier({0}).rz(0.5, 0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert: 3 ops: rz, barrier, rz — no merge across barrier.
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 7: Measure breaks window ────────────────────────────────────────────

TEST(MergeSingleQubitUnitaryPass, Measure_BreaksRun) {
    // Arrange
    Circuit c(1, 1);
    c.rz(0.3, 0).measure(0, 0).rz(0.5, 0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert: 3 ops remain — measure breaks the run.
    EXPECT_EQ(out.num_ops(), std::size_t{3});
    EXPECT_FALSE(pass.changed());
}

// ── Case 8: Single gate is not merged ────────────────────────────────────────

TEST(MergeSingleQubitUnitaryPass, SingleGate_NotMerged) {
    // Arrange
    Circuit c(1);
    c.rz(0.7, 0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert: 1 op remains unchanged (run length < 2).
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_FALSE(pass.changed());
    // Gate type preserved.
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::RZ);
}

// ── Case 9: Self-inverse run (H·H) — composed to near-identity → dropped ─────

TEST(MergeSingleQubitUnitaryPass, HH_ComposesToIdentity_Dropped) {
    // H*H = I, so the run should be dropped entirely.
    Circuit c(1);
    c.h(0).h(0);

    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(c));

    // H*H = Identity: the run should produce 0 ops.
    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 10: Two qubits — independent runs on each qubit merged separately ───

TEST(MergeSingleQubitUnitaryPass, TwoQubits_IndependentRunsMerged) {
    // Arrange: rz+rz on qubit 0, h+x on qubit 1 (interleaved).
    Circuit orig(2);
    orig.rz(0.4, 0).h(1).rz(0.8, 0).x(1);

    Circuit merged_in(2);
    merged_in.rz(0.4, 0).h(1).rz(0.8, 0).x(1);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(merged_in));

    // 4 gates: two runs of 2 each -> 2 U3 gates.
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_TRUE(pass.changed());

    // Quantum equivalence.
    const StateVector sv_orig   = run_statevector(std::move(orig));
    const StateVector sv_merged = run_statevector(std::move(out));
    EXPECT_NEAR(inner_product_magnitude(sv_orig, sv_merged), 1.0, 1e-10);
}

// ── Case 11: U3 params have 3 elements ───────────────────────────────────────

TEST(MergeSingleQubitUnitaryPass, U3Output_HasThreeParams) {
    // Any merged run must produce U3 with exactly 3 parameters.
    Circuit c(1);
    c.rz(0.3, 0).rx(0.7, 0);

    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(c));

    EXPECT_EQ(out.num_ops(), std::size_t{1});
    const GateOp& g = std::get<GateOp>(out.ops()[0]);
    EXPECT_TRUE(g.type == GateType::U3);
    EXPECT_EQ(g.params.size(), std::size_t{3});
}

// ── Case 12: name() returns the expected string ───────────────────────────────

TEST(MergeSingleQubitUnitaryPass, Name_ReturnsExpectedString) {
    MergeSingleQubitUnitaryPass pass;
    EXPECT_EQ(pass.name(), std::string{"MergeSingleQubitUnitaryPass"});
}

// ── Case 13: Long run (5 rotations) merged correctly ─────────────────────────

TEST(MergeSingleQubitUnitaryPass, LongRotationRun_MergedToSingleU3) {
    // Arrange: 5 consecutive rotations on one qubit.
    Circuit orig(1);
    orig.rz(0.1, 0).rx(0.2, 0).ry(0.3, 0).rz(0.4, 0).rx(0.5, 0);

    Circuit merged_in(1);
    merged_in.rz(0.1, 0).rx(0.2, 0).ry(0.3, 0).rz(0.4, 0).rx(0.5, 0);

    // Act
    MergeSingleQubitUnitaryPass pass;
    Circuit out = pass.run(std::move(merged_in));

    // Assert: 5 gates -> 1 gate.
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());

    // Quantum equivalence.
    const StateVector sv_orig   = run_statevector(std::move(orig));
    const StateVector sv_merged = run_statevector(std::move(out));
    EXPECT_NEAR(inner_product_magnitude(sv_orig, sv_merged), 1.0, 1e-10);
}
