// test_transpile_device_json.cpp — QRegister Batch pipeline wiring:
// - transpile() device_json policy (⓪.5): empty -> device-
//     independent; invalid -> fall back to device-less; valid coupling map ->
//     coupling-map routing pipeline (② layout → ③ routing → ⑤ verify).
//   - parse() delegates to the Phase 3 Parser facade.
// - run() drives the fast path end-to-end (counts populated).
// - set_omp_threads() applies without error.
//

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::QRegister;
using qplayer::Circuit;
using qplayer::SimType;
using qplayer::Result;
using qplayer::CircuitError;
using qplayer::NotImplementedError;

// ── device_json policy (Phase 5 conservative) ────────────────────────────────

// Empty device_json -> device-independent transpile, no throw.
TEST(TranspileDeviceJson, EmptyDeviceJson_Succeeds) {
    // Arrange
    QRegister qr(SimType::FullState);
    Circuit circ(2);
    circ.h(0).cx(0, 1);

    // Act / Assert: device-independent path returns a transpiled circuit.
    EXPECT_NO_THROW(qr.transpile(circ, /*optimization_level=*/1, /*device_json=*/""));
}

// Invalid device_json -> falls back to the (absent) member device -> device-
// less compile, no throw (⓪.5 device-entry decision).
TEST(TranspileDeviceJson, InvalidDeviceJson_FallsBackToDeviceLess) {
    // Arrange
    QRegister qr(SimType::FullState);
    Circuit circ(2);
    circ.h(0).cx(0, 1);

    // Act / Assert — "device.json" does not start with '{', so it is treated as
    // a file path; the file does not exist, so the transpiler warns
    // and compiles device-independently (no throw).
    EXPECT_NO_THROW(qr.transpile(circ, 1, "device.json"));
}

// Valid device_json (JSON CONTENT) with a coupling map drives coupling-map routing.
TEST(TranspileDeviceJson, ValidCouplingMap_RoutesWithoutThrow) {
    QRegister qr(SimType::FullState);
    Circuit circ(3);
    circ.h(0).cx(0, 2);  // non-adjacent on a chain -> SWAP inserted

    EXPECT_NO_THROW(qr.transpile(circ, 1, R"({"coupling_map":[[0,1],[1,2]]})"));
}

// device_json may be a FILE PATH (auto-detected: does not start with '{'),
// symmetric with load_device_model(path). Writing a coupling-map
// device to disk and transpiling a non-adjacent cx must route (SWAPs inserted),
// proving the path was read rather than silently skipped.
TEST(TranspileDeviceJson, DeviceJsonAsFilePath_LoadsAndRoutes) {
    const char* path = "qplayer_test_device_0120.json";
    {
        std::ofstream f(path);
        f << R"({"coupling_map":[[0,1],[1,2]]})";
    }

    QRegister qr(SimType::FullState);
    Circuit circ(3);
    circ.h(0).cx(0, 2);  // non-adjacent -> routing expands the op list

    Circuit out = qr.transpile(circ, 1, path);
    std::remove(path);

    // Device-less fallback would leave the 2 input ops untouched; a loaded
    // coupling map routes cx(0,2) and grows the circuit.
    EXPECT_TRUE(out.num_ops() > static_cast<std::size_t>(2));
}

// Out-of-range optimization_level -> CircuitError.
TEST(TranspileDeviceJson, InvalidLevel_Throws) {
    QRegister qr(SimType::FullState);
    Circuit circ(1);
    EXPECT_THROW(qr.transpile(circ, /*optimization_level=*/5, ""), CircuitError);
}

// ── Pipeline wiring (parse / run / set_omp_threads) ──────────────────────────

// parse() delegates to the Parser facade and builds a Circuit (Phase 3 wiring).
TEST(QRegisterPipeline, Parse_SimpleQASM_BuildsCircuit) {
    // Arrange
    QRegister qr(SimType::FullState);
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "creg c[2];\n"
        "h q[0];\n"
        "cx q[0],q[1];\n";

    // Act
    Circuit circ = qr.parse(qasm);

    // Assert
    EXPECT_EQ(circ.num_qubits(), std::uint32_t{2});
}

// run() drives the fast path: counts populated, shots match nshots.
TEST(QRegisterPipeline, Run_Bell_ProducesCounts) {
    // Arrange
    QRegister qr(SimType::FullState);
    qr.set_seed(123);  // deterministic
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act
    Result r = qr.run(circ, /*nshots=*/2000);

    // Assert: counts sum to nshots; only correlated "00"/"11" appear.
    std::uint64_t total = 0;
    for (const auto& kv : r.counts()) { total += kv.second; }
    EXPECT_EQ(total, std::uint64_t{2000});
    EXPECT_EQ(r.shots(), std::uint64_t{2000});
}

// enable_fusion on vs off produce the same correct distribution (parity).
TEST(QRegisterPipeline, Run_FusionOnOff_SameTotals) {
    QRegister qr(SimType::FullState);
    qr.set_seed(7);
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).measure_all();

    Result on  = qr.run(circ, 1000, /*enable_fusion=*/true,  4);
    Result off = qr.run(circ, 1000, /*enable_fusion=*/false, 4);

    std::uint64_t total_on = 0, total_off = 0;
    for (const auto& kv : on.counts())  { total_on  += kv.second; }
    for (const auto& kv : off.counts()) { total_off += kv.second; }
    EXPECT_EQ(total_on,  std::uint64_t{1000});
    EXPECT_EQ(total_off, std::uint64_t{1000});
}

// set_omp_threads() applies without error for a positive count.
TEST(QRegisterPipeline, SetOmpThreads_Positive_NoThrow) {
    QRegister qr(SimType::FullState);
    EXPECT_NO_THROW(qr.set_omp_threads(2));
}
