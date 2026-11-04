/// @file observable.hpp
/// @brief Weighted Pauli-sum observable H = Σ cᵢ Pᵢ.
/// @ingroup api
///
/// Header-only value type: the Sampler/Estimator facade (include/primitives.hpp)
/// and QRegister::expectation_value(const Observable&) consume it. Coefficients
/// are real (double) — a non-Hermitian operator's expectation is not a physical
/// measurement, so Estimator semantics are guaranteed by restricting to real
/// (complex coefficients deferred). Pauli qubit order matches
/// expectation_value(pauli): Qiskit big-endian (pauli[0] = qubit n-1).

#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "error.hpp"

namespace qplayer {

/// Weighted Pauli-sum observable H = Sum(c_i * P_i) consumed by
/// QRegister::expectation_value() and the Estimator facade.
class Observable {
public:
    /// Constructs an empty observable with no terms.
    Observable() = default;

    /// Build from a term list: Observable::from_list({{"ZZ", 0.5}, {"XX", 0.5}}).
    /// @param terms (Pauli string, coefficient) pairs appended in the given order.
    /// @return The assembled observable.
    /// @throws CircuitError Any term has an empty or non-I/X/Y/Z Pauli string.
    static Observable from_list(
        std::initializer_list<std::pair<std::string_view, double>> terms) {
        Observable obs;
        for (const auto& [pauli, coeff] : terms) {
            obs.add_term(coeff, pauli);
        }
        return obs;
    }

    /// Append one term (chainable). The length==num_qubits check is deferred to
    /// expectation evaluation where the circuit's qubit count is known.
    /// @param coeff Real weight c_i of this term.
    /// @param pauli Pauli string P_i in big-endian order (pauli[0] is qubit n-1).
    /// @return *this, so terms can be chained.
    /// @throws CircuitError pauli is empty or contains a char other than I/X/Y/Z.
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

    /// (coefficient, Pauli string) terms in insertion order.
    /// @return Reference to the term list owned by this observable.
    const std::vector<std::pair<double, std::string>>& terms() const noexcept {
        return terms_;
    }

    /// Number of terms currently held.
    /// @return Term count.
    std::size_t size() const noexcept { return terms_.size(); }

private:
    std::vector<std::pair<double, std::string>> terms_;
};

}  // namespace qplayer
