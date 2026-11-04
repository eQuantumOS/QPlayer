// test_idle_noise_inserter.cpp — Phase 8 unit tests for
// IdleNoiseInserterPass. A timed gate inserts an
// IdleOp on every non-participating qubit; untimed gates insert nothing.
// Durations are supplied via DurationAnnotationPass so the test exercises
// the realistic scheduling-stage pairing.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <variant>
#include <vector>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/noise/device_model.hpp"
#include "../../../../include/transpiler/passes/analysis/duration_annotation_pass.hpp"
#include "../../../../include/transpiler/passes/scheduling/idle_noise_inserter_pass.hpp"

using qplayer::Circuit;
using qplayer::DeviceModel;
using qplayer::DurationAnnotationPass;
using qplayer::IdleNoiseInserterPass;
using qplayer::IdleOp;
using qplayer::Op;
using qplayer::QubitIndex;

namespace {

// Collect all IdleOps in op order.
std::vector<IdleOp> idle_ops(const Circuit& c) {
    std::vector<IdleOp> out;
    for (const Op& op : c.ops()) {
        if (std::holds_alternative<IdleOp>(op)) out.push_back(std::get<IdleOp>(op));
    }
    return out;
}

// A 3-qubit device with generic durations for h and cx.
DeviceModel three_qubit_device() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "gates": {"h": {"duration_ns": 35.0}, "cx": {"duration_ns": 300.0}}
    })");
}

}  // namespace

// ── Single-qubit gate idles the remaining qubits ─────────────────────────────

TEST(IdleNoiseInserter, SingleQubitGate_IdlesOtherQubits) {
    // Arrange — annotate durations, then insert idle noise.
    DeviceModel dev = three_qubit_device();
    Circuit circ(3, 0);
    circ.h(0);
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    // Act
    IdleNoiseInserterPass pass;
    Circuit out = pass.run(timed);

    // Assert — h(0) idles qubits 1 and 2 for 35 ns each.
    EXPECT_TRUE(pass.changed());
    auto idles = idle_ops(out);
    EXPECT_EQ(idles.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(idles[0].qubit, static_cast<QubitIndex>(1));
    EXPECT_EQ(idles[1].qubit, static_cast<QubitIndex>(2));
    EXPECT_NEAR(idles[0].duration_ns, 35.0, 1e-9);
    EXPECT_NEAR(idles[1].duration_ns, 35.0, 1e-9);
}

// ── Two-qubit gate idles only the non-participating qubit ─────────────────────

TEST(IdleNoiseInserter, TwoQubitGate_IdlesOnlyNonParticipating) {
    // Arrange
    DeviceModel dev = three_qubit_device();
    Circuit circ(3, 0);
    circ.cx(0, 1);
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    // Act
    IdleNoiseInserterPass pass;
    Circuit out = pass.run(timed);

    // Assert — only qubit 2 idles; operands 0,1 are busy.
    auto idles = idle_ops(out);
    EXPECT_EQ(idles.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(idles[0].qubit, static_cast<QubitIndex>(2));
    EXPECT_NEAR(idles[0].duration_ns, 300.0, 1e-9);
}

// ── Untimed gates contribute no idle noise (limitation) ──────────────

TEST(IdleNoiseInserter, UntimedGate_InsertsNothing) {
    // Arrange — no duration annotation (no device pass run).
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1);

    // Act
    IdleNoiseInserterPass pass;
    Circuit out = pass.run(circ);

    // Assert
    EXPECT_FALSE(pass.changed());
    EXPECT_TRUE(idle_ops(out).empty());
}

// ── Multiple timed gates each insert their own idle window ────────────────────

TEST(IdleNoiseInserter, MultipleGates_EachInsertIdle) {
    // Arrange
    DeviceModel dev = three_qubit_device();
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1);  // h(0): idle {1,2}; cx(0,1): idle {2}
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    // Act
    Circuit out = IdleNoiseInserterPass().run(timed);

    // Assert — 2 (from h) + 1 (from cx) = 3 IdleOps total.
    EXPECT_EQ(idle_ops(out).size(), static_cast<std::size_t>(3));
}
