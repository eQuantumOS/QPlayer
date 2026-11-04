// test_compiler.cpp — Phase 8 unit tests for the device-aware transpile()
// pipeline and the coupling validator. Covers: CouplingValidatorPass
// rejecting a non-adjacent 2Q gate with ConnectivityError, accepting an adjacent
// one, the full-connectivity no-op, and transpile() routing a non-adjacent gate
// against a coupling map (SWAP insertion, no error).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <variant>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/error.hpp"
#include "../../../include/noise/device_model.hpp"
#include "../../../include/transpiler/transpiler.hpp"
#include "../../../include/transpiler/passes/validation/coupling_validator.hpp"

using qplayer::Circuit;
using qplayer::ConnectivityError;
using qplayer::CouplingValidatorPass;
using qplayer::DeviceModel;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;
using qplayer::Transpiler;

namespace {

DeviceModel linear_3q() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "coupling_map": [[0, 1], [1, 2]]
    })");
}

std::size_t count_swaps(const Circuit& c) {
    std::size_t n = 0;
    for (const Op& op : c.ops()) {
        if (std::holds_alternative<GateOp>(op) &&
            std::get<GateOp>(op).type == GateType::SWAP) {
            ++n;
        }
    }
    return n;
}

}  // namespace

// ── CouplingValidatorPass ───────────────────────────────────────────

TEST(Compiler, CouplingValidator_NonAdjacentGate_ThrowsConnectivityError) {
    // Arrange — cx on physical 0,2 which are NOT directly coupled (0-1-2).
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);
    circ.cx(0, 2);

    // Act / Assert
    CouplingValidatorPass pass(dev);
    EXPECT_THROW(pass.run(circ), ConnectivityError);
}

TEST(Compiler, CouplingValidator_AdjacentGate_NoThrow) {
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);
    circ.cx(0, 1).cx(1, 2);  // both adjacent on the chain

    CouplingValidatorPass pass(dev);
    EXPECT_NO_THROW(pass.run(circ));
}

TEST(Compiler, CouplingValidator_FullConnectivity_NoThrow) {
    // No coupling map -> full connectivity -> nothing to validate.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}]
    })");
    Circuit circ(3, 0);
    circ.cx(0, 2);

    CouplingValidatorPass pass(dev);
    EXPECT_NO_THROW(pass.run(circ));
}

// ── transpile() device pipeline ───────────────────────────────────────────────

TEST(Compiler, Transpile_RoutesNonAdjacentGate_InsertsSwapAndVerifies) {
    // A non-adjacent cx on a linear chain must be routed (SWAP inserted) and
    // pass the post-routing coupling verify without throwing.
    Circuit circ(3, 0);
    circ.h(0).cx(0, 2);

    Transpiler t;
    Circuit out = t.transpile(std::move(circ), 1,
                              R"({"coupling_map":[[0,1],[1,2]]})");

    EXPECT_TRUE(count_swaps(out) > 0);
}

TEST(Compiler, Transpile_AdjacentCircuit_NoSwapNeeded) {
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1);  // already adjacent

    Transpiler t;
    Circuit out = t.transpile(std::move(circ), 1,
                              R"({"coupling_map":[[0,1],[1,2]]})");

    EXPECT_EQ(count_swaps(out), static_cast<std::size_t>(0));
}
