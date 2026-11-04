// test_willow_device.cpp — Willow-like device model + Google noise synthesis
// verification. Loads the committed hand-authored
// tests/data/device_json/willow_like.json (a degree-4 square
// lattice with CZ-native basis) and checks that NoiseModel::load_device folds
// its calibration into basis-aware channels on the CZ native gate: per-operand
// T1/T2 relaxation + a genuine two-qubit (4x4) depolarizing channel + per-qubit
// readout error. This is the mechanism check; end-to-end surface-17 logical
// error runs are (report-only).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "../../../include/noise/device_model.hpp"
#include "../../../include/noise/noise_model.hpp"

using qplayer::DeviceModel;
using qplayer::NoiseModel;
using qplayer::KrausChannel;
using qplayer::QubitIndex;

namespace {

// Path to the committed test fixture. QPLAYER_ACCEPTANCE_DATA_DIR is the tests/
// source dir (tests/CMakeLists.txt target_compile_definitions). The fixture is
// kept inside tests/ so the test suite is self-contained (no reference outside
// tests/); the sc17-willow harness keeps its own copy
std::string willow_json_path() {
    return std::string(QPLAYER_ACCEPTANCE_DATA_DIR) +
           "/device_json/willow_like.json";
}

bool basis_has(const std::vector<std::string>& b, const std::string& g) {
    return std::find(b.begin(), b.end(), g) != b.end();
}

}  // namespace

// ── The committed JSON parses into a 25-qubit CZ-native square lattice ────────
TEST(WillowDevice, JsonFile_ParsesSquareLatticeAndCzBasis) {
    DeviceModel dm = DeviceModel::from_json_file(willow_json_path());

    EXPECT_EQ(dm.num_qubits(), QubitIndex{25});           // 5x5 grid (>= surface-17).
    EXPECT_TRUE(basis_has(dm.basis_gates(), "cz"));
    EXPECT_TRUE(dm.has_two_qubit_gate());
    EXPECT_EQ(dm.two_qubit_gate(), std::string{"cz"});

    // Degree-4 square lattice: every physical qubit couples to at most 4 others.
    EXPECT_TRUE(dm.has_coupling_map());
    std::vector<int> degree(25, 0);
    for (const auto& e : dm.coupling_map()) degree[e.first]++;
    int max_degree = 0;
    for (int d : degree) max_degree = std::max(max_degree, d);
    EXPECT_TRUE(max_degree <= 4);   // interior qubits have exactly 4 neighbours.
}

// ── load_device synthesizes CZ noise: 1Q relaxation on each operand + 2Q depol ─
TEST(WillowDevice, LoadDevice_SynthesizesCzNoiseChannels) {
    DeviceModel dm = DeviceModel::from_json_file(willow_json_path());
    NoiseModel nm;
    nm.load_device(dm);

    EXPECT_TRUE(nm.has_channels_for_gate("cz"));
    // Qubits 0 and 1 are coupled (row-0 horizontal edge) -> cz_0_1 in gate_props.
    std::vector<KrausChannel> chans = nm.channels_for_gate("cz", {0, 1});
    EXPECT_TRUE(chans.size() >= std::size_t{2});   // relaxation(q0) + relaxation(q1) + 2Q depol.

    // A genuine two-qubit channel has 4x4 Kraus operators.
    bool has_2q = false, has_1q = false;
    for (const KrausChannel& ch : chans) {
        if (ch.operators.empty()) continue;
        const std::size_t dim = ch.operators[0].size();
        if (dim == 4) has_2q = true;   // 2-qubit depolarizing.
        if (dim == 2) has_1q = true;   // per-operand relaxation.
    }
    EXPECT_TRUE(has_2q);   // excess-over-relaxation 2Q depolarizing present.
    EXPECT_TRUE(has_1q);   // per-operand T1/T2 relaxation present.
}

// ── load_device synthesizes single-qubit noise + per-qubit readout error ──────
TEST(WillowDevice, LoadDevice_SynthesizesSingleQubitAndReadout) {
    DeviceModel dm = DeviceModel::from_json_file(willow_json_path());
    NoiseModel nm;
    nm.load_device(dm);

    // sx is a native single-qubit gate -> relaxation (+ excess depolarizing).
    EXPECT_TRUE(nm.has_channels_for_gate("sx"));
    EXPECT_FALSE(nm.channels_for_gate("sx", {0}).empty());

    // Readout error auto-registered from prob_meas0_prep1 / prob_meas1_prep0.
    EXPECT_TRUE(nm.has_readout_errors());
    const std::pair<double, double> dev_ro = dm.readout_error(0);
    const std::pair<double, double> nm_ro  = nm.readout_error(0);
    EXPECT_NEAR(nm_ro.first,  dev_ro.first,  1e-12);   // p01 passed through exactly.
    EXPECT_NEAR(nm_ro.second, dev_ro.second, 1e-12);   // p10 passed through exactly.
    EXPECT_TRUE(dev_ro.first > 0.0 && dev_ro.second > 0.0);  // Willow readout ~0.7-0.9 %.
}
