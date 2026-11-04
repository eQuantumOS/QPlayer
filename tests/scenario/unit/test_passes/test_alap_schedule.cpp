// test_alap_schedule.cpp — unit tests for ALAPSchedulePass (
// supersedes). The pass list-schedules the circuit from the
// device durations stamped by DurationAnnotationPass and inserts one IdleOp per
// qubit idle gap. The defining property vs the old per-gate IdleNoiseInserter
// (removed in Phase 8-F):
// gates that run in parallel share a time slot, so a bystander qubit idles once
// for the slot span, not once per foreign gate.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <variant>
#include <vector>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/noise/device_model.hpp"
#include "../../../../include/transpiler/passes/analysis/duration_annotation_pass.hpp"
#include "../../../../include/transpiler/passes/scheduling/alap_schedule.hpp"

using qplayer::ALAPSchedulePass;
using qplayer::Circuit;
using qplayer::DeviceModel;
using qplayer::DurationAnnotationPass;
using qplayer::GateOp;
using qplayer::IdleOp;
using qplayer::Op;
using qplayer::QubitIndex;

namespace {

std::vector<IdleOp> idle_ops(const Circuit& c) {
    std::vector<IdleOp> out;
    for (const Op& op : c.ops()) {
        if (std::holds_alternative<IdleOp>(op)) out.push_back(std::get<IdleOp>(op));
    }
    return out;
}

// Total idle duration accumulated on a given qubit.
double idle_total(const Circuit& c, QubitIndex q) {
    double sum = 0.0;
    for (const IdleOp& io : idle_ops(c)) {
        if (io.qubit == q) sum += io.duration_ns;
    }
    return sum;
}

DeviceModel three_qubit_device() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "gates": {"h": {"duration_ns": 35.0}, "cx": {"duration_ns": 300.0}}
    })");
}

}  // namespace

// ── Parallel single-qubit gates share a slot: bystander idles once ────────────

TEST(ALAPSchedule, ParallelGates_BystanderIdlesOnce) {
    // Arrange — h(0) and h(1) act on disjoint qubits, so they schedule into the
    // same [0,35] slot. Only q2 (idle the whole slot) should decohere; q0 and q1
    // are busy and must NOT idle during each other's parallel gate.
    DeviceModel dev = three_qubit_device();
    Circuit circ(3, 0);
    circ.h(0).h(1);
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    // Act
    ALAPSchedulePass pass;
    Circuit out = pass.run(timed);

    // Assert — a single IdleOp on q2 for the 35 ns slot (per-gate model would
    // have produced q0:35 + q1:35 + q2:70 across 4 IdleOps).
    EXPECT_TRUE(pass.changed());
    auto idles = idle_ops(out);
    EXPECT_EQ(idles.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(idles[0].qubit, static_cast<QubitIndex>(2));
    EXPECT_NEAR(idles[0].duration_ns, 35.0, 1e-9);
    EXPECT_NEAR(idle_total(out, 0), 0.0, 1e-9);
    EXPECT_NEAR(idle_total(out, 1), 0.0, 1e-9);
}

// ── Serial chain preserves per-qubit idle totals ──────────────────────────────

TEST(ALAPSchedule, SerialChain_PreservesPerQubitIdle) {
    // Arrange — h(0) then cx(0,1): the cx depends on q0, so no parallelism. q1
    // idles for the h window (35 ns); q2 idles the whole makespan (35+300 ns).
    DeviceModel dev = three_qubit_device();
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1);
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    // Act
    Circuit out = ALAPSchedulePass().run(timed);

    // Assert — per-qubit totals match the physical idle (same as per-gate here).
    EXPECT_NEAR(idle_total(out, 0), 0.0, 1e-9);
    EXPECT_NEAR(idle_total(out, 1), 35.0, 1e-9);
    EXPECT_NEAR(idle_total(out, 2), 335.0, 1e-9);
}

// ── Barrier is a synchronisation point (forces serialisation) ─────────────────

TEST(ALAPSchedule, Barrier_ForcesSerialisation) {
    // Arrange — a barrier between h(0) and h(1) prevents them from sharing a
    // slot, so q2 idles across both windows (35 + 35 = 70 ns), unlike the
    // parallel case above.
    DeviceModel dev = three_qubit_device();
    Circuit circ(3, 0);
    circ.h(0).barrier().h(1);
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    // Act
    Circuit out = ALAPSchedulePass().run(timed);

    // Assert — q2 decoheres over the full serialised span.
    EXPECT_NEAR(idle_total(out, 2), 70.0, 1e-9);
    EXPECT_NEAR(idle_total(out, 0), 35.0, 1e-9);  // q0 idles while h(1) runs
    EXPECT_NEAR(idle_total(out, 1), 35.0, 1e-9);  // q1 idles while h(0) runs
}

// ── Untimed circuit inserts no idle (device durations required) ───────────────

TEST(ALAPSchedule, UntimedCircuit_InsertsNothing) {
    // Arrange — no DurationAnnotationPass, so no gate carries a duration.
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1);

    // Act
    ALAPSchedulePass pass;
    Circuit out = pass.run(circ);

    // Assert
    EXPECT_FALSE(pass.changed());
    EXPECT_TRUE(idle_ops(out).empty());
}

// ── (#4): true ALAP places leading slack BEFORE the gate ──────

TEST(ALAPSchedule, TrueALAP_LeadingSlackIdleBeforeGate) {
    // cx(1,2) keeps q1 busy for 300 ns; h(0) then waits for q1 before cx(0,1),
    // so q0 has 265 ns of slack. True ALAP schedules h(0) LATE, so
    // q0's idle precedes h(0) (idle on |0>). ASAP would place it after h(0) (on
    // the prepared state), which over-dephases. Total idle is identical; only
    // the position differs — and position matters for the non-unitary channel.
    DeviceModel dev = three_qubit_device();
    Circuit circ(3, 0);
    circ.cx(1, 2).h(0).cx(0, 1);
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    Circuit out = ALAPSchedulePass().run(timed);

    int idle0_idx = -1, h0_idx = -1, i = 0;
    for (const Op& op : out.ops()) {
        if (idle0_idx < 0 && std::holds_alternative<IdleOp>(op) &&
            std::get<IdleOp>(op).qubit == 0) {
            idle0_idx = i;
        }
        if (std::holds_alternative<GateOp>(op)) {
            const GateOp& g = std::get<GateOp>(op);
            if (g.name == "h" && !g.qubits.empty() && g.qubits[0] == 0) h0_idx = i;
        }
        ++i;
    }
    EXPECT_TRUE(idle0_idx >= 0);
    EXPECT_TRUE(h0_idx >= 0);
    EXPECT_TRUE(idle0_idx < h0_idx);                 // idle BEFORE h (ALAP)
    EXPECT_NEAR(idle_total(out, 0), 265.0, 1e-9);    // total unchanged vs ASAP
}

// ── (#3): measurement duration counts bystander idle ──────────

TEST(ALAPSchedule, MeasureDuration_CountsBystanderIdle) {
    // A device that describes a measurement time makes the readout window advance
    // the schedule, so a bystander qubit idles during it. Pre-fix
    // MeasureOp was zero-time and no idle appeared.
    DeviceModel dev = DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "gates": {"measure": {"duration_ns": 1000.0}}
    })");
    Circuit circ(2, 1);
    circ.measure(0, 0);                 // q1 is a bystander during readout
    Circuit timed = DurationAnnotationPass(dev).run(circ);

    Circuit out = ALAPSchedulePass().run(timed);

    EXPECT_NEAR(idle_total(out, 1), 1000.0, 1e-9);   // idles the whole readout
}
