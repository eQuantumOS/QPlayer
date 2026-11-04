/// @file alap_schedule.cpp
/// @brief ALAPSchedulePass implementation (,
///
/// supersedes deferral). Schedules the circuit into time slots from the
/// per-gate durations stamped by DurationAnnotationPass, then materialises each
/// qubit's idle windows as IdleOp nodes — the Qiskit ALAPScheduleAnalysis +
/// PadDelay idiom in one pass. This replaces the per-gate IdleNoiseInserterPass
/// in the device-noise pipeline: gates that run in parallel share a time slot, so
/// a bystander qubit idles once for the slot span instead of once per foreign
/// gate (which double-counted idle in deep parallel circuits — sc17-willow d=3
/// measured a 5.0x inflation of the per-cycle logical error, 68% of it idle).
///
/// Model: a list schedule assigns each op a start = max over its qubits of their
/// current free time (ASAP); the op occupies [start, start+duration]. A qubit's
/// idle gap before an op it joins, and its trailing gap up to a barrier / the
/// circuit makespan, each become one IdleOp{q, gap}. Barriers (and the circuit
/// end) are full synchronisation points. Gates without a duration contribute no
/// time, so an untimed circuit schedules to makespan 0 and no idle is inserted
/// (limitation preserved: idle needs device durations).

#include "transpiler/passes/scheduling/alap_schedule.hpp"

#include <algorithm>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

// GCC 13 emits a false-positive -Wmaybe-uninitialized for push_back(Op{IdleOp{}})
// under -O2 (same idiom / suppression as idle_noise_inserter_pass.cpp).
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace qplayer {

namespace {

// Qubits an op occupies for scheduling. Barriers are handled by the caller.
std::vector<QubitIndex> op_qubits(const Op& op) {
    if (std::holds_alternative<GateOp>(op)) return std::get<GateOp>(op).qubits;
    if (std::holds_alternative<MeasureOp>(op)) return {std::get<MeasureOp>(op).qubit};
    if (std::holds_alternative<IdleOp>(op)) return {std::get<IdleOp>(op).qubit};
    if (std::holds_alternative<IfOp>(op)) return std::get<IfOp>(op).body.qubits;
    return {};
}

// Time an op occupies its qubits. Timed GateOps (incl. reset — a GateOp{Reset}),
// IdleOps, and MeasureOps (duration annotated by the device) advance
// the clock; barrier / if are zero-time ordering events. Unannotated ops are 0.
double op_duration(const Op& op) {
    if (std::holds_alternative<GateOp>(op)) {
        const GateOp& g = std::get<GateOp>(op);
        return g.duration_ns.has_value() ? *g.duration_ns : 0.0;
    }
    if (std::holds_alternative<IdleOp>(op)) return std::get<IdleOp>(op).duration_ns;
    if (std::holds_alternative<MeasureOp>(op)) {
        const MeasureOp& m = std::get<MeasureOp>(op);
        return m.duration_ns.has_value() ? *m.duration_ns : 0.0;   // (#3)
    }
    return 0.0;
}

}  // namespace

std::string ALAPSchedulePass::name() const {
    return "ALAPSchedulePass";
}

Circuit ALAPSchedulePass::run(Circuit circuit) {
    changed_ = false;

    const QubitIndex     n       = circuit.num_qubits();
    const auto&          old_ops = circuit.ops();
    const std::size_t    N       = old_ops.size();
    const std::size_t    nn      = static_cast<std::size_t>(n);
    if (n == 0 || N == 0) return circuit;

    // Qubits an op occupies for scheduling; a barrier with an empty list spans
    // the whole register (full sync). Unlike ASAP, barriers are handled inline
    // as zero-duration ops whose whole qubit set aligns (start = group max).
    auto occ = [&](const Op& op) -> std::vector<QubitIndex> {
        if (std::holds_alternative<BarrierOp>(op)) {
            const std::vector<QubitIndex>& bq = std::get<BarrierOp>(op).qubits;
            if (!bq.empty()) return bq;
            std::vector<QubitIndex> all(nn);
            for (QubitIndex q = 0; q < n; ++q) all[static_cast<std::size_t>(q)] = q;
            return all;
        }
        return op_qubits(op);
    };

    // ── Pass 1: forward ASAP → makespan T ────────────────────────────────────
    // start = latest free time among the op's qubits; a barrier's whole set
    // aligns to that max (its zero duration leaves them synchronised).
    std::vector<double> avail(nn, 0.0);
    for (const Op& op : old_ops) {
        const std::vector<QubitIndex> qs = occ(op);
        double start = 0.0;
        for (QubitIndex q : qs) start = std::max(start, avail[static_cast<std::size_t>(q)]);
        const double end = start + op_duration(op);
        for (QubitIndex q : qs) avail[static_cast<std::size_t>(q)] = end;
    }
    const double T = *std::max_element(avail.begin(), avail.end());

    // ── Pass 2: reverse ASAP → each op's ALAP (latest) start ─────────────────
    // Running the forward list-schedule on the reversed op stream yields, per
    // op, its distance from the circuit end; barriers stay symmetric syncs
    // (nothing crosses them on their qubits), so this mirrors true ALAP exactly
    // alap_start = T - rev_end keeps each op as late as its
    // successors allow, so a qubit's leading slack becomes idle BEFORE its first
    // op (on |0>) instead of after a state-preparing gate (ASAP, pessimistic).
    std::vector<double> rev_avail(nn, 0.0);
    std::vector<double> alap_start(N, 0.0);
    for (std::size_t idx = N; idx-- > 0;) {
        const Op& op = old_ops[idx];
        const std::vector<QubitIndex> qs = occ(op);
        double rstart = 0.0;
        for (QubitIndex q : qs) rstart = std::max(rstart, rev_avail[static_cast<std::size_t>(q)]);
        const double rend = rstart + op_duration(op);
        for (QubitIndex q : qs) rev_avail[static_cast<std::size_t>(q)] = rend;
        alap_start[idx] = T - rend;
    }

    // ── Pass 3: forward emit, inserting each qubit's idle gap before its op ───
    std::vector<Op> new_ops;
    new_ops.reserve(N * 2);
    std::vector<double> last_end(nn, 0.0);
    auto flush_idle = [&](QubitIndex q, double target) {
        const double gap = target - last_end[static_cast<std::size_t>(q)];
        if (gap > 0.0) {
            new_ops.push_back(Op{IdleOp{q, gap}});
            last_end[static_cast<std::size_t>(q)] = target;
            changed_ = true;
        }
    };
    for (std::size_t idx = 0; idx < N; ++idx) {
        const Op& op = old_ops[idx];
        const std::vector<QubitIndex> qs = occ(op);
        const double s = alap_start[idx];
        for (QubitIndex q : qs) flush_idle(q, s);
        new_ops.push_back(op);
        const double e = s + op_duration(op);
        for (QubitIndex q : qs) last_end[static_cast<std::size_t>(q)] = e;
    }
    // Circuit end: qubits that finished before the makespan idled while the
    // critical path ran (in ALAP this is usually only fully-unused qubits).
    for (QubitIndex q = 0; q < n; ++q) flush_idle(q, T);

    if (!changed_) {
        return circuit;  // untimed circuit or single-qubit-only on full register.
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool ALAPSchedulePass::changed() const {
    return changed_;
}

}  // namespace qplayer

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic pop
#endif
