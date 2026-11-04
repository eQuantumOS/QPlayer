// tn_backend.hpp — TensorNetwork backend declaration.
// MPS / general TN storage. Interactive mode is NOT supported — init_state /
// apply_gate / apply_measure / apply_reset throw SimulatorError at runtime
// (backend matrix). Batch-only via execute().

#pragma once

#include "backend/simulator_backend.hpp"

namespace qplayer {

class TensorNetworkBackend : public SimulatorBackend {
public:
    TensorNetworkBackend();
    ~TensorNetworkBackend() override;

    void   prepare(const Circuit& circuit) override;
    Result execute(const Circuit& circuit, std::uint32_t nshots,
                   const NoiseModel* noise = nullptr) override;

    // Interactive surface — always throws SimulatorError. Kept overridden so
    // QRegister::interactive can dispatch uniformly and trigger the error
    // at the first user call.
    void init_state   (QubitIndex num_qubits) override;
    void apply_gate   (const GateOp& op)      override;
    int  apply_measure(QubitIndex qubit)      override;
    void apply_reset  (QubitIndex qubit)      override;

    StateVector  get_statevector()                              override;
    RawStateView get_statevector_raw()                          override;
    double       expectation_value(std::string_view pauli)      override;

    // TensorNetwork does not support nonzero_amplitudes() — throws SimulatorError
    // num_qubits() always returns 0 (TN is batch-only, no init_state).
    std::vector<std::pair<StateIndex, std::complex<double>>>
            nonzero_amplitudes() const override;
    QubitIndex num_qubits() const override;

    SimType          sim_type()       const override;
    bool             supports_noise() const override;
    bool             supports_gpu()   const override;
    std::string_view name()           const override;

    // TN storage is unbounded; report the dense-equivalent host bound as a
    // densification guidance value (정책 계승).
    std::uint32_t max_qubits(detail::WorkingSet ws) const noexcept override;
};

}  // namespace qplayer
