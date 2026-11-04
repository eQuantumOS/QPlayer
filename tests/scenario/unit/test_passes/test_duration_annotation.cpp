// test_duration_annotation.cpp — Phase 8 unit tests for
// DurationAnnotationPass. Three
// scenarios from the task spec: partial fill, full fill, and the
// device-undefined fallback. Plus idempotency (pre-set durations are respected).

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <string>
#include <variant>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/noise/device_model.hpp"
#include "../../../../include/transpiler/passes/analysis/duration_annotation_pass.hpp"

using qplayer::Circuit;
using qplayer::DeviceModel;
using qplayer::DurationAnnotationPass;
using qplayer::GateOp;
using qplayer::Op;

namespace {

// Find the first GateOp with the given name in circuit op order and return its
// duration_ns (or a sentinel object when absent). Tests use simple circuits so
// "first match" is unambiguous for distinct gate names.
std::optional<double> duration_of(const Circuit& c, const std::string& name,
                                  std::size_t skip = 0) {
    std::size_t seen = 0;
    for (const Op& op : c.ops()) {
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            if (g.name == name) {
                if (seen++ == skip) return g.duration_ns;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

// ── Scenario 1: partial fill (some gates described, some not) ─────────────────

TEST(DurationAnnotation, PartialFill_OnlyDescribedGatesAnnotated) {
    // Arrange — device knows generic "h" and qubit-specific "cx_0_1" only.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "gates": {"h": {"duration_ns": 35.0}, "cx_0_1": {"duration_ns": 300.0}}
    })");
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1).cx(1, 2);  // cx(1,2) has no matching device key

    // Act
    DurationAnnotationPass pass(dev);
    Circuit out = pass.run(circ);

    // Assert
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(duration_of(out, "h").has_value());
    EXPECT_NEAR(*duration_of(out, "h"), 35.0, 1e-9);
    EXPECT_TRUE(duration_of(out, "cx", 0).has_value());      // cx(0,1) -> cx_0_1
    EXPECT_NEAR(*duration_of(out, "cx", 0), 300.0, 1e-9);
    EXPECT_FALSE(duration_of(out, "cx", 1).has_value());     // cx(1,2) -> fallback
}

// ── Scenario 2: full fill (all gates described generically) ───────────────────

TEST(DurationAnnotation, FullFill_AllGatesAnnotated) {
    // Arrange — generic durations for every gate the circuit uses.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "gates": {"h": {"duration_ns": 35.0}, "cx": {"duration_ns": 300.0}}
    })");
    Circuit circ(2, 0);
    circ.h(0).cx(0, 1);

    // Act
    DurationAnnotationPass pass(dev);
    Circuit out = pass.run(circ);

    // Assert
    EXPECT_TRUE(pass.changed());
    EXPECT_NEAR(*duration_of(out, "h"), 35.0, 1e-9);
    EXPECT_NEAR(*duration_of(out, "cx"), 300.0, 1e-9);
}

// ── Scenario 3: device-undefined gate/qubit fallback ─────────────────────────

TEST(DurationAnnotation, UndefinedGate_LeavesDurationEmpty) {
    // Arrange — device describes only cx_0_1; the circuit's gates do not match.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}],
      "gates": {"cx_0_1": {"duration_ns": 300.0}}
    })");
    Circuit circ(1, 0);
    circ.h(0).x(0);  // neither "h" nor "x" described

    // Act
    DurationAnnotationPass pass(dev);
    Circuit out = pass.run(circ);

    // Assert — nothing annotated; pass reports no change.
    EXPECT_FALSE(pass.changed());
    EXPECT_FALSE(duration_of(out, "h").has_value());
    EXPECT_FALSE(duration_of(out, "x").has_value());
}

// ── Idempotency: a second run respects already-filled durations ───────────────

TEST(DurationAnnotation, SecondRun_IsNoOp) {
    // Arrange
    DeviceModel dev = DeviceModel::from_json_string(
        R"({"qubits": [{"T1": 1e5, "T2": 6e4}], "gates": {"h": {"duration_ns": 35.0}}})");
    Circuit circ(1, 0);
    circ.h(0);

    // Act — first run fills, second run finds everything already set.
    DurationAnnotationPass pass(dev);
    Circuit once  = pass.run(circ);
    EXPECT_TRUE(pass.changed());
    Circuit twice = pass.run(once);

    // Assert
    EXPECT_FALSE(pass.changed());
    EXPECT_NEAR(*duration_of(twice, "h"), 35.0, 1e-9);
}
