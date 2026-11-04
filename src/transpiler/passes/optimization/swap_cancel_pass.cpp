/// @file swap_cancel_pass.cpp
/// @brief SwapCancelPass implementation (second optimization round).
///
/// Cancels redundant SWAP pairs left by routing: for each atomic SWAP{a,b}, scan
/// forward; if the next op that touches a or b is another SWAP on the same
/// unordered pair, both are removed (SWAP·SWAP = I commuting past disjoint ops).
/// Any other op touching a or b blocks the cancellation.

#include "transpiler/passes/optimization/swap_cancel_pass.hpp"

#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

// Unordered pair equality for a two-qubit SWAP.
bool same_pair(const std::vector<QubitIndex>& x, QubitIndex a, QubitIndex b) {
    return x.size() == 2 &&
           ((x[0] == a && x[1] == b) || (x[0] == b && x[1] == a));
}

// True when `op` acts on physical qubit a or b. A BarrierOp with an empty qubit
// list means "all qubits" and therefore always touches the pair.
bool touches(const Op& op, QubitIndex a, QubitIndex b) {
    if (std::holds_alternative<GateOp>(op)) {
        for (QubitIndex q : std::get<GateOp>(op).qubits) {
            if (q == a || q == b) return true;
        }
        return false;
    }
    if (std::holds_alternative<MeasureOp>(op)) {
        const QubitIndex q = std::get<MeasureOp>(op).qubit;
        return q == a || q == b;
    }
    if (std::holds_alternative<BarrierOp>(op)) {
        const auto& qs = std::get<BarrierOp>(op).qubits;
        if (qs.empty()) return true;  // all-qubit barrier
        for (QubitIndex q : qs) {
            if (q == a || q == b) return true;
        }
        return false;
    }
    if (std::holds_alternative<IfOp>(op)) {
        for (QubitIndex q : std::get<IfOp>(op).body.qubits) {
            if (q == a || q == b) return true;
        }
        return false;
    }
    // IdleOp
    const QubitIndex q = std::get<IdleOp>(op).qubit;
    return q == a || q == b;
}

bool is_atomic_swap(const Op& op) {
    return std::holds_alternative<GateOp>(op) &&
           std::get<GateOp>(op).type == GateType::SWAP &&
           !std::get<GateOp>(op).is_opaque;
}

}  // namespace

std::string SwapCancelPass::name() const {
    return "SwapCancelPass";
}

Circuit SwapCancelPass::run(Circuit circuit) {
    changed_ = false;

    const auto& ops = circuit.ops();
    const std::size_t n = ops.size();
    std::vector<bool> removed(n, false);

    for (std::size_t i = 0; i < n; ++i) {
        if (removed[i] || !is_atomic_swap(ops[i])) continue;

        const auto& q = std::get<GateOp>(ops[i]).qubits;
        const QubitIndex a = q[0];
        const QubitIndex b = q[1];

        // Find the next op touching a or b. If it is the matching SWAP, cancel.
        for (std::size_t j = i + 1; j < n; ++j) {
            if (removed[j] || !touches(ops[j], a, b)) continue;
            if (is_atomic_swap(ops[j]) &&
                same_pair(std::get<GateOp>(ops[j]).qubits, a, b)) {
                removed[i] = true;
                removed[j] = true;
                changed_  = true;
            }
            break;  // first touching op decides: cancel or block
        }
    }

    if (!changed_) {
        return circuit;
    }

    std::vector<Op> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!removed[i]) out.push_back(ops[i]);
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(out),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool SwapCancelPass::changed() const {
    return changed_;
}

}  // namespace qplayer
