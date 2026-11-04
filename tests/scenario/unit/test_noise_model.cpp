// test_noise_model.cpp — Phase 8 unit tests for NoiseModel channel
// registration, query, and Kraus operator construction.
// Each built-in channel must produce trace-preserving Kraus
// operators; out-of-range parameters and non-trace-preserving raw channels
// must throw CircuitError. Also covers the Circuit::define_opaque(KrausChannel)
// trace-preserving gate.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/noise/device_model.hpp"
#include "../../../include/noise/noise_model.hpp"

using qplayer::ChannelType;
using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::DeviceModel;
using qplayer::KrausChannel;
using qplayer::NoiseModel;
using qplayer::QubitIndex;
using qplayer::UnitaryMatrix;

namespace {

// Verify sum_i K_i^† K_i ≈ I within tol — the completeness / trace-preserving
// condition every physical Kraus channel must satisfy.
bool is_trace_preserving(const std::vector<UnitaryMatrix>& ops, double tol) {
    if (ops.empty()) return false;
    const std::size_t dim = ops.front().size();
    for (std::size_t i = 0; i < dim; ++i) {
        for (std::size_t j = 0; j < dim; ++j) {
            std::complex<double> acc{0.0, 0.0};
            for (const auto& K : ops) {
                for (std::size_t k = 0; k < dim; ++k) {
                    acc += std::conj(K[k][i]) * K[k][j];
                }
            }
            const std::complex<double> target =
                (i == j) ? std::complex<double>{1.0, 0.0}
                         : std::complex<double>{0.0, 0.0};
            if (std::abs(acc - target) > tol) return false;
        }
    }
    return true;
}

}  // namespace

// ── Built-in channel construction ────────────────────────────────────────────

TEST(NoiseModel, Depolarizing_RegistersFourTracePreservingOperators) {
    // Arrange
    NoiseModel nm;

    // Act
    nm.add_depolarizing("h", 0.1);
    auto chans = nm.channels_for_gate("h", {0});

    // Assert
    EXPECT_EQ(chans.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(chans[0].operators.size(), static_cast<std::size_t>(4));
    EXPECT_EQ(static_cast<int>(chans[0].type),
              static_cast<int>(ChannelType::Depolarizing));
    EXPECT_TRUE(is_trace_preserving(chans[0].operators, 1e-10));
}

TEST(NoiseModel, AmplitudeDamping_OperatorsMatchFormula) {
    // Arrange
    NoiseModel nm;
    const double gamma = 0.3;

    // Act
    nm.add_amplitude_damping("x", gamma);
    auto chans = nm.channels_for_gate("x", {2});

    // Assert
    EXPECT_EQ(chans[0].operators.size(), static_cast<std::size_t>(2));
    // K0[1][1] = sqrt(1 - gamma); K1[0][1] = sqrt(gamma).
    EXPECT_NEAR(chans[0].operators[0][1][1].real(), std::sqrt(1.0 - gamma), 1e-12);
    EXPECT_NEAR(chans[0].operators[1][0][1].real(), std::sqrt(gamma), 1e-12);
    EXPECT_TRUE(is_trace_preserving(chans[0].operators, 1e-10));
}

TEST(NoiseModel, PhaseDamping_IsTracePreserving) {
    // Arrange
    NoiseModel nm;

    // Act
    nm.add_phase_damping("cx", 0.25);
    auto chans = nm.channels_for_gate("cx", {0});

    // Assert
    EXPECT_TRUE(is_trace_preserving(chans[0].operators, 1e-10));
}

TEST(NoiseModel, BitFlipAndPhaseFlip_AreTracePreserving) {
    // Arrange
    NoiseModel nm;

    // Act
    nm.add_bit_flip("h", 0.2).add_phase_flip("z", 0.15);

    // Assert
    EXPECT_TRUE(is_trace_preserving(nm.channels_for_gate("h", {0})[0].operators, 1e-10));
    EXPECT_TRUE(is_trace_preserving(nm.channels_for_gate("z", {0})[0].operators, 1e-10));
}

// ── Parameter validation ─────────────────────────────────────────────────────

TEST(NoiseModel, Probability_AboveOne_ThrowsCircuitError) {
    NoiseModel nm;
    EXPECT_THROW(nm.add_depolarizing("h", 1.5), CircuitError);
}

TEST(NoiseModel, Probability_Negative_ThrowsCircuitError) {
    NoiseModel nm;
    EXPECT_THROW(nm.add_amplitude_damping("x", -0.1), CircuitError);
}

TEST(NoiseModel, ReadoutError_OutOfRange_ThrowsCircuitError) {
    NoiseModel nm;
    EXPECT_THROW(nm.add_readout_error(0, 0.05, 2.0), CircuitError);
}

TEST(NoiseModel, ReadoutError_Valid_DoesNotThrow) {
    NoiseModel nm;
    // Readout error is qubit-keyed, not gate-keyed: registration succeeds but
    // it does not appear under any gate name.
    EXPECT_NO_THROW(nm.add_readout_error(0, 0.02, 0.03));
    EXPECT_FALSE(nm.has_channels_for_gate("measure"));
}

// ── Raw Kraus injection ────────────────────────────────────────────

TEST(NoiseModel, AddKraus_TracePreserving_Accepted) {
    // Arrange — identity channel {I} is trivially trace-preserving.
    NoiseModel   nm;
    KrausChannel ch;
    ch.operators = {{{{1.0, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {1.0, 0.0}}}};

    // Act / Assert
    EXPECT_NO_THROW(nm.add_kraus("custom", ch));
    EXPECT_TRUE(nm.has_channels_for_gate("custom"));
}

TEST(NoiseModel, AddKraus_NotTracePreserving_ThrowsCircuitError) {
    // Arrange — a lone scaled identity (0.5 I) violates sum K†K = I.
    NoiseModel   nm;
    KrausChannel ch;
    ch.operators = {{{{0.5, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {0.5, 0.0}}}};

    // Act / Assert
    EXPECT_THROW(nm.add_kraus("custom", ch), CircuitError);
}

// ── Query semantics ──────────────────────────────────────────────────────────

TEST(NoiseModel, ChannelsForGate_GlobalRule_AppliesToEveryOperand) {
    // Arrange — empty qubit filter means the channel hits every operand.
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.1);

    // Act — a 2-qubit gate instance on operands {1, 3}.
    auto chans = nm.channels_for_gate("cx", {1, 3});

    // Assert — one channel per operand, target_qubit bound in operand order.
    EXPECT_EQ(chans.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(chans[0].target_qubit, static_cast<QubitIndex>(1));
    EXPECT_EQ(chans[1].target_qubit, static_cast<QubitIndex>(3));
}

TEST(NoiseModel, ChannelsForGate_QubitSpecific_FiltersOperands) {
    // Arrange — restrict the channel to qubit 3 only.
    NoiseModel nm;
    nm.add_bit_flip("cx", 0.1, {3});

    // Act
    auto chans = nm.channels_for_gate("cx", {1, 3});

    // Assert — operand 1 is filtered out; only qubit 3 matches.
    EXPECT_EQ(chans.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(chans[0].target_qubit, static_cast<QubitIndex>(3));
}

TEST(NoiseModel, HasChannelsForGate_UnregisteredGate_ReturnsFalse) {
    NoiseModel nm;
    nm.add_depolarizing("h", 0.1);
    EXPECT_TRUE(nm.has_channels_for_gate("h"));
    EXPECT_FALSE(nm.has_channels_for_gate("x"));
    EXPECT_TRUE(nm.channels_for_gate("x", {0}).empty());
}

TEST(NoiseModel, AddMethods_AreChainable) {
    // Arrange
    NoiseModel nm;

    // Act — every add_* returns NoiseModel& so calls chain.
    NoiseModel& ref = nm.add_depolarizing("h", 0.1).add_phase_flip("z", 0.05);

    // Assert
    EXPECT_TRUE(&ref == &nm);
    EXPECT_TRUE(nm.has_t1_t2() == false);
}

// ── Circuit::define_opaque(KrausChannel) trace-preserving gate ─────

TEST(NoiseModel, DefineOpaqueKraus_TracePreserving_Accepted) {
    // Arrange — bit-flip channel {sqrt(0.9) I, sqrt(0.1) X} is trace-preserving.
    Circuit      circ(1, 0);
    const double a = std::sqrt(0.9);
    const double b = std::sqrt(0.1);
    KrausChannel ch;
    ch.operators = {
        {{{a, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {a, 0.0}}},
        {{{0.0, 0.0}, {b, 0.0}}, {{b, 0.0}, {0.0, 0.0}}},
    };

    // Act / Assert
    EXPECT_NO_THROW(circ.define_opaque("bitflip_chan", ch));
}

TEST(NoiseModel, DefineOpaqueKraus_NotTracePreserving_ThrowsCircuitError) {
    // Arrange — 0.5 I alone is not trace-preserving.
    Circuit      circ(1, 0);
    KrausChannel ch;
    ch.operators = {{{{0.5, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {0.5, 0.0}}}};

    // Act / Assert
    EXPECT_THROW(circ.define_opaque("bad_chan", ch), CircuitError);
}

// ── load_device: T1/T2 -> relaxation Kraus channels ───────────

TEST(NoiseModel, LoadDevice_RegistersRelaxationChannels) {
    // Arrange — device with finite coherence + a described "h" gate duration.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {"h": {"duration_ns": 300.0}}
    })");
    NoiseModel nm;

    // Act
    NoiseModel& ref = nm.load_device(dev);

    // Assert — has_t1_t2() now true; "h" carries amplitude+dephasing relaxation
    // on qubit 0 (T2 < 2 T1 -> both channels present).
    EXPECT_TRUE(&ref == &nm);
    EXPECT_TRUE(nm.has_t1_t2());
    auto chans = nm.channels_for_gate("h", {0});
    EXPECT_EQ(chans.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(chans[0].target_qubit, static_cast<QubitIndex>(0));
}

// ── : basis-aware synthesis — excess depolarizing + readout ──

namespace {
// True when a Depolarizing channel is present among the gate's channels.
bool has_depolarizing(const std::vector<qplayer::KrausChannel>& chans) {
    for (const auto& ch : chans)
        if (ch.type == ChannelType::Depolarizing) return true;
    return false;
}
// The depolarizing channel's p parameter (1 - |K0[0][0]|^2). Precondition:
// has_depolarizing(chans) is true.
double depol_param(const std::vector<qplayer::KrausChannel>& chans) {
    for (const auto& ch : chans)
        if (ch.type == ChannelType::Depolarizing) {
            const double k00 = ch.operators[0][0][0].real();  // sqrt(1-p)
            return 1.0 - k00 * k00;
        }
    return 0.0;
}
}  // namespace

TEST(NoiseModel, LoadDevice_OneQubitGate_AddsExcessDepolarizing) {
    // Infinite coherence -> relaxation floor 0, so the whole gate_error becomes
    // excess depolarizing. QPlayer depolarizing_ops uses r = p*d/(d+1), so for
    // d=2 the param is p = 1.5 * error_rate (corrected inversion).
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "basis_gates": ["sx", "rz", "x", "ecr"],
      "gates": {"sx_0": {"duration_ns": 35.0, "error_rate": 0.0006}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    auto chans = nm.channels_for_gate("sx", {0});
    EXPECT_TRUE(has_depolarizing(chans));
    EXPECT_NEAR(depol_param(chans), 1.5 * 0.0006, 1e-9);   // p = r_excess * (d+1)/d
}

TEST(NoiseModel, LoadDevice_ExcessDepolClampedWhenBelowRelaxation) {
    // Strong relaxation (short T1/T2, long gate) with a tiny reported error_rate:
    // the relaxation floor exceeds gate_error, so excess depolarizing clamps to 0
    // (no Depolarizing channel; only relaxation channels remain).
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 10000.0, "T2": 8000.0}],
      "gates": {"sx_0": {"duration_ns": 500.0, "error_rate": 0.00001}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    auto chans = nm.channels_for_gate("sx", {0});
    EXPECT_FALSE(has_depolarizing(chans));          // excess clamped to 0
    EXPECT_TRUE(nm.has_t1_t2());                     // relaxation still present
}

TEST(NoiseModel, LoadDevice_TwoQubitGate_AddsExcessDepolarizing) {
    // 2Q gate: relaxation on both operands + one 4x4 excess depolarizing channel
    // bound to both operands.
    // error_rate 0.05 is well above the ~0.018 relaxation floor for these times,
    // so a positive excess depolarizing survives the clamp.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}, {"T1": 45000.0, "T2": 28000.0}],
      "gates": {"ecr_0_1": {"duration_ns": 660.0, "error_rate": 0.05}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    auto chans = nm.channels_for_gate("ecr", {0, 1});
    // Find the 4x4 depolarizing channel; verify it binds both operands.
    bool found_2q = false;
    for (const auto& ch : chans) {
        if (ch.type == ChannelType::Depolarizing && ch.operators.front().size() == 4) {
            found_2q = true;
            EXPECT_EQ(ch.target_qubit, static_cast<QubitIndex>(0));
            EXPECT_EQ(ch.target_qubit2, static_cast<QubitIndex>(1));
        }
    }
    EXPECT_TRUE(found_2q);
}

TEST(NoiseModel, LoadDevice_TwoQubitGate_ExcessDepolExactParam) {
    // Infinite coherence -> relaxation floor 0, so the full gate_error is excess.
    // For d=4 the inversion is p = 1.25 * error_rate (corrected).
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "basis_gates": ["sx", "rz", "x", "ecr"],
      "gates": {"ecr_0_1": {"duration_ns": 660.0, "error_rate": 0.008}}
    })");
    NoiseModel nm;
    nm.load_device(dev);

    auto chans = nm.channels_for_gate("ecr", {0, 1});
    double p = -1.0;
    for (const auto& ch : chans)
        if (ch.type == ChannelType::Depolarizing && ch.operators.front().size() == 4) {
            const double k00 = ch.operators[0][0][0].real();  // sqrt(1-p)
            p = 1.0 - k00 * k00;
        }
    EXPECT_NEAR(p, 1.25 * 0.008, 1e-9);   // p = r_excess * (d+1)/d, d=4
}

TEST(NoiseModel, LoadDevice_RegistersReadoutFromDevice) {
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [
        {"T1": 50000.0, "T2": 30000.0, "prob_meas0_prep1": 0.02, "prob_meas1_prep0": 0.03},
        {"T1": 45000.0, "T2": 28000.0}
      ]
    })");
    NoiseModel nm;
    nm.load_device(dev);

    EXPECT_TRUE(nm.has_readout_errors());
    const auto ro0 = nm.readout_error(0);
    EXPECT_NEAR(ro0.first, 0.02, 1e-12);
    EXPECT_NEAR(ro0.second, 0.03, 1e-12);
    const auto ro1 = nm.readout_error(1);        // no readout fields -> {0,0}
    EXPECT_NEAR(ro1.first, 0.0, 1e-12);
    EXPECT_NEAR(ro1.second, 0.0, 1e-12);
}

TEST(NoiseModel, LoadDevice_NoReadout_LeavesReadoutErrorsEmpty) {
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}]
    })");
    NoiseModel nm;
    nm.load_device(dev);
    EXPECT_FALSE(nm.has_readout_errors());
}

TEST(NoiseModel, LoadDevice_InfiniteCoherence_RegistersNothing) {
    // Arrange — no qubit coherence data -> infinite T1/T2 -> no relaxation. The
    // bare "h" key carries no arity, so no depolarizing is synthesised either.
    DeviceModel dev = DeviceModel::from_json_string(
        R"({"gates": {"h": {"duration_ns": 300.0}}})");
    NoiseModel nm;

    // Act
    nm.load_device(dev);

    // Assert
    EXPECT_FALSE(nm.has_t1_t2());
    EXPECT_FALSE(nm.has_channels_for_gate("h"));
}

// ── project_to_dense: truncation handoff ──────────
// After QubitTruncationPass compacts scattered physical qubits into a dense
// [0,k) range, the physical-keyed noise model must be re-indexed so a backend
// executing dense operands still binds each qubit's real device parameters.

TEST(NoiseModel, ProjectToDense_ReadoutRekeyedByPhysicalQubit) {
    // Arrange — readout errors on physical qubits 2 and 7.
    NoiseModel nm;
    nm.add_readout_error(2, 0.10, 0.05);
    nm.add_readout_error(7, 0.20, 0.15);

    // Act — dense 0<-phys2, 1<-phys5, 2<-phys7 (the truncation mapping).
    NoiseModel proj = nm.project_to_dense({{0, 2}, {1, 5}, {2, 7}});

    // Assert — each dense qubit reports its physical qubit's readout error.
    EXPECT_TRUE(proj.has_readout_errors());
    EXPECT_NEAR(proj.readout_error(0).first,  0.10, 1e-12);  // was physical 2
    EXPECT_NEAR(proj.readout_error(0).second, 0.05, 1e-12);
    EXPECT_NEAR(proj.readout_error(2).first,  0.20, 1e-12);  // was physical 7
    EXPECT_NEAR(proj.readout_error(2).second, 0.15, 1e-12);
    // dense 1 <- physical 5, which had no readout error.
    EXPECT_NEAR(proj.readout_error(1).first,  0.0, 1e-12);
    EXPECT_NEAR(proj.readout_error(1).second, 0.0, 1e-12);
}

TEST(NoiseModel, ProjectToDense_QubitSpecificChannelRekeyed) {
    // Arrange — a depolarizing channel only on physical qubit 7.
    NoiseModel nm;
    nm.add_depolarizing("sx", 0.3, {7});

    // Act — physical 7 maps to dense 2.
    NoiseModel proj = nm.project_to_dense({{0, 2}, {1, 5}, {2, 7}});

    // Assert — the channel now answers to dense qubit 2, not dense 0 (phys 2).
    EXPECT_TRUE(proj.channels_for_gate("sx", {0}).empty());
    auto chans = proj.channels_for_gate("sx", {2});
    EXPECT_EQ(chans.size(), std::size_t{1});
    if (!chans.empty()) EXPECT_EQ(chans[0].target_qubit, QubitIndex{2});
}

TEST(NoiseModel, ProjectToDense_AbsentPhysicalChannelDropped) {
    // A channel on a physical qubit outside the truncated set is discarded.
    NoiseModel nm;
    nm.add_depolarizing("sx", 0.3, {9});   // physical 9 not in the mapping

    NoiseModel proj = nm.project_to_dense({{0, 2}, {1, 5}, {2, 7}});

    EXPECT_FALSE(proj.has_channels_for_gate("sx"));
}

TEST(NoiseModel, ProjectToDense_GlobalChannelPreserved) {
    // Global (empty-filter) channels apply to every operand -> copied verbatim.
    NoiseModel nm;
    nm.add_depolarizing("h", 0.1);

    NoiseModel proj = nm.project_to_dense({{0, 2}, {1, 5}});

    EXPECT_FALSE(proj.channels_for_gate("h", {0}).empty());
    EXPECT_FALSE(proj.channels_for_gate("h", {1}).empty());
}

TEST(NoiseModel, ProjectToDense_T1T2RekeyedByPhysicalQubit) {
    // Arrange — only physical qubit 2 has finite coherence; 5 is infinite.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [
        {}, {}, {"T1": 50000.0, "T2": 30000.0}, {}, {}, {}
      ],
      "gates": {"sx_2": {"duration_ns": 35.0, "error_rate": 0.001}}
    })");
    NoiseModel nm;
    nm.load_device(dev);
    EXPECT_TRUE(nm.has_t1_t2());

    // Act — dense 0<-phys2 (finite), dense 1<-phys5 (infinite).
    NoiseModel proj = nm.project_to_dense({{0, 2}, {1, 5}});

    // Assert — idle relaxation follows the physical qubit's coherence.
    EXPECT_TRUE(proj.has_t1_t2());
    EXPECT_FALSE(proj.idle_channels(0, 100.0).empty());   // phys2 had coherence
    EXPECT_TRUE(proj.idle_channels(1, 100.0).empty());    // phys5 infinite -> none
}

TEST(NoiseModel, ProjectToDense_IdentityMap_Equivalent) {
    // An identity mapping must leave the observable behaviour unchanged.
    NoiseModel nm;
    nm.add_readout_error(0, 0.1, 0.2);
    nm.add_readout_error(1, 0.3, 0.4);

    NoiseModel proj = nm.project_to_dense({{0, 0}, {1, 1}});

    EXPECT_NEAR(proj.readout_error(0).first,  0.1, 1e-12);
    EXPECT_NEAR(proj.readout_error(1).second, 0.4, 1e-12);
}

// ── (#1): 2Q relaxation bound to its exact edge + direction ─────────────

namespace {
// Count relaxation (single-qubit, 2x2-operator) channels targeting qubit q.
int count_relax_on(const std::vector<KrausChannel>& chans, QubitIndex q) {
    int n = 0;
    for (const auto& ch : chans)
        if (!ch.operators.empty() && ch.operators.front().size() == 2 &&
            ch.type != ChannelType::Depolarizing && ch.target_qubit == q)
            ++n;
    return n;
}
// Count two-qubit (4x4-operator) channels.
int count_2q(const std::vector<KrausChannel>& chans) {
    int n = 0;
    for (const auto& ch : chans)
        if (!ch.operators.empty() && ch.operators.front().size() == 4) ++n;
    return n;
}
// A heavy-hex-like fragment: qubit 1 sits on four directed edges, so the pre-fix
// per-qubit registration damped it four-fold on any incident cx.
DeviceModel shared_qubit_device() {
    return DeviceModel::from_json_string(R"({
      "qubits": [
        {"T1": 50000.0, "T2": 30000.0},
        {"T1": 50000.0, "T2": 30000.0},
        {"T1": 50000.0, "T2": 30000.0}
      ],
      "gates": {
        "cx_0_1": {"duration_ns": 300.0, "error_rate": 0.02},
        "cx_1_0": {"duration_ns": 300.0, "error_rate": 0.03},
        "cx_1_2": {"duration_ns": 300.0, "error_rate": 0.02},
        "cx_2_1": {"duration_ns": 300.0, "error_rate": 0.02}
      }
    })");
}
}  // namespace

TEST(NoiseModel, LoadDevice_2QRelaxation_NotDuplicatedAcrossSharedEdges) {
    // cx(0,1): each operand relaxes exactly once (amp + dephasing = 2 channels
    // per operand) from the cx_0_1 calibration alone; the reversed cx_1_0 and the
    // cx_1_2/cx_2_1 edges (which share qubit 1) must not pile on. Pre-fix this
    // produced relax(q1) four-fold plus a spurious second 2Q depol.
    NoiseModel nm;
    nm.load_device(shared_qubit_device());

    auto chans = nm.channels_for_gate("cx", {0, 1});
    EXPECT_EQ(count_relax_on(chans, 0), 2);   // amp + dephasing, once
    EXPECT_EQ(count_relax_on(chans, 1), 2);   // once, NOT once-per-incident-edge
    EXPECT_EQ(count_relax_on(chans, 2), 0);   // qubit 2 is not an operand
    EXPECT_EQ(count_2q(chans), 1);            // exactly one 2Q depolarizing
}

TEST(NoiseModel, LoadDevice_2QNoise_NotAppliedToNonNativeEdge) {
    // cx(0,2) is not a coupling edge; no relaxation or depol may attach to it.
    NoiseModel nm;
    nm.load_device(shared_qubit_device());
    EXPECT_TRUE(nm.channels_for_gate("cx", {0, 2}).empty());
}

TEST(NoiseModel, LoadDevice_2QNoise_DirectionSensitive) {
    // cx(0,1) uses cx_0_1 (error 0.02); the reversed cx(1,0) uses cx_1_0 (0.03).
    // Ordered matching keeps the two calibrations distinct, so the excess
    // depolarizing parameter differs by direction.
    NoiseModel nm;
    nm.load_device(shared_qubit_device());

    auto fwd = nm.channels_for_gate("cx", {0, 1});
    auto rev = nm.channels_for_gate("cx", {1, 0});
    EXPECT_TRUE(has_depolarizing(fwd));
    EXPECT_TRUE(has_depolarizing(rev));
    EXPECT_TRUE(depol_param(rev) > depol_param(fwd));   // 0.03 edge > 0.02 edge
}

// ── (#2): load_device is idempotent (replaces, not accumulates) ─────────

TEST(NoiseModel, LoadDevice_Twice_DoesNotAccumulateChannels) {
    NoiseModel nm;
    DeviceModel dev = shared_qubit_device();

    nm.load_device(dev);
    const std::size_t after_first = nm.channels_for_gate("cx", {0, 1}).size();
    nm.load_device(dev);                       // reload the same device
    const std::size_t after_second = nm.channels_for_gate("cx", {0, 1}).size();

    EXPECT_EQ(after_first, after_second);      // pre-fix this doubled
}

TEST(NoiseModel, LoadDevice_SecondDevice_DropsFirstDeviceChannels) {
    // Loading a different device must not leave the first device's gate behind.
    NoiseModel nm;
    nm.load_device(DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {"sx_0": {"duration_ns": 35.0, "error_rate": 0.001}}
    })"));
    EXPECT_TRUE(nm.has_channels_for_gate("sx"));

    nm.load_device(DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {"x_0": {"duration_ns": 35.0, "error_rate": 0.001}}
    })"));
    EXPECT_FALSE(nm.has_channels_for_gate("sx"));   // gone after the reload
    EXPECT_TRUE(nm.has_channels_for_gate("x"));
}

// ── (#5): bare gate key applies relaxation but no depol (with a warn) ────

TEST(NoiseModel, LoadDevice_BareGateKey_AppliesRelaxationButNoDepol) {
    // A bare "sx" key (no operand suffix) has unknown arity, so its error_rate
    // cannot become depolarizing; only duration-based relaxation attaches. A
    // log::warn is emitted (visible in the test log) so the drop is not silent.
    NoiseModel nm;
    nm.load_device(DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {"sx": {"duration_ns": 35.0, "error_rate": 0.001}}
    })"));

    auto chans = nm.channels_for_gate("sx", {0});
    EXPECT_EQ(count_relax_on(chans, 0), 2);        // relaxation still applied
    EXPECT_FALSE(has_depolarizing(chans));         // error_rate not synthesised
}

// ── Malformed device gate keys throw QPlayerError, not std::out_of_range (#20) ─
//
// split_gate_key() parsed the trailing qubit tokens with std::stoul, which throws
// std::out_of_range on an oversized number. That type is outside the QPlayerError
// hierarchy, so error.hpp's contract ("All throws must use one of these types so
// Python bindings can map them") was violated and the exception escaped unmapped.

TEST(NoiseModel, LoadDevice_GateKeyQubitTokenOverflows_ThrowsCircuitError) {
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {"cx_99999999999999999999_0": {"duration_ns": 300.0, "error_rate": 0.01}}
    })");
    NoiseModel nm;
    EXPECT_THROW(nm.load_device(dev), qplayer::CircuitError);
}

// A qubit index that fits in unsigned long but not in QubitIndex (uint32) used to
// be truncated into a plausible-looking id; it is now rejected.
TEST(NoiseModel, LoadDevice_GateKeyQubitExceedsQubitIndex_ThrowsCircuitError) {
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}],
      "gates": {"h_4294967296": {"duration_ns": 300.0}}
    })");
    NoiseModel nm;
    EXPECT_THROW(nm.load_device(dev), qplayer::CircuitError);
}

// ── explicit idle Kraus registry ─────────────────────────

namespace {

// A phase-flip channel written as raw Kraus operators, standing in for the
// twirled idle channel a harness computes: Pauli, trace-preserving, and
// distinguishable from the two-operator AD+PD pair T1T2Calculator emits.
KrausChannel phase_flip_channel(double p) {
    using C = std::complex<double>;
    KrausChannel ch;
    ch.operators = {
        {{C{std::sqrt(1.0 - p), 0.0}, C{0.0, 0.0}}, {C{0.0, 0.0}, C{std::sqrt(1.0 - p), 0.0}}},
        {{C{std::sqrt(p), 0.0}, C{0.0, 0.0}}, {C{0.0, 0.0}, C{-std::sqrt(p), 0.0}}},
    };
    ch.type = ChannelType::PhaseFlip;
    return ch;
}

DeviceModel one_qubit_device() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 50000.0, "T2": 30000.0}, {"T1": 50000.0, "T2": 30000.0}],
      "gates": {"sx_0": {"duration_ns": 35.0, "error_rate": 0.001}}
    })");
}

}  // namespace

TEST(NoiseModel, AddIdleKraus_RegisteredWindow_TakesPrecedenceOverT1T2) {
    // Arrange — device coherence would synthesise AD + PD (two channels).
    NoiseModel nm;
    nm.load_device(one_qubit_device());
    EXPECT_EQ(nm.idle_channels(0, 400.0).size(), std::size_t{2});

    // Act — register one explicit channel for exactly that window.
    nm.add_idle_kraus(0, 400.0, phase_flip_channel(0.02));

    // Assert — the registration replaces the synthesis, it does not add to it.
    auto chans = nm.idle_channels(0, 400.0);
    EXPECT_EQ(chans.size(), std::size_t{1});
    EXPECT_TRUE(chans.front().type == ChannelType::PhaseFlip);
    EXPECT_EQ(chans.front().target_qubit, qplayer::QubitIndex{0});
}

TEST(NoiseModel, AddIdleKraus_UnregisteredWindow_FallsBackToT1T2) {
    // Registering one window must not silence the others: a different duration
    // and a different qubit both keep the synthesised relaxation.
    NoiseModel nm;
    nm.load_device(one_qubit_device());
    nm.add_idle_kraus(0, 400.0, phase_flip_channel(0.02));

    EXPECT_EQ(nm.idle_channels(0, 401.0).size(), std::size_t{2});
    EXPECT_EQ(nm.idle_channels(1, 400.0).size(), std::size_t{2});
    EXPECT_TRUE(nm.has_idle_kraus(0, 400.0));
    EXPECT_FALSE(nm.has_idle_kraus(0, 401.0));
    EXPECT_FALSE(nm.has_idle_kraus(1, 400.0));
}

TEST(NoiseModel, AddIdleKraus_DurationKeyQuantisedToFemtosecond) {
    // The key is llround(ns * 1e6): a caller re-deriving 400 ns through a
    // different sum must hit the same entry, while a real (ns-scale) window
    // must not be merged into it.
    NoiseModel nm;
    nm.add_idle_kraus(0, 400.0, phase_flip_channel(0.02));

    EXPECT_TRUE(nm.has_idle_kraus(0, 400.0 + 1e-9));   // below the 1 fs grid
    EXPECT_FALSE(nm.has_idle_kraus(0, 400.001));       // 1 ps apart -> distinct
}

TEST(NoiseModel, AddIdleKraus_NegativeOrNonFiniteDuration_ThrowsCircuitError) {
    NoiseModel nm;
    EXPECT_THROW(nm.add_idle_kraus(0, -1.0, phase_flip_channel(0.02)),
                 qplayer::CircuitError);
    EXPECT_THROW(nm.add_idle_kraus(0, std::numeric_limits<double>::infinity(),
                                   phase_flip_channel(0.02)),
                 qplayer::CircuitError);
}

TEST(NoiseModel, AddIdleKraus_NonTracePreserving_ThrowsCircuitError) {
    // An idle window is a channel like any other (contract): reject
    // registration, not silently inside a trajectory.
    using C = std::complex<double>;
    KrausChannel bad;
    bad.operators = {{{C{2.0, 0.0}, C{0.0, 0.0}}, {C{0.0, 0.0}, C{2.0, 0.0}}}};
    NoiseModel nm;
    EXPECT_THROW(nm.add_idle_kraus(0, 400.0, bad), qplayer::CircuitError);
}

TEST(NoiseModel, AddIdleKraus_LoadDeviceClearsRegistry) {
    // load_device() is idempotent by clearing prior state; the idle registry
    // follows that rule, so callers must register after loading.
    NoiseModel nm;
    nm.load_device(one_qubit_device());
    nm.add_idle_kraus(0, 400.0, phase_flip_channel(0.02));
    EXPECT_TRUE(nm.has_idle_kraus(0, 400.0));

    nm.load_device(one_qubit_device());

    EXPECT_FALSE(nm.has_idle_kraus(0, 400.0));
    EXPECT_EQ(nm.idle_channels(0, 400.0).size(), std::size_t{2});
}

TEST(NoiseModel, AddIdleKraus_EmptyRegistry_IdleChannelsUnchanged) {
    // The regression guard for "no behaviour change when unused": with nothing
    // registered, idle_channels() must return exactly what it returned before.
    NoiseModel nm;
    nm.load_device(one_qubit_device());

    auto chans = nm.idle_channels(0, 400.0);

    EXPECT_EQ(chans.size(), std::size_t{2});
    EXPECT_TRUE(chans[0].type == ChannelType::AmplitudeDamping);
    EXPECT_TRUE(chans[1].type == ChannelType::PhaseDamping);
}

TEST(NoiseModel, ProjectToDense_IdleKrausRekeyedAndDroppedWithQubit) {
    // Truncation re-indexes the registry into dense space; an entry whose
    // physical qubit did not survive is dropped (handoff).
    NoiseModel nm;
    nm.add_idle_kraus(2, 400.0, phase_flip_channel(0.02));
    nm.add_idle_kraus(9, 400.0, phase_flip_channel(0.02));   // outside the map

    NoiseModel proj = nm.project_to_dense({{0, 2}, {1, 5}});

    EXPECT_TRUE(proj.has_idle_kraus(0, 400.0));              // dense 0 <- phys 2
    EXPECT_FALSE(proj.has_idle_kraus(2, 400.0));             // physical key gone
    EXPECT_EQ(proj.idle_channels(0, 400.0).front().target_qubit,
              qplayer::QubitIndex{0});                        // target remapped
    EXPECT_FALSE(proj.has_idle_kraus(1, 400.0));             // phys 5 unregistered
}
