// test_device_model.cpp — Phase 8 unit tests for DeviceModel JSON
// ingestion + query. Covers the
// schema (qubits / gates / coupling_map), the infinity fallback for
// unknown qubits, qubit-specific vs bare gate keys, full-connectivity
// semantics, and value-range / syntax error paths.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <string>

#include "../../../include/error.hpp"
#include "../../../include/noise/device_model.hpp"

using qplayer::CircuitError;
using qplayer::DeviceModel;
using qplayer::GateProperty;
using qplayer::QubitIndex;

namespace {

// A full 2-qubit device: coherence times, one bare + one qubit-specific gate,
// and a single coupling edge.
const char* kDeviceJson = R"({
  "qubits": [
    {"T1": 50000.0, "T2": 30000.0},
    {"T1": 45000.0, "T2": 28000.0}
  ],
  "gates": {
    "h": {"duration_ns": 35.0, "error_rate": 0.001},
    "cx_0_1": {"duration_ns": 300.0, "error_rate": 0.01}
  },
  "coupling_map": [[0, 1]]
})";

}  // namespace

// ── Schema parsing ───────────────────────────────────────────────────────────

TEST(DeviceModel, FromJsonString_ParsesCoherenceAndCount) {
    // Act
    DeviceModel dm = DeviceModel::from_json_string(kDeviceJson);

    // Assert
    EXPECT_EQ(dm.num_qubits(), static_cast<QubitIndex>(2));
    EXPECT_NEAR(dm.T1(0), 50000.0, 1e-9);
    EXPECT_NEAR(dm.T2(1), 28000.0, 1e-9);
}

TEST(DeviceModel, UnknownQubit_ReturnsInfinity) {
    // Arrange
    DeviceModel dm = DeviceModel::from_json_string(kDeviceJson);

    // Act / Assert — qubit 5 is undescribed, so coherence is infinite (no noise).
    EXPECT_TRUE(std::isinf(dm.T1(5)));
    EXPECT_TRUE(std::isinf(dm.T2(5)));
}

TEST(DeviceModel, GateProperty_QubitSpecificKey_TakesPrecedence) {
    // Arrange
    DeviceModel dm = DeviceModel::from_json_string(kDeviceJson);

    // Act — "cx_0_1" is the qubit-specific key for cx on operands {0, 1}.
    EXPECT_TRUE(dm.has_gate_property("cx", {0, 1}));
    GateProperty gp = dm.gate_property("cx", {0, 1});

    // Assert
    EXPECT_NEAR(gp.duration_ns, 300.0, 1e-9);
    EXPECT_NEAR(gp.error_rate, 0.01, 1e-12);
}

TEST(DeviceModel, GateProperty_BareNameFallback) {
    // Arrange
    DeviceModel dm = DeviceModel::from_json_string(kDeviceJson);

    // Act — "h" has only a bare key; any operand list resolves to it.
    EXPECT_TRUE(dm.has_gate_property("h", {1}));
    GateProperty gp = dm.gate_property("h", {1});

    // Assert
    EXPECT_NEAR(gp.duration_ns, 35.0, 1e-9);
}

TEST(DeviceModel, GateProperty_Unknown_ThrowsCircuitError) {
    DeviceModel dm = DeviceModel::from_json_string(kDeviceJson);
    EXPECT_FALSE(dm.has_gate_property("rz", {0}));
    EXPECT_THROW(dm.gate_property("rz", {0}), CircuitError);
}

// ── Coupling map / full connectivity ───────────────────────────────

TEST(DeviceModel, CouplingMap_Present_IsReadable) {
    // Arrange
    DeviceModel dm = DeviceModel::from_json_string(kDeviceJson);

    // Assert
    EXPECT_TRUE(dm.has_coupling_map());
    EXPECT_EQ(dm.coupling_map().size(), static_cast<std::size_t>(1));
    EXPECT_EQ(dm.coupling_map()[0].first, static_cast<QubitIndex>(0));
    EXPECT_EQ(dm.coupling_map()[0].second, static_cast<QubitIndex>(1));
}

TEST(DeviceModel, CouplingMap_Absent_MeansFullConnectivity) {
    // Arrange — no coupling_map key.
    DeviceModel dm = DeviceModel::from_json_string(
        R"({"qubits": [{"T1": 1.0, "T2": 1.0}]})");

    // Act / Assert — full connectivity: has_coupling_map() false, accessor throws.
    EXPECT_FALSE(dm.has_coupling_map());
    EXPECT_THROW(dm.coupling_map(), CircuitError);
}

TEST(DeviceModel, CouplingMap_EmptyArray_MeansFullConnectivity) {
    // An explicit empty array collapses to the absent case.
    DeviceModel dm = DeviceModel::from_json_string(R"({"coupling_map": []})");
    EXPECT_FALSE(dm.has_coupling_map());
}

TEST(DeviceModel, CouplingMap_ExtendsNumQubits) {
    // A coupling edge may reference qubits beyond the 'qubits' array.
    DeviceModel dm = DeviceModel::from_json_string(R"({"coupling_map": [[0, 3]]})");
    EXPECT_EQ(dm.num_qubits(), static_cast<QubitIndex>(4));
}

// ── Value-range + syntax errors ──────────────────────────────────────────────

TEST(DeviceModel, NonPositiveT1_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_string(R"({"qubits": [{"T1": 0.0, "T2": 1.0}]})"),
                 CircuitError);
}

TEST(DeviceModel, ErrorRateOutOfRange_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_string(
                     R"({"gates": {"h": {"duration_ns": 10.0, "error_rate": 1.5}}})"),
                 CircuitError);
}

TEST(DeviceModel, MalformedJson_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_string("{ not valid json"), CircuitError);
}

TEST(DeviceModel, TopLevelNotObject_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_string("[1, 2, 3]"), CircuitError);
}

// ── File factory ─────────────────────────────────────────────────────────────

TEST(DeviceModel, FromJsonFile_MissingFile_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_file("/nonexistent/path/device.json"),
                 CircuitError);
}

TEST(DeviceModel, FromJsonFile_RoundTripsContent) {
    // Arrange — write the device JSON to a temp file, then load it back.
    const std::string path = "test_device_model_tmp.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << kDeviceJson;
    }

    // Act
    DeviceModel dm = DeviceModel::from_json_file(path);
    std::remove(path.c_str());

    // Assert
    EXPECT_EQ(dm.num_qubits(), static_cast<QubitIndex>(2));
    EXPECT_NEAR(dm.T1(0), 50000.0, 1e-9);
}

// ── Extended schema: basis_gates / two_qubit_gate / readout (1006) ─

// A gen_device_json.py-style superset: native basis, 2Q gate, asymmetric readout.
static const char* kEagleJson = R"({
  "qubits": [
    {"T1": 200000.0, "T2": 150000.0, "prob_meas0_prep1": 0.02, "prob_meas1_prep0": 0.03},
    {"T1": 180000.0, "T2": 120000.0, "readout_error": 0.05}
  ],
  "gates": {"ecr_0_1": {"duration_ns": 660.0, "error_rate": 0.008}},
  "basis_gates": ["id", "rz", "sx", "x", "ecr", "measure", "reset"],
  "two_qubit_gate": "ecr",
  "coupling_map": [[0, 1]]
})";

TEST(DeviceModel, BasisGates_Parsed) {
    DeviceModel dm = DeviceModel::from_json_string(kEagleJson);

    const auto& bg = dm.basis_gates();
    EXPECT_EQ(bg.size(), std::size_t{7});
    EXPECT_EQ(bg[0], std::string{"id"});
    EXPECT_EQ(bg[4], std::string{"ecr"});
}

TEST(DeviceModel, BasisGates_Absent_ReturnsEmpty) {
    DeviceModel dm = DeviceModel::from_json_string(R"({"qubits": [{"T1": 1.0, "T2": 1.0}]})");
    EXPECT_TRUE(dm.basis_gates().empty());
}

TEST(DeviceModel, BasisGates_NonStringEntry_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_string(R"({"basis_gates": ["sx", 42]})"),
                 CircuitError);
}

TEST(DeviceModel, TwoQubitGate_Parsed) {
    DeviceModel dm = DeviceModel::from_json_string(kEagleJson);

    EXPECT_TRUE(dm.has_two_qubit_gate());
    EXPECT_EQ(dm.two_qubit_gate(), std::string{"ecr"});
}

TEST(DeviceModel, TwoQubitGate_Null_MeansAbsent) {
    // JSON null (device without a 2Q gate) leaves has_two_qubit_gate() false.
    DeviceModel dm = DeviceModel::from_json_string(R"({"two_qubit_gate": null})");
    EXPECT_FALSE(dm.has_two_qubit_gate());
    EXPECT_THROW(dm.two_qubit_gate(), CircuitError);
}

TEST(DeviceModel, TwoQubitGate_Absent_AccessorThrows) {
    DeviceModel dm = DeviceModel::from_json_string(R"({"qubits": [{"T1": 1.0, "T2": 1.0}]})");
    EXPECT_FALSE(dm.has_two_qubit_gate());
    EXPECT_THROW(dm.two_qubit_gate(), CircuitError);
}

TEST(DeviceModel, TwoQubitGate_WrongType_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_string(R"({"two_qubit_gate": 7})"), CircuitError);
}

TEST(DeviceModel, ReadoutError_AsymmetricParsed) {
    DeviceModel dm = DeviceModel::from_json_string(kEagleJson);

    EXPECT_TRUE(dm.has_readout_error(0));
    const auto ro = dm.readout_error(0);
    EXPECT_NEAR(ro.first, 0.02, 1e-12);   // prob_meas0_prep1 (p01)
    EXPECT_NEAR(ro.second, 0.03, 1e-12);  // prob_meas1_prep0 (p10)
}

TEST(DeviceModel, ReadoutError_SymmetricFallback) {
    // Qubit 1 gives only 'readout_error' -> both directions take that value.
    DeviceModel dm = DeviceModel::from_json_string(kEagleJson);

    const auto ro = dm.readout_error(1);
    EXPECT_NEAR(ro.first, 0.05, 1e-12);
    EXPECT_NEAR(ro.second, 0.05, 1e-12);
}

TEST(DeviceModel, ReadoutError_Absent_ReturnsZero) {
    DeviceModel dm = DeviceModel::from_json_string(R"({"qubits": [{"T1": 1.0, "T2": 1.0}]})");

    EXPECT_FALSE(dm.has_readout_error(0));
    const auto ro = dm.readout_error(0);
    EXPECT_NEAR(ro.first, 0.0, 1e-12);
    EXPECT_NEAR(ro.second, 0.0, 1e-12);
    // Out-of-range qubit is also {0, 0}, not a crash.
    EXPECT_FALSE(dm.has_readout_error(9));
}

TEST(DeviceModel, ReadoutError_OutOfRange_ThrowsCircuitError) {
    EXPECT_THROW(DeviceModel::from_json_string(
                     R"({"qubits": [{"prob_meas0_prep1": 1.5}]})"),
                 CircuitError);
}
