// test_qubit_truncation.cpp — QubitTruncationPass.
// Verifies sparse physical qubit sets are compacted to a dense [0,k) range with
// num_qubits reduced, that the dense->physical mapping is stamped into metadata,
// that measurement cubits (user-perspective counts) are preserved, and that
// already-dense circuits are a no-op.,.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <variant>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/noise/noise_model.hpp"
#include "../../../../include/qregister.hpp"
#include "../../../../include/transpiler/passes/optimization/qubit_truncation_pass.hpp"
#include "../../../../include/types.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::MeasureOp;
using qplayer::NoiseModel;
using qplayer::Op;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::QubitTruncationPass;
using qplayer::SimType;

namespace {

// Collect the qubit operands referenced by every GateOp/MeasureOp in a circuit.
std::vector<QubitIndex> referenced_qubits(const Circuit& c) {
    std::vector<QubitIndex> qs;
    for (const Op& op : c.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op))
            for (QubitIndex q : g->qubits) qs.push_back(q);
        else if (const auto* m = std::get_if<MeasureOp>(&op))
            qs.push_back(m->qubit);
    }
    return qs;
}

}  // namespace

// ── Already-dense circuit -> no-op ────────────────────────────────────────────
TEST(QubitTruncation, DenseCircuit_IsNoOp) {
    Circuit c(3);
    c.h(0).cx(0, 1).x(2);

    QubitTruncationPass pass;
    Circuit out = pass.run(c);

    EXPECT_FALSE(pass.changed());
    EXPECT_EQ(out.num_qubits(), QubitIndex{3});
    EXPECT_FALSE(out.metadata().qubit_truncation.has_value());
}

// ── Sparse physical qubits -> compacted to dense [0, k) ───────────────────────
TEST(QubitTruncation, SparseCircuit_CompactedToDense) {
    // 8-qubit register but only qubits {2, 5, 7} are used.
    Circuit c(8);
    c.h(2).cx(2, 5).x(7);

    QubitTruncationPass pass;
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_EQ(out.num_qubits(), QubitIndex{3});   // 3 distinct qubits
    // Every referenced qubit must now be within [0, 3).
    for (QubitIndex q : referenced_qubits(out)) EXPECT_TRUE(q < QubitIndex{3});
}

// ── Dense->physical mapping is stamped into metadata (for noise handoff) ──────
TEST(QubitTruncation, TruncationMapping_StoredInMetadata) {
    Circuit c(8);
    c.h(2).cx(2, 5).x(7);          // used {2,5,7} ascending -> dense {0,1,2}

    QubitTruncationPass pass;
    Circuit out = pass.run(c);

    EXPECT_TRUE(out.metadata().qubit_truncation.has_value());
    if (out.metadata().qubit_truncation.has_value()) {
        const auto& map = *out.metadata().qubit_truncation;
        EXPECT_EQ(map.size(), std::size_t{3});
        EXPECT_EQ(map.at(0), QubitIndex{2});
        EXPECT_EQ(map.at(1), QubitIndex{5});
        EXPECT_EQ(map.at(2), QubitIndex{7});
    }
}

// ── Measure cubit preserved, only the qubit operand is relabelled ─────────────
TEST(QubitTruncation, MeasureCubitPreserved_QubitRemapped) {
    Circuit c(8, 3);
    c.x(2).x(7);
    c.measure(2, 0).measure(5, 1).measure(7, 2);   // qubit 5 used only via measure

    QubitTruncationPass pass;
    Circuit out = pass.run(c);

    // Used set {2,5,7} -> dense {0,1,2}. Measures keep their cubits, remap qubits.
    std::map<QubitIndex, QubitIndex> qubit_of_cubit;  // cubit -> remapped qubit
    for (const Op& op : out.ops())
        if (const auto* m = std::get_if<MeasureOp>(&op))
            qubit_of_cubit[m->cubit] = m->qubit;

    EXPECT_EQ(qubit_of_cubit.at(0), QubitIndex{0});   // was qubit 2
    EXPECT_EQ(qubit_of_cubit.at(1), QubitIndex{1});   // was qubit 5
    EXPECT_EQ(qubit_of_cubit.at(2), QubitIndex{2});   // was qubit 7
}

// ── Counts are identical before and after truncation (user-perspective) ───────
TEST(QubitTruncation, DeterministicCounts_PreservedAcrossTruncation) {
    // Deterministic outcome: q2=1, q5=0, q7=1 -> cubits (0,1,2) = (1,0,1).
    Circuit c(8, 3);
    c.x(2).x(7);
    c.measure(2, 0).measure(5, 1).measure(7, 2);

    QubitTruncationPass pass;
    Circuit out = pass.run(c);
    EXPECT_TRUE(pass.changed());

    QRegister qr_orig(SimType::FullState);
    QRegister qr_trunc(SimType::FullState);
    const auto counts_orig  = qr_orig.run(c, 256).counts();
    const auto counts_trunc = qr_trunc.run(out, 256).counts();

    // Same single bitstring with all shots; truncation must not change it.
    EXPECT_TRUE(counts_orig == counts_trunc);   // std::map has ==, but no operator<<
    EXPECT_EQ(counts_trunc.size(), std::size_t{1});
    EXPECT_EQ(counts_trunc.begin()->second, std::uint64_t{256});
}

// ── Superposition counts distribution preserved (same seed) ───────────────────
TEST(QubitTruncation, BellCounts_PreservedAcrossTruncation) {
    Circuit c(6, 2);
    c.h(1).cx(1, 4);                 // Bell on sparse qubits {1,4}
    c.measure(1, 0).measure(4, 1);

    QubitTruncationPass pass;
    Circuit out = pass.run(c);

    QRegister qr_orig(SimType::FullState);
    QRegister qr_trunc(SimType::FullState);
    qr_orig.set_seed(1234);
    qr_trunc.set_seed(1234);
    const auto co = qr_orig.run(c, 4096).counts();
    const auto ct = qr_trunc.run(out, 4096).counts();

    // Only "00" and "11" appear (Bell), and with a shared seed the sampling
    // stream is identical, so the counts maps match exactly.
    EXPECT_TRUE(co == ct);
}

// ── run-path noise handoff: dense operand -> correct physical qubit's noise ───
// After truncation the circuit runs in dense space, but the noise model is
// keyed by physical qubit. QRegister::run() must project the model onto the
// dense mapping so a per-qubit readout error binds
// to the physical qubit it was defined on — not the dense index that happens to
// share its number. Without the projection the flip would silently vanish.
TEST(QubitTruncation, RunPath_NoiseBindsToPhysicalQubit) {
    // physical 7 prepared |1>; measures on scattered {2,5,7} -> dense {0,1,2}.
    Circuit c(8, 3);
    c.x(7);
    c.measure(2, 0).measure(5, 1).measure(7, 2);   // cubit 2 reads physical 7

    QubitTruncationPass pass;
    Circuit out = pass.run(c);
    EXPECT_TRUE(pass.changed());

    // Baseline: no noise -> physical 7 (dense 2) reads its true outcome 1.
    QRegister qr_base(SimType::FullState);
    const auto counts_base = qr_base.run(out, 256).counts();

    // Readout flip only on physical qubit 7: P(report 0 | true 1) = 1.0. With a
    // correct dense projection this lands on dense qubit 2 and always flips its
    // reported bit, changing the deterministic outcome. A physical-2 lookup
    // (the bug) would find no error and reproduce the baseline.
    NoiseModel nm;
    nm.add_readout_error(7, /*p01=*/1.0, /*p10=*/0.0);
    QRegister qr_noisy(SimType::FullState);
    qr_noisy.set_noise_model(nm);
    const auto counts_noisy = qr_noisy.run(out, 256).counts();

    EXPECT_EQ(counts_base.size(),  std::size_t{1});   // both deterministic
    EXPECT_EQ(counts_noisy.size(), std::size_t{1});
    EXPECT_FALSE(counts_base == counts_noisy);         // flip must take effect
}
