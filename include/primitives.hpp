// primitives.hpp — Qiskit-compatible Sampler / Estimator facades.
// Header-only thin wrappers over existing entry points (no backend code):
//   Estimator -> QRegister::run() + expectation_value(Observable) (exact path)
// Sampler -> QRegister::run().counts() (lazy sampling)
// Because QPlayer is a simulator, Estimator evaluates Sum(c_i*<P_i>) directly on
// the statevector — no measurement-basis-rotation circuits as Qiskit needs.

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "circuit/circuit.hpp"
#include "observable.hpp"
#include "qregister.hpp"

namespace qplayer {

// Estimator: expectation value <psi|H|psi> of an observable H = Sum(c_i*P_i).
// The default path is exact (statevector evaluation, no shot noise). The circuit
// should prepare the state (no terminal measurement) — after run() the backend
// holds |psi_final> on the fast path, which expectation_value(obs) reads.
class Estimator {
public:
    explicit Estimator(QRegister& qr) : qr_(qr) {}

    double run(const Circuit& circuit, const Observable& obs) {
        qr_.run(circuit, /*nshots=*/1);   // exact: no sampling, state = |psi_final>
        return qr_.expectation_value(obs);
    }

private:
    QRegister& qr_;
};

// Sampler: measurement distribution. Thin wrapper over Result::counts().
class Sampler {
public:
    explicit Sampler(QRegister& qr) : qr_(qr) {}

    std::map<std::string, std::uint64_t>
    run(const Circuit& circuit, std::uint32_t nshots = 1024) {
        return qr_.run(circuit, nshots).counts();
    }

private:
    QRegister& qr_;
};

}  // namespace qplayer
