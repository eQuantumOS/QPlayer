// tn_backend.cpp — Phase 0-S stub for TensorNetworkBackend
// Interactive is NOT supported on TN.

#include "backend/tensor_network/tn_backend.hpp"

#include "error.hpp"

namespace qplayer {

namespace {
constexpr const char* kPhase = "Phase 9 pending";
}  // namespace

TensorNetworkBackend::TensorNetworkBackend()  = default;
TensorNetworkBackend::~TensorNetworkBackend() = default;

void TensorNetworkBackend::prepare(const Circuit& /*circuit*/) {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

Result TensorNetworkBackend::execute(const Circuit&     /*circuit*/,
                                     std::uint32_t      /*nshots*/,
                                     const NoiseModel*  /*noise*/) {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

// Interactive surface — Phase 9 replaces these with a permanent SimulatorError.
void TensorNetworkBackend::init_state(QubitIndex /*num_qubits*/) {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

void TensorNetworkBackend::apply_gate(const GateOp& /*op*/) {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

int TensorNetworkBackend::apply_measure(QubitIndex /*qubit*/) {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

void TensorNetworkBackend::apply_reset(QubitIndex /*qubit*/) {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

StateVector TensorNetworkBackend::get_statevector() {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

RawStateView TensorNetworkBackend::get_statevector_raw() {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

double TensorNetworkBackend::expectation_value(std::string_view /*pauli*/) {
    throw NotImplementedError{"TensorNetwork", kPhase};
}

// nonzero_amplitudes() — TensorNetwork does not support this API.
// TN is batch-only with no incremental state; throws SimulatorError (not stub).
std::vector<std::pair<StateIndex, std::complex<double>>>
TensorNetworkBackend::nonzero_amplitudes() const {
    throw SimulatorError{"TensorNetwork", "nonzero_amplitudes not supported"};
}

// num_qubits() — TN is batch-only (no init_state); always returns 0.
QubitIndex TensorNetworkBackend::num_qubits() const {
    return QubitIndex{0};
}

SimType          TensorNetworkBackend::sim_type()       const { return SimType::TensorNetwork; }
bool             TensorNetworkBackend::supports_noise() const { return true;  }
bool             TensorNetworkBackend::supports_gpu()   const { return true;  }
std::string_view TensorNetworkBackend::name()           const { return "TensorNetwork"; }

// max_qubits() — TN storage is unbounded; report the dense-equivalent host
// bound as a densification guidance value (정책 계승).
std::uint32_t TensorNetworkBackend::max_qubits(detail::WorkingSet ws) const noexcept {
    return qplayer::detail::max_qubits_for_budget(ws);
}

}  // namespace qplayer
