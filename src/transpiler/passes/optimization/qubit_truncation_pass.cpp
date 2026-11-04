// qubit_truncation_pass.cpp — QubitTruncationPass. Compacts
// the physical qubits actually referenced by the circuit into a dense [0, k)
// range, rewrites every op's qubit operands, reduces Circuit::num_qubits to k,
// and stamps the dense->physical mapping into CircuitMetadata.qubit_truncation
// for noise synthesis. No-op when the referenced qubits are already
// the dense set [0, num_qubits).

#include "transpiler/passes/optimization/qubit_truncation_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "log.hpp"

namespace qplayer {

std::string QubitTruncationPass::name() const {
    return "QubitTruncationPass";
}

Circuit QubitTruncationPass::run(Circuit circuit) {
    changed_ = false;

    // Pauli expectation targets are full-width qubit strings whose remapping is
    // out of scope here; skip truncation rather than emit a mismatched circuit
    // (the device noise/counts workflow does not use expectations)..
    if (!circuit.expectation_targets().empty()) {
        return circuit;
    }

    // ── 1. Collect the set of physical qubits referenced by any op ───────────
    std::set<QubitIndex> used;  // ordered ascending -> gives the dense ranking
    auto note = [&](QubitIndex q) { used.insert(q); };
    for (const Op& op : circuit.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            for (QubitIndex q : g->qubits) note(q);
        } else if (const auto* m = std::get_if<MeasureOp>(&op)) {
            note(m->qubit);
        } else if (const auto* b = std::get_if<BarrierOp>(&op)) {
            for (QubitIndex q : b->qubits) note(q);   // empty = "all", contributes none
        } else if (const auto* iff = std::get_if<IfOp>(&op)) {
            for (QubitIndex q : iff->body.qubits) note(q);
        } else if (const auto* idle = std::get_if<IdleOp>(&op)) {
            note(idle->qubit);
        }
    }

    const QubitIndex k = static_cast<QubitIndex>(used.size());
    // Already dense: used == {0, 1, ..., num_qubits-1}. A valid circuit has all
    // operands < num_qubits, so used.size() == num_qubits implies exactly that.
    if (k == circuit.num_qubits()) {
        return circuit;
    }

    // ── 2. Build physical -> dense and dense -> physical maps ────────────────
    std::unordered_map<QubitIndex, QubitIndex> to_dense;
    std::unordered_map<QubitIndex, QubitIndex> dense_to_physical;
    to_dense.reserve(used.size());
    dense_to_physical.reserve(used.size());
    {
        QubitIndex dense = 0;
        for (QubitIndex phys : used) {   // std::set iterates ascending
            to_dense.emplace(phys, dense);
            dense_to_physical.emplace(dense, phys);
            ++dense;
        }
    }

    auto remap = [&](QubitIndex q) { return to_dense.at(q); };

    // ── 3. Rewrite every op's qubit operands (cubits untouched) ──────────────
    std::vector<Op> new_ops;
    new_ops.reserve(circuit.ops().size());
    for (const Op& op : circuit.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            GateOp ng = *g;
            for (QubitIndex& q : ng.qubits) q = remap(q);
            new_ops.push_back(std::move(ng));
        } else if (const auto* m = std::get_if<MeasureOp>(&op)) {
            MeasureOp nm = *m;
            nm.qubit = remap(nm.qubit);   // cubit preserved -> counts stay user-indexed
            new_ops.push_back(nm);
        } else if (const auto* b = std::get_if<BarrierOp>(&op)) {
            BarrierOp nb = *b;
            for (QubitIndex& q : nb.qubits) q = remap(q);
            new_ops.push_back(std::move(nb));
        } else if (const auto* iff = std::get_if<IfOp>(&op)) {
            IfOp ni = *iff;
            for (QubitIndex& q : ni.body.qubits) q = remap(q);
            new_ops.push_back(std::move(ni));
        } else if (const auto* idle = std::get_if<IdleOp>(&op)) {
            IdleOp nd = *idle;
            nd.qubit = remap(nd.qubit);
            new_ops.push_back(nd);
        } else {
            new_ops.push_back(op);
        }
    }

    // ── 4. Rebuild the circuit at width k, carrying the mapping in metadata ──
    CircuitMetadata meta = circuit.metadata();
    meta.qubit_truncation = std::move(dense_to_physical);

    changed_ = true;
    log::debug("QubitTruncationPass: compacted " +
               std::to_string(circuit.num_qubits()) + " -> " +
               std::to_string(k) + " qubits");

    return detail_make_circuit_from_payload(
        k,
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        std::move(meta));
}

bool QubitTruncationPass::changed() const {
    return changed_;
}

}  // namespace qplayer
