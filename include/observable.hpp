// observable.hpp — Weighted Pauli-sum observable H = Σ cᵢ Pᵢ.
// Header-only value type: the Sampler/Estimator facade (include/primitives.hpp)
// and QRegister::expectation_value(const Observable&) consume it. Coefficients
// are real (double) — a non-Hermitian operator's expectation is not a physical
// measurement, so Estimator semantics are guaranteed by restricting to real
// (complex coefficients deferred to). Pauli qubit order matches
// expectation_value(pauli): Qiskit big-endian (pauli[0] = qubit n-1).

#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "error.hpp"

namespace qplayer {

class Observable {
public:
    Observable() = default;

    // Build from a term list: Observable::from_list({{"ZZ", 0.5}, {"XX", 0.5}}).
    // throws: CircuitError — any term has an empty or non-I/X/Y/Z Pauli string.
    static Observable from_list(
        std::initializer_list<std::pair<std::string_view, double>> terms) {
        Observable obs;
        for (const auto& [pauli, coeff] : terms) {
            obs.add_term(coeff, pauli);
        }
        return obs;
    }

    // Append one term (chainable). The length==num_qubits check is deferred to
    // expectation evaluation where the circuit's qubit count is known.
    // throws: CircuitError — pauli is empty or contains a char other than I/X/Y/Z.
    Observable& add_term(double coeff, std::string_view pauli) {
        if (pauli.empty()) {
            throw CircuitError{"Observable::add_term: Pauli string must not be empty"};
        }
        for (char ch : pauli) {
            if (ch != 'I' && ch != 'X' && ch != 'Y' && ch != 'Z') {
                throw CircuitError{std::string("Observable::add_term: invalid Pauli char '") +
                                   ch + "' (only I/X/Y/Z allowed)"};
            }
        }
        terms_.emplace_back(coeff, std::string{pauli});
        return *this;
    }

    // (coefficient, Pauli string) terms in insertion order.
    const std::vector<std::pair<double, std::string>>& terms() const noexcept {
        return terms_;
    }

    std::size_t size() const noexcept { return terms_.size(); }

private:
    std::vector<std::pair<double, std::string>> terms_;
};

}  // namespace qplayer
