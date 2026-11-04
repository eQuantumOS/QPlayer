// test_tn_3q.cpp — three-qubit gates on TensorNetwork.
// 1.1, 1.3.
//
// TN used to reject ccx / cswap outright while FullState and SparseState ran
// them through dedicated kernels, so a gate the parser accepts as first class
// failed on one backend. TN now runs a FIXED decomposition
// inside MpsState::apply_3q().
//
// The decomposition is where this can go wrong, and it already did once in this
// codebase: the GPU path decomposed CCX into 15 H/T/CX gates and dropped one T,
// leaving the target in superposition when both controls were set. That defect
// is only visible in AMPLITUDES, not in a distribution shape, so the tests here
// compare against FullState CPU amplitude by amplitude and additionally require
// bit-identical counts at a fixed seed (C draw convention).
//
// Tolerance: 1e-10 is the gate. Observed is ~1e-15 (double rounding
// over ~15 composed gates), so the gate has four orders of headroom -- it fails
// on a wrong decomposition, not on rounding.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <complex>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/error.hpp"
#include "../../../include/circuit/gate_op.hpp"

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

std::vector<std::complex<double>> statevector_on(SimType sim, const Circuit& circ) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(7);
    qr.run(circ, 1);
    return qr.get_statevector();
}

// Largest amplitude-wise deviation from FullState CPU, which is exact here.
double max_amp_delta(const Circuit& circ) {
    const auto a = statevector_on(SimType::FullState, circ);
    const auto b = statevector_on(SimType::TensorNetwork, circ);
    if (a.size() != b.size()) return 1.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = std::abs(a[i] - b[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

Counts counts_on(SimType sim, const Circuit& circ, std::uint32_t shots, std::uint64_t seed) {
    QRegister qr(sim, ProcType::CPU);
    qr.set_seed(seed);
    return qr.run(circ, shots).counts();
}

void measure_all(Circuit& c, QubitIndex n) {
    for (QubitIndex q = 0; q < n; ++q) c.measure(q, q);
}

GateOp body_of(GateType type, const std::string& name, std::vector<QubitIndex> qubits) {
    GateOp op;
    op.type   = type;
    op.name   = name;
    op.qubits = std::move(qubits);
    return op;
}

}  // namespace

// ── CCX correctness ─────────────────────────────────────────────────────────

TEST(TnThreeQubit, CCX_OnSuperposedControls_MatchesFullStateAmplitudes) {
    // Arrange — both controls in superposition is the case the GPU defect hid in:
    // a missing phase gate only shows once the |11> control branch is populated.
    Circuit c(3, 3);
    c.h(0).h(1).ccx(0, 1, 2);

    // Act
    const double delta = max_amp_delta(c);

    // Assert — 1e-10 is the equivalence gate; observed ~1e-15.
    EXPECT_NEAR(delta, 0.0, 1e-10);
}

TEST(TnThreeQubit, CCX_OperandPermutations_MatchFullStateAmplitudes) {
    // Arrange/Act/Assert — operand order is the other half of the convention.
    // A target-in-the-middle and a descending order both exercise the site
    // mapping differently from the ascending case above.
    Circuit mid(3, 3);
    mid.h(0).h(2).ccx(0, 2, 1);
    EXPECT_NEAR(max_amp_delta(mid), 0.0, 1e-10);

    Circuit down(3, 3);
    down.h(1).h(2).ccx(2, 1, 0);
    EXPECT_NEAR(max_amp_delta(down), 0.0, 1e-10);
}

TEST(TnThreeQubit, CCX_DistantOperands_MatchFullStateAmplitudes) {
    // Arrange — spread operands force the SWAP walk inside every decomposed CX,
    // which is where a mis-mirrored walk would leave the chain permuted.
    Circuit c(6, 6);
    c.h(0).h(2).ccx(0, 2, 5);

    // Act + Assert
    EXPECT_NEAR(max_amp_delta(c), 0.0, 1e-10);
}

// ── CSwap correctness ───────────────────────────────────────────────────────

TEST(TnThreeQubit, CSwap_MatchesFullStateAmplitudes) {
    // Arrange — control in superposition, one target set: the swap must happen
    // only on the |1> control branch, so a wrong reduction to CCX is visible.
    Circuit c(3, 3);
    c.h(0).x(1).cswap(0, 1, 2);

    // Act + Assert
    EXPECT_NEAR(max_amp_delta(c), 0.0, 1e-10);
}

TEST(TnThreeQubit, CSwap_DistantOperands_MatchFullStateAmplitudes) {
    Circuit c(6, 6);
    c.h(0).x(1).cswap(0, 1, 5);
    EXPECT_NEAR(max_amp_delta(c), 0.0, 1e-10);
}

TEST(TnThreeQubit, MixedCircuit_WithRotationsAndBoth3QGates_MatchesFullState) {
    // Arrange — non-Clifford rotations plus both 3Q gates interleaved. A
    // permutation-only circuit cannot catch a phase error, which is exactly the
    // class of defect the GPU decomposition had.
    Circuit c(4, 4);
    c.rx(0.7, 0).ry(1.1, 1).h(2).ccx(0, 1, 2).cswap(2, 3, 0).t(1).ccx(3, 2, 1);

    // Act + Assert
    EXPECT_NEAR(max_amp_delta(c), 0.0, 1e-10);
}

// ── Sampling convention ─────────────────────────────────────────────────────

TEST(TnThreeQubit, CCX_Counts_BitIdenticalToFullState) {
    // Arrange — same seed, same draw convention (C), so the comparison
    // is exact rather than statistical.
    Circuit c(3, 3);
    c.h(0).h(1).ccx(0, 1, 2);
    measure_all(c, 3);

    // Act
    const Counts fs = counts_on(SimType::FullState, c, 512, 20260903);
    const Counts tn = counts_on(SimType::TensorNetwork, c, 512, 20260903);

    // Assert
    EXPECT_TRUE(fs == tn);
}

// ── Truncation ledger (folding) ────────────────────────────────────

TEST(TnThreeQubit, TruncationLedger_HasOneEntryPerUserGate) {
    // Arrange — one 2Q gate and two 3Q gates. Each 3Q gate expands into six CX,
    // and every distant CX adds a SWAP walk, so an unfolded ledger would hold
    // well over a dozen entries instead of three (B).
    Circuit c(6, 6);
    c.h(0).h(2).ccx(0, 2, 5).cx(1, 3).cswap(0, 1, 4);

    // Act
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_seed(7);
    const auto r = qr.run(c, 1);

    // Assert — 1 (cx) + 1 (ccx) + 1 (cswap).
    EXPECT_EQ(r.truncation_errors().size(), static_cast<std::size_t>(3));
}

// ── Noise path ──────────────────────────────────────────────────────────────

TEST(TnThreeQubit, CCX_UnderNoise_RunsOnTrajectoryPath) {
    // Arrange — the Monte Carlo path has its own dispatch, so it needs its own
    // coverage: the pre-flight rejection and the apply site are different code
    // from the noiseless stream.
    Circuit c(3, 3);
    c.h(0).h(1).ccx(0, 1, 2);
    measure_all(c, 3);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.01);

    // Act + Assert — correctness under noise is a distribution question the
    // parity suites own; here the claim is only that it is no longer rejected.
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_seed(7);
    qr.set_noise_model(nm);
    EXPECT_NO_THROW(qr.run(c, 32));
}

// ── Remaining rejections stay rejections ────────────────────────────────────

TEST(TnThreeQubit, ConditionalThreeQubitBody_StillRejected) {
    // Arrange — an IfOp step carries ONE resolved matrix, not a
    // sequence, so a 3Q body is a deliberate scope limit rather than an
    // oversight (result).
    Circuit c(4, 4);
    c.h(0).measure(0, 0);
    c.if_then(0, 1, body_of(GateType::CCX, "ccx", {1, 2, 3}));

    // Act + Assert
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    EXPECT_THROW(qr.run(c, 4), SimulatorError);
}

TEST(TnThreeQubit, MpoPath_ThreeQubitGate_Rejected) {
    // Arrange — MpoState exposes 1Q/2Q unitaries only, so is MPS-only.
    // The rejection must be explicit, not an arity complaint from a 64-entry
    // matrix reaching apply_unitary_2q.
    Circuit c(3, 3);
    c.h(0).h(1).ccx(0, 1, 2);
    measure_all(c, 3);
    NoiseModel nm;
    nm.add_depolarizing("h", 0.01);

    // Act + Assert
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    qr.set_noise_model(nm);
    qr.set_tn_noise_path("mpo");
    EXPECT_THROW(qr.run(c, 4), SimulatorError);
}
