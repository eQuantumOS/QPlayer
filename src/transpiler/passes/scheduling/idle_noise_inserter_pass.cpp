// idle_noise_inserter_pass.cpp — IdleNoiseInserterPass implementation
// One linear scan: copy each op, and after every
// timed GateOp insert an IdleOp{q, duration_ns} for each non-participating
// qubit. Gates without a duration contribute no idle noise (
// limitation — duration is supplied by DurationAnnotationPass / the device).
// Per-gate model; ALAP slot-grouping deferred.

#include "transpiler/passes/scheduling/idle_noise_inserter_pass.hpp"

#include <algorithm>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

// GCC 13 emits a false-positive -Wmaybe-uninitialized for the
// `push_back(Op{IdleOp{...}})` form when -O2 inlines the variant constructor and
// walks the GateOp arm of the union. The IdleOp alternative is fully brace-
// initialised; clang + GCC <= 12 do not report it. Same suppression idiom as
// src/circuit/circuit.cpp and src/parser/circuit_builder.cpp (no real defect).
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace qplayer {

std::string IdleNoiseInserterPass::name() const {
    return "IdleNoiseInserterPass";
}

Circuit IdleNoiseInserterPass::run(Circuit circuit) {
    changed_ = false;

    const QubitIndex n      = circuit.num_qubits();
    const auto&      old_ops = circuit.ops();
    std::vector<Op>  new_ops;
    // Each timed gate can spawn up to n idle ops, so reserve the worst case to
    // avoid repeated reallocation as the vector grows past old_ops.size().
    new_ops.reserve(old_ops.size() * (static_cast<std::size_t>(n) + 1));

    for (const Op& op : old_ops) {
        new_ops.push_back(op);

        if (!std::holds_alternative<GateOp>(op)) continue;
        const GateOp& g = std::get<GateOp>(op);

        // Only timed gates drive idle noise; an empty duration means the device
        // did not annotate this gate, so no idle window is known.
        if (!g.duration_ns.has_value()) continue;
        const double d = *g.duration_ns;

        // Every qubit not participating in g idles for its duration. Operand
        // lists are tiny, so a linear membership check is fine.
        for (QubitIndex q = 0; q < n; ++q) {
            if (std::find(g.qubits.begin(), g.qubits.end(), q) != g.qubits.end()) {
                continue;
            }
            new_ops.push_back(Op{IdleOp{q, d}});
            changed_ = true;
        }
    }

    if (!changed_) {
        return circuit;  // no timed gates / single-qubit-only on full register
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool IdleNoiseInserterPass::changed() const {
    return changed_;
}

}  // namespace qplayer

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic pop
#endif
