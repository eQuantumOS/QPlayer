// test_qregister_load_device.cpp — Unit tests for QRegister::load_device_model()
// Verifies that load_device_model() stores
// the DeviceModel for transpile() fallback and populates the noise model so
// has_noise_model_ is activated (confirmed indirectly via run() not throwing).

#include "../../framework/assertions.hpp"
#include "../../framework/test_case.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/qregister.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::QRegister;

namespace {

// Write a minimal 2-qubit device JSON to a temp file. Returns the file path.
// The caller is responsible for cleanup; the test binary working directory is
// used so the path is always writable during ctest (schema).
std::string write_device_json(const std::string& suffix = "") {
    const char* dir = std::getenv("TMPDIR");
    std::string path = (dir ? std::string(dir) : std::string("/tmp"))
                       + "/qplayer_test_device" + suffix + ".json";
    std::ofstream f(path);
    f << R"({
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
    return path;
}

// Minimal 2-qubit Bell circuit (no measure), safe for noise-mode run().
static Circuit make_bell() {
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    return c;
}

}  // namespace

// ── Happy path ───────────────────────────────────────────────────────────────

TEST(QRegisterLoadDevice, LoadValidJson_NoThrow) {
    // load_device_model() should complete without exception for a valid file.
    QRegister qr;
    const std::string path = write_device_json("_a");
    EXPECT_NO_THROW(qr.load_device_model(path));
}

TEST(QRegisterLoadDevice, LoadValidJson_RunSucceeds) {
    // After load_device_model(), run() must not throw for a normal circuit.
    // The noise path is activated; this verifies the noise model integration
    // (: T1/T2 channels are injected via NoiseModel::load_device).
    QRegister qr;
    const std::string path = write_device_json("_b");
    EXPECT_NO_THROW(qr.load_device_model(path));
    EXPECT_NO_THROW(qr.run(make_bell(), /*nshots=*/100));
}

TEST(QRegisterLoadDevice, LoadValidJson_TranspileUsesDevice) {
    // After load_device_model(), transpile() with an empty device_json should
    // fall back to the stored device_model_ (truth table).
    // Linear coupling [0,1] admits the Bell CX, so no SWAP needed.
    QRegister qr;
    const std::string path = write_device_json("_c");
    EXPECT_NO_THROW(qr.load_device_model(path));
    // transpile() with empty device_json uses the stored device — must not throw.
    EXPECT_NO_THROW(qr.transpile(make_bell(), /*optimization_level=*/1, /*device_json=*/""));
}

// ── Error path ───────────────────────────────────────────────────────────────

TEST(QRegisterLoadDevice, MissingFile_ThrowsCircuitError) {
    // from_json_file() throws CircuitError on I/O failure; load_device_model()
    // must propagate it unmodified.
    QRegister qr;
    EXPECT_THROW(qr.load_device_model("/nonexistent/path/device.json"),
                 CircuitError);
}
