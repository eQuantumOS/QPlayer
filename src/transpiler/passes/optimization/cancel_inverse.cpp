/// @file cancel_inverse.cpp
/// @brief CancelInversePass implementation (Phase 4).
///
/// Removes adjacent self-inverse or mutually-inverse gate pairs on the same qubits.
/// Self-inverse: X, Y, Z, H, CX, CY, CZ, CH, SWAP, CCX, CSwap.
/// Mutually-inverse pairs: S/Sdg, T/Tdg, SX/SXdg.
/// Boundary ops (Measure, Barrier, If, Idle) flush the pending-ops stack.
/// Opaque GateOps and rotation gates (RX/RY/RZ/U1/U2/U3/CRZ/CU1/CU3) pass through
/// unchanged — angle-based cancellation is out of scope for this pass.
///

#include "transpiler/passes/optimization/cancel_inverse.hpp"

#include <cstddef>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

// Returns true when (a, b) is an adjacent cancellation pair on the same qubits.
// Both ops must be GateOps; qubit lists must match exactly.
static bool is_cancel_pair(const GateOp& a, const GateOp& b) {
    // Opaque or rotation gates are excluded — only plain built-in gates.
    if (a.is_opaque || b.is_opaque) return false;

    // Qubit lists must be identical.
    if (a.qubits != b.qubits) return false;

    // Self-inverse single-type pairs: X-X, Y-Y, Z-Z, H-H, CX-CX, CY-CY,
    // CZ-CZ, CH-CH, SWAP-SWAP, CCX-CCX, CSwap-CSwap, ECR-ECR. ECR is Hermitian
    // (ECR = (1/sqrt2)(IX - XY)), hence self-inverse: ECR^2 = I. The
    // exact ordered qubit-vector match above is required for ECR — ECR(a,b) is
    // NOT symmetric, so only ECR(a,b) followed by ECR(a,b) cancels, never (b,a).
    static constexpr GateType self_inverse[] = {
        GateType::X, GateType::Y, GateType::Z, GateType::H,
        GateType::CX, GateType::CY, GateType::CZ, GateType::CH,
        GateType::SWAP, GateType::CCX, GateType::CSwap, GateType::ECR
    };
    if (a.type == b.type) {
        for (auto t : self_inverse) {
            if (a.type == t) return true;
        }
        return false;
    }

    // Mutually-inverse pairs: S/Sdg, T/Tdg, SX/SXdg in either order.
    if ((a.type == GateType::S   && b.type == GateType::Sdg) ||
        (a.type == GateType::Sdg && b.type == GateType::S))   return true;
    if ((a.type == GateType::T   && b.type == GateType::Tdg) ||
        (a.type == GateType::Tdg && b.type == GateType::T))   return true;
    if ((a.type == GateType::SX   && b.type == GateType::SXdg) ||
        (a.type == GateType::SXdg && b.type == GateType::SX))  return true;

    return false;
}

// Returns true when the Op is a non-gate boundary that terminates the
// cancellation window (Measure, Barrier, If, Idle).
static bool is_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

std::string CancelInversePass::name() const {
    return "CancelInversePass";
}

// Algorithm: one forward pass using a stack of pending GateOps per cancellation
// window. When a new GateOp arrives and its top-of-stack peer is its inverse on
// the same qubits, both are cancelled (pop + skip). Otherwise the new op is
// pushed. Boundary ops flush the accumulated stack into the output list and
// are appended directly.
//
// Note: only the most-recently-pushed gate is compared against the incoming op
// (immediate-adjacency cancellation only). Commutation-based distant cancellation
// is the responsibility of CommutationReorderPass which should run
// before this pass in the full pipeline.
Circuit CancelInversePass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    std::vector<Op> output;
    output.reserve(old_ops.size());

    // pending: a window of consecutive GateOps accumulated between boundary ops.
    // We keep them in a vector (used as a stack — back = top) so we can examine
    // the top and cancel in O(1).
    std::vector<const GateOp*> pending_ptrs;  // pointers into old_ops (valid while circuit lives)
    std::vector<std::size_t>   pending_idx;   // corresponding indices into old_ops

    // Flush all accumulated pending ops into output.
    auto flush_pending = [&]() {
        for (std::size_t idx : pending_idx) {
            output.push_back(old_ops[idx]);
        }
        pending_ptrs.clear();
        pending_idx.clear();
    };

    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        const Op& op = old_ops[i];

        if (is_boundary(op)) {
            // Flush any buffered GateOps, then append the boundary itself.
            flush_pending();
            output.push_back(op);
            continue;
        }

        if (!std::holds_alternative<GateOp>(op)) {
            // Should not happen given the Op variant definition, but be safe.
            flush_pending();
            output.push_back(op);
            continue;
        }

        const GateOp& gate = std::get<GateOp>(op);

        // Opaque GateOps are passed through without entering the cancellation
        // window.
        if (gate.is_opaque) {
            flush_pending();
            output.push_back(op);
            continue;
        }

        // Rotation gates (params non-empty for built-in rotation gate types)
        // act as soft boundaries: they are not cancelled but they do not flush
        // the pending window either — they just pass through alongside others.
        // However to keep correctness simple we check the type explicitly.
        // Rotation gate types: RX, RY, RZ, U1, U2, U3, CRZ, CU1, CU3.
        // These pass through without triggering cancellation (no angle-based
        // identity check in this pass).
        bool is_rotation = (gate.type == GateType::RX  || gate.type == GateType::RY  ||
                            gate.type == GateType::RZ  || gate.type == GateType::U1  ||
                            gate.type == GateType::U2  || gate.type == GateType::U3  ||
                            gate.type == GateType::CRZ || gate.type == GateType::CU1 ||
                            gate.type == GateType::CU3);
        if (is_rotation) {
            // Flush window before the rotation so the rotation acts as a boundary
            // for the cancellation window.
            flush_pending();
            output.push_back(op);
            continue;
        }

        // Try to cancel against the top of the pending stack.
        if (!pending_ptrs.empty() &&
            is_cancel_pair(*pending_ptrs.back(), gate)) {
            // Cancel: remove the top of the stack and skip the current op.
            pending_ptrs.pop_back();
            pending_idx.pop_back();
            changed_ = true;
        } else {
            pending_ptrs.push_back(&gate);
            pending_idx.push_back(i);
        }
    }

    // Flush any remaining ops after the last boundary.
    flush_pending();

    // Build the output Circuit with the filtered op list.
    // Use detail_circuit_push_op to populate the new circuit's op list.
    Circuit result(circuit.num_qubits(), circuit.num_cubits());

    // Re-add classical registers from original circuit (beyond the default one).
    // The default register "c" was auto-created with num_cubits; for multi-creg
    // circuits we need to replicate additional registers.
    // Simpler approach: reconstruct the circuit via detail helpers if available.
    // Since Circuit only exposes add_creg for extra registers and the default one
    // is auto-created, we must handle the creg list carefully.
    //
    // Strategy: use detail_make_circuit_from_payload which reconstructs the
    // circuit from raw components (declared in circuit.hpp as a free function).
    const auto& regs  = circuit.classical_registers();
    const auto& opdefs = circuit.opaque_definitions();
    const auto& meta   = circuit.metadata();

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        regs,
        opdefs,
        std::move(output),
        circuit.expectation_targets(),
        meta);
}

bool CancelInversePass::changed() const {
    return changed_;
}

}  // namespace qplayer
