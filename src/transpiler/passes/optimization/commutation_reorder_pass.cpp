// commutation_reorder_pass.cpp — CommutationReorderPass (Phase 4).
// Swaps adjacent GateOp pairs whose qubit sets are disjoint, making the circuit
// more amenable to CancelInversePass and MergeSingleQubitPass.
//
// Phase 4 scope: single forward sweep of adjacent pair swaps — no multi-pass
// bubble-sort or DAG analysis. Full DAG commutation is deferred to.
//
// Commutativity conditions checked here:
//   (1) Qubit-disjoint: two gates that touch entirely different physical qubits
//       always commute regardless of gate type (tensor product independence).
//   (2) Same-qubit diagonal: two diagonal gates on the same qubit commute, but
//       swapping them produces no structural benefit, so we skip that case.
//
// A swap is performed when the two consecutive GateOps are qubit-disjoint AND
// swapping them brings an earlier gate closer to a potential cancel/merge partner.
// Because we only do one pass the pass is idempotent after convergence; the
// PassManager may run it repeatedly until changed() returns false.
//
// Boundary ops (Measure, Barrier, If, Idle) terminate the reorder window.
// Opaque GateOps are treated as opaque barriers.
//

#include "transpiler/passes/optimization/commutation_reorder_pass.hpp"

#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

// Returns true when the two qubit lists share no element — disjoint qubit sets
// mean the two gates commute trivially (tensor product independence).
bool qubits_disjoint(const std::vector<QubitIndex>& a,
                     const std::vector<QubitIndex>& b) {
    for (auto qa : a) {
        for (auto qb : b) {
            if (qa == qb) return false;
        }
    }
    return true;
}

// Returns true when op is a non-gate boundary that terminates the reorder window.
bool is_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

// Returns true when the gate is opaque or a rotation gate — both act as soft
// boundaries for the reorder window.
bool is_opaque_or_rotation(const GateOp& g) {
    if (g.is_opaque) return true;
    switch (g.type) {
        case GateType::RX: case GateType::RY: case GateType::RZ:
        case GateType::U1: case GateType::U2: case GateType::U3:
        case GateType::CRZ: case GateType::CU1: case GateType::CU3:
            return true;
        default:
            return false;
    }
}

}  // anonymous namespace

std::string CommutationReorderPass::name() const {
    return "CommutationReorderPass";
}

// Single-pass adjacent swap: walk the op list and whenever two consecutive
// GateOps are qubit-disjoint, swap them. Mark changed_ = true on any swap.
// The heuristic favours swapping so that later gates with a matching type to
// an earlier gate are brought adjacent (enabling CancelInversePass cancellation
// in the next pipeline stage).
Circuit CommutationReorderPass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    std::vector<Op> ops(old_ops.begin(), old_ops.end());

    std::size_t n = ops.size();
    for (std::size_t i = 0; i + 1 < n; ++i) {
        // Both must be plain (non-boundary, non-opaque, non-rotation) GateOps.
        if (is_boundary(ops[i]) || is_boundary(ops[i + 1])) continue;

        if (!std::holds_alternative<GateOp>(ops[i]) ||
            !std::holds_alternative<GateOp>(ops[i + 1])) continue;

        const GateOp& gi   = std::get<GateOp>(ops[i]);
        const GateOp& gj   = std::get<GateOp>(ops[i + 1]);

        if (is_opaque_or_rotation(gi) || is_opaque_or_rotation(gj)) continue;

        if (!qubits_disjoint(gi.qubits, gj.qubits)) continue;

        // Swap the two ops — they commute because their qubit sets are disjoint.
        // Bring gates of the same type adjacent to enable CancelInversePass.
        // Only swap if gj.type == some earlier gate type OR gi.type == gj.type
        // for self-inverse pairing purposes. To avoid infinite oscillation in
        // a single pass, only swap when gj.type < gi.type (lexicographic on the
        // enum underlying integer) so the pass is monotone.
        //
        // Rationale for the ordering heuristic: a strict ordering guarantees the
        // single forward pass terminates and is deterministic. It also naturally
        // groups gates of the same type together (bubble-sort style), which is
        // the minimum needed for CancelInversePass to see adjacent pairs.
        if (static_cast<int>(gj.type) < static_cast<int>(gi.type)) {
            std::swap(ops[i], ops[i + 1]);
            changed_ = true;
        }
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool CommutationReorderPass::changed() const {
    return changed_;
}

}  // namespace qplayer
