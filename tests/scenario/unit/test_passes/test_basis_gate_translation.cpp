// test_basis_gate_translation.cpp — BasisGateTranslationPass (,
// / /). Verifies logical gates
// decompose into the device native basis {id, rz, sx, x, ecr} (Eagle/ECR-native)
// and {id, rz, sx, x, cz} (Willow/CZ-native), and that each decomposition is
// unitarily equivalent (up to global phase) to the original gate, checked via the
// FullState CPU statevector to 1e-10.,.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <string>
#include <variant>
#include <vector>

#include "../../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/error.hpp"
#include "../../../../include/noise/device_model.hpp"
#include "../../../../include/transpiler/passes/translation/basis_gate_translation_pass.hpp"
#include "../../../../include/types.hpp"

using qplayer::BasisGateTranslationPass;
using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::DeviceModel;
using qplayer::FullStateCpuBackend;
using qplayer::GateOp;
using qplayer::Op;
using qplayer::QubitIndex;
using qplayer::StateVector;

namespace {

// Eagle-like native basis: single-qubit {id, rz, sx, x} + ecr as the 2Q gate.
DeviceModel eagle_device() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e9, "T2": 1e9}, {"T1": 1e9, "T2": 1e9}, {"T1": 1e9, "T2": 1e9}],
      "basis_gates": ["id", "rz", "sx", "x", "ecr"],
      "two_qubit_gate": "ecr"
    })");
}

// Willow-like native basis: single-qubit {id, rz, sx, x} + cz as the 2Q gate
// (Google Willow square-lattice).
DeviceModel willow_device() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e9, "T2": 1e9}, {"T1": 1e9, "T2": 1e9}, {"T1": 1e9, "T2": 1e9}],
      "basis_gates": ["id", "rz", "sx", "x", "cz"],
      "two_qubit_gate": "cz"
    })");
}

// FullState statevector of a circuit's unitary action on |0...0> (GateOps only).
StateVector sv_of(const Circuit& c) {
    FullStateCpuBackend b;
    b.init_state(c.num_qubits());
    for (const Op& op : c.ops())
        if (std::holds_alternative<GateOp>(op))
            b.apply_gate(std::get<GateOp>(op));
    return b.get_statevector();
}

// Two statevectors equal up to an unobservable global phase (translation drops
// global phase; rz is virtual). Anchor on the first sizeable amplitude.
bool equal_up_to_phase(const StateVector& a, const StateVector& b, double tol) {
    if (a.size() != b.size()) return false;
    std::complex<double> phase{1, 0};
    bool anchored = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!anchored && std::abs(b[i]) > 1e-6) {
            phase = a[i] / b[i];
            anchored = true;
        }
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::complex<double> d = a[i] - phase * b[i];
        if (std::abs(d) > tol) return false;
    }
    return true;
}

// Every GateOp in the circuit must be one of the Eagle native basis gates.
bool all_native(const Circuit& c) {
    for (const Op& op : c.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            const std::string& n = g->name;
            if (n != "id" && n != "rz" && n != "sx" && n != "x" && n != "ecr")
                return false;
        }
    }
    return true;
}

// Every GateOp in the circuit must be one of the Willow native basis gates
//
bool willow_all_native(const Circuit& c) {
    for (const Op& op : c.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            const std::string& n = g->name;
            if (n != "id" && n != "rz" && n != "sx" && n != "x" && n != "cz")
                return false;
        }
    }
    return true;
}

}  // namespace

// ── Native gates pass through unchanged ───────────────────────────────────────
TEST(BasisGateTranslation, NativeGates_PassThroughUnchanged) {
    Circuit c(2);
    c.rz(0.5, 0).sx(0).x(1).ecr(0, 1);

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_FALSE(pass.changed());
    EXPECT_EQ(out.num_ops(), std::size_t{4});
    EXPECT_TRUE(all_native(out));
}

// ── No basis_gates -> pass is a no-op (cannot know the target basis) ───────────
TEST(BasisGateTranslation, NoBasisGates_IsNoOp) {
    DeviceModel dm = DeviceModel::from_json_string(R"({"qubits": [{"T1": 1.0, "T2": 1.0}]})");
    Circuit c(1);
    c.h(0);

    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_FALSE(pass.changed());
    EXPECT_EQ(out.num_ops(), std::size_t{1});   // still an 'h', untouched
}

// ── h decomposes to native basis and preserves the unitary ────────────────────
TEST(BasisGateTranslation, Hadamard_DecomposesEquivalently) {
    Circuit c(1);
    c.h(0);
    const StateVector before = sv_of(c);

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(all_native(out));
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));
}

// ── cx decomposes to an ecr-based template (Bell state preserved) ─────────────
TEST(BasisGateTranslation, Cx_DecomposesToEcrEquivalently) {
    Circuit c(2);
    c.h(0).cx(0, 1);                 // Bell
    const StateVector before = sv_of(c);

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(all_native(out));
    // The decomposition must actually use ECR (the native entangler).
    bool has_ecr = false;
    for (const Op& op : out.ops())
        if (const auto* g = std::get_if<GateOp>(&op))
            if (g->name == "ecr") has_ecr = true;
    EXPECT_TRUE(has_ecr);
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));
}

// ── swap decomposes (via cx×3 -> ecr) and preserves the permutation ───────────
TEST(BasisGateTranslation, Swap_DecomposesEquivalently) {
    Circuit c(2);
    c.x(0).swap(0, 1);               // |01> -> |10>
    const StateVector before = sv_of(c);

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(all_native(out));
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));
}

// ── Arbitrary 1Q (u3) decomposes via ZSX Euler equivalently ───────────────────
TEST(BasisGateTranslation, ArbitraryU3_EulerEquivalent) {
    Circuit c(1);
    c.u3(1.9, -0.4, 0.8, 0);
    const StateVector before = sv_of(c);

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(all_native(out));
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));
}

// ── T gate (non-Clifford phase) decomposes equivalently ───────────────────────
TEST(BasisGateTranslation, TGate_EulerEquivalent) {
    Circuit c(1);
    c.h(0).t(0);                     // exercises a non-trivial phase in superposition
    const StateVector before = sv_of(c);

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(all_native(out));
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));
}

// ── Mixed circuit: full-pipeline equivalence to 1e-10 (acceptance) ─────
TEST(BasisGateTranslation, MixedCircuit_EquivalentAndAllNative) {
    Circuit c(3);
    c.h(0).cx(0, 1).t(1).rx(0.9, 2).swap(1, 2).cx(2, 0).sx(0);
    const StateVector before = sv_of(c);

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(all_native(out));
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));
}

// ── Arbitrary 2Q gate with no rule -> CircuitError (KAK deferred) ─────
TEST(BasisGateTranslation, ArbitraryTwoQubit_ThrowsCircuitError) {
    Circuit c(2);
    c.cz(0, 1);                      // not cx/swap, not native -> rejected

    const DeviceModel dm = eagle_device();  // must outlive the pass (holds const DeviceModel*)
    BasisGateTranslationPass pass(dm);
    EXPECT_THROW(pass.run(c), CircuitError);
}

// ── cx-native device: cx passes through, ecr is rejected ──────────────────────
TEST(BasisGateTranslation, CxNativeDevice_PassesThroughCx) {
    DeviceModel dm = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e9, "T2": 1e9}, {"T1": 1e9, "T2": 1e9}],
      "basis_gates": ["id", "rz", "sx", "x", "cx"],
      "two_qubit_gate": "cx"
    })");
    Circuit c(2);
    c.h(0).cx(0, 1);
    const StateVector before = sv_of(c);

    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    // h decomposes, cx stays. No ecr should appear on a cx-native device.
    for (const Op& op : out.ops())
        if (const auto* g = std::get_if<GateOp>(&op))
            EXPECT_NE(g->name, std::string{"ecr"});
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));
}

// ─────────────────────────────────────────────────────────────────────────────
// Willow / CZ-native device tests
// ─────────────────────────────────────────────────────────────────────────────

// ── cx decomposes to cz-based template (Bell state preserved) ─────────────────
TEST(BasisGateTranslation, CxCz_DecomposesToCzEquivalently) {
    Circuit c(2);
    c.h(0).cx(0, 1);  // Bell state preparation
    const StateVector before = sv_of(c);

    const DeviceModel dm = willow_device();
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(willow_all_native(out));
    // Decomposition must use the native CZ entangler.
    bool has_cz = false;
    for (const Op& op : out.ops())
        if (const auto* g = std::get_if<GateOp>(&op))
            if (g->name == "cz") has_cz = true;
    EXPECT_TRUE(has_cz);
    // Unitary equivalence up to global phase (ZSX Euler drops global phase).
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));  // 1e-10: exact math identity
}

// ── swap decomposes via cx×3 -> cz template, permutation preserved ────────────
TEST(BasisGateTranslation, Swap_DecomposesToCzEquivalently) {
    Circuit c(2);
    c.x(0).swap(0, 1);  // |01> -> |10>
    const StateVector before = sv_of(c);

    const DeviceModel dm = willow_device();
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(willow_all_native(out));
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));  // 1e-10: exact permutation
}

// ── native cz gate passes through unchanged (surface code circuits) ──
TEST(BasisGateTranslation, NativeCz_PassThroughUnchanged) {
    Circuit c(2);
    c.cz(0, 1);  // already in the Willow basis

    const DeviceModel dm = willow_device();
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    // cz is already native: changed_ must not be set and cz must remain.
    EXPECT_FALSE(pass.changed());
    bool has_cz = false;
    for (const Op& op : out.ops())
        if (const auto* g = std::get_if<GateOp>(&op))
            if (g->name == "cz") has_cz = true;
    EXPECT_TRUE(has_cz);
    EXPECT_TRUE(willow_all_native(out));
}

// ── fSim on CZ-native device throws CircuitError (no KAK rule) ─────────
TEST(BasisGateTranslation, Fsim_OnCzDevice_Throws) {
    Circuit c(2);
    c.fsim(M_PI / 2.0, M_PI / 6.0, 0, 1);  // Sycamore-style fSim, not native CZ

    const DeviceModel dm = willow_device();
    BasisGateTranslationPass pass(dm);
    // fSim is a 2Q gate with no decomposition rule for CZ-native basis.
    EXPECT_THROW(pass.run(c), CircuitError);
}

// ── h decomposes on Willow device via ZSX Euler, same as Eagle ────────────────
TEST(BasisGateTranslation, Hadamard_OnWillow_DecomposesEquivalently) {
    Circuit c(1);
    c.h(0);
    const StateVector before = sv_of(c);

    const DeviceModel dm = willow_device();
    BasisGateTranslationPass pass(dm);
    Circuit out = pass.run(c);

    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(willow_all_native(out));
    EXPECT_TRUE(equal_up_to_phase(before, sv_of(out), 1e-10));  // 1e-10: exact ZSX identity
}
