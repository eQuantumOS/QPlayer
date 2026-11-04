/// @file primitives.hpp
/// @brief Qiskit-compatible Sampler / Estimator facades.
/// @ingroup api
///
/// Header-only thin wrappers over existing entry points (no backend code):
///   Estimator -> QRegister::run() + expectation_value(Observable) (exact path)
/// Sampler -> QRegister::run().counts() (lazy sampling)
/// Because QPlayer is a simulator, Estimator evaluates Sum(c_i*<P_i>) directly on
/// the statevector — no measurement-basis-rotation circuits as Qiskit needs.

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "circuit/circuit.hpp"
#include "observable.hpp"
#include "qregister.hpp"

namespace qplayer {

/// Estimator: expectation value <psi|H|psi> of an observable H = Sum(c_i*P_i).
/// The default path is exact (statevector evaluation, no shot noise). The circuit
/// should prepare the state (no terminal measurement) — after run() the backend
/// holds |psi_final> on the fast path, which expectation_value(obs) reads.
/// Qiskit-compatible Estimator facade: evaluates Sum(c_i * <P_i>) exactly on
/// the simulated state, with no basis-rotation circuits.
class Estimator {
public:
    /// Binds the facade to a register; the register must outlive the Estimator.
    /// @param qr Register whose backend evaluates the observable.
    explicit Estimator(QRegister& qr) : qr_(qr) {}

    /// Runs the circuit once and evaluates the observable on the final state.
    /// @param circuit State-preparation circuit; it should carry no terminal
    ///                measurement, since collapsing destroys the state the
    ///                evaluation reads.
    /// @param obs     Observable H = Sum(c_i * P_i) to evaluate.
    /// @return The exact expectation value, free of shot noise.
    double run(const Circuit& circuit, const Observable& obs) {
        qr_.run(circuit, /*nshots=*/1);   // exact: no sampling, state = |psi_final>
        return qr_.expectation_value(obs);
    }

private:
    QRegister& qr_;
};

/// Sampler: measurement distribution. Thin wrapper over Result::counts().
/// Qiskit-compatible Sampler facade: wraps QRegister::run() and returns
/// measurement quasi-distributions.
class Sampler {
public:
    /// Binds the facade to a register; the register must outlive the Sampler.
    /// @param qr Register that executes the circuit.
    explicit Sampler(QRegister& qr) : qr_(qr) {}

    /// Runs the circuit and returns its measurement distribution.
    /// @param circuit Circuit to execute; it should carry measurements.
    /// @param nshots  Number of shots to draw.
    /// @return Counts keyed by bit-string, MSB->LSB in creg layout.
    std::map<std::string, std::uint64_t>
    run(const Circuit& circuit, std::uint32_t nshots = 1024) {
        return qr_.run(circuit, nshots).counts();
    }

private:
    QRegister& qr_;
};

}  // namespace qplayer
