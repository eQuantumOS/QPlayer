// initial_layout_pass.cpp — InitialLayoutPass implementation
// Stamps CircuitMetadata.layout with a logical->physical mapping
// (Trivial identity by default, or a caller-supplied permutation) and copies
// the device coupling map into the metadata. Op qubit indices are left as-is;
// physical routing is the SwapRouterPass's job.

#include "transpiler/passes/layout/initial_layout_pass.hpp"

#include <string>
#include <unordered_set>
#include <utility>

#include "circuit/circuit.hpp"
#include "circuit/metadata.hpp"
#include "error.hpp"

namespace qplayer {

InitialLayoutPass::InitialLayoutPass(const DeviceModel& device)
    : device_(&device) {}

InitialLayoutPass::InitialLayoutPass(const DeviceModel&      device,
                                     std::vector<QubitIndex> hints)
    : device_(&device), hints_(std::move(hints)) {}

std::string InitialLayoutPass::name() const {
    return "InitialLayoutPass";
}

Circuit InitialLayoutPass::run(Circuit circuit) {
    changed_ = false;

    const QubitIndex n      = circuit.num_qubits();
    const QubitIndex dev_n  = device_->num_qubits();

    // Physical-qubit space the layout maps into. A device with a coupling map
    // has a fixed topology of dev_n qubits; a full-connectivity device (no
    // coupling map) imposes no width limit, so the space simply accommodates
    // the circuit (and any larger enumerated qubit count).
    const QubitIndex phys_space =
        device_->has_coupling_map() ? dev_n : (dev_n > n ? dev_n : n);

    // A circuit cannot occupy more physical qubits than the topology provides
    // Reject loudly rather than producing an unroutable layout.
    if (n > phys_space) {
        throw CircuitError{
            "InitialLayoutPass: circuit uses " + std::to_string(n) +
            " qubits but device describes only " + std::to_string(dev_n)};
    }

    QubitLayout layout;
    if (hints_.has_value()) {
        const std::vector<QubitIndex>& hints = *hints_;
        // Hints index by logical qubit, so the vector must cover the circuit.
        if (hints.size() != static_cast<std::size_t>(n)) {
            throw CircuitError{
                "InitialLayoutPass: layout hints size " +
                std::to_string(hints.size()) + " != circuit qubit count " +
                std::to_string(n)};
        }
        // Validate permutation: each physical qubit in device range and unique.
        std::unordered_set<QubitIndex> seen;
        seen.reserve(hints.size());
        for (QubitIndex logical = 0; logical < n; ++logical) {
            const QubitIndex physical = hints[logical];
            if (physical >= phys_space) {
                throw CircuitError{
                    "InitialLayoutPass: layout hint maps logical qubit " +
                    std::to_string(logical) + " to physical " +
                    std::to_string(physical) + " beyond device range " +
                    std::to_string(dev_n)};
            }
            if (!seen.insert(physical).second) {
                throw CircuitError{
                    "InitialLayoutPass: layout hints are not a permutation; "
                    "physical qubit " + std::to_string(physical) +
                    " assigned more than once"};
            }
            layout.logical_to_physical.emplace(logical, physical);
            layout.physical_to_logical.emplace(physical, logical);
        }
    } else {
        // Trivial identity layout (default).
        layout = make_identity_layout(n);
    }

    CircuitMetadata meta = circuit.metadata();
    meta.layout = std::move(layout);
    // Carry the connectivity forward so SwapRouterPass and serialisation see it.
    // An absent/empty coupling map means full connectivity — leave it unset.
    if (device_->has_coupling_map()) {
        meta.coupling_map = device_->coupling_map();
    }

    changed_ = true;
    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::vector<Op>(circuit.ops()),
        circuit.expectation_targets(),
        std::move(meta));
}

bool InitialLayoutPass::changed() const {
    return changed_;
}

}  // namespace qplayer
