// duration_annotation_pass.cpp — DurationAnnotationPass implementation
// One linear scan: every GateOp
// with an empty duration_ns gets the DeviceModel's gate duration when the
// device describes that gate on those operands; pre-set durations and
// device-unknown gates are left untouched (the fallback path).

#include "transpiler/passes/analysis/duration_annotation_pass.hpp"

#include <utility>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

DurationAnnotationPass::DurationAnnotationPass(const DeviceModel& device)
    : device_(&device) {}

std::string DurationAnnotationPass::name() const {
    return "DurationAnnotationPass";
}

Circuit DurationAnnotationPass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    std::vector<Op> new_ops;
    new_ops.reserve(old_ops.size());

    for (const Op& op : old_ops) {
        if (std::holds_alternative<GateOp>(op)) {
            GateOp g = std::get<GateOp>(op);
            // Fill only when empty (respect any pre-set duration) and the device
            // actually describes this gate on these operands. Unknown
            // gate/qubit -> leave empty (fallback, no idle-noise timing).
            if (!g.duration_ns.has_value() &&
                device_->has_gate_property(g.name, g.qubits)) {
                g.duration_ns = device_->gate_property(g.name, g.qubits).duration_ns;
                changed_ = true;
            }
            new_ops.push_back(std::move(g));
        } else if (std::holds_alternative<MeasureOp>(op)) {
            MeasureOp m = std::get<MeasureOp>(op);
            // Measurement duration (#3): fill from the device "measure"
            // gate property so ALAP counts readout time as bystander idle. Reset
            // is a GateOp{Reset} and is handled by the GateOp branch above via its
            // "reset" gate key. Left empty when the device omits measure duration
            // (current device JSONs do) -> zero-time, unchanged behaviour.
            if (!m.duration_ns.has_value() &&
                device_->has_gate_property("measure", {m.qubit})) {
                m.duration_ns = device_->gate_property("measure", {m.qubit}).duration_ns;
                changed_ = true;
            }
            new_ops.push_back(std::move(m));
        } else {
            new_ops.push_back(op);
        }
    }

    if (!changed_) {
        return circuit;  // nothing annotated — return unchanged
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool DurationAnnotationPass::changed() const {
    return changed_;
}

}  // namespace qplayer
