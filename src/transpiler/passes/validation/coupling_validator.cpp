/// @file coupling_validator.cpp
/// @brief CouplingValidatorPass implementation.
///
/// Verification-only pass: scans every two-qubit GateOp and throws
/// ConnectivityError when its (physical) operand pair is absent from the device
/// coupling map. transpile() runs it after routing (⑤ verify) so it is
/// normally a safety net confirming SwapRouterPass honoured the coupling
/// constraints; it also rejects circuits whose violations routing did not fix.

#include "transpiler/passes/validation/coupling_validator.hpp"

#include <set>
#include <string>
#include <utility>
#include <variant>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "error.hpp"

namespace qplayer {

CouplingValidatorPass::CouplingValidatorPass(const DeviceModel& device)
    : device_(&device) {}

std::string CouplingValidatorPass::name() const {
    return "CouplingValidatorPass";
}

Circuit CouplingValidatorPass::run(Circuit circuit) {
    changed_ = false;

    // No coupling map -> full connectivity -> nothing to validate.
    if (!device_->has_coupling_map()) {
        return circuit;
    }

    std::set<std::pair<QubitIndex, QubitIndex>> adj;
    for (const auto& [a, b] : device_->coupling_map()) {
        adj.emplace(a, b);
        adj.emplace(b, a);
    }

    for (const Op& op : circuit.ops()) {
        if (!std::holds_alternative<GateOp>(op)) continue;
        const GateOp& g = std::get<GateOp>(op);
        if (g.qubits.size() != 2) continue;  // 1Q/3Q not coupling-constrained
        if (adj.find({g.qubits[0], g.qubits[1]}) == adj.end()) {
            throw ConnectivityError{
                "CouplingValidatorPass: gate '" + g.name + "' on physical "
                "qubits " + std::to_string(g.qubits[0]) + "," +
                std::to_string(g.qubits[1]) +
                " violates the device coupling map"};
        }
    }
    return circuit;  // validation-only; circuit unchanged
}

bool CouplingValidatorPass::changed() const {
    return changed_;
}

}  // namespace qplayer
