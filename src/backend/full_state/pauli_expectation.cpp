/// @file pauli_expectation.cpp
/// @brief shared host-side <psi|P|psi> primitive.
///
/// See pauli_expectation.hpp for the contract.

#include "backend/full_state/pauli_expectation.hpp"

#include <cstdint>
#include <string>

#include "error.hpp"

namespace qplayer {
namespace detail {

std::vector<PauliOp> compile_pauli_ops(std::string_view pauli, QubitIndex n_qubits) {
    // The local string must outlive the view for the rest of this call.
    std::string all_z;
    if (pauli.empty()) {
        all_z.assign(static_cast<std::size_t>(n_qubits), 'Z');
        pauli = all_z;
    }
    if (pauli.size() != static_cast<std::size_t>(n_qubits)) {
        throw CircuitError{"expectation_value: Pauli string length (" +
                           std::to_string(pauli.size()) + ") != num_qubits (" +
                           std::to_string(n_qubits) + ")"};
    }

    std::vector<PauliOp> ops(static_cast<std::size_t>(n_qubits), PauliOp::I);
    for (std::size_t k = 0; k < pauli.size(); ++k) {
        const QubitIndex q = n_qubits - 1u - static_cast<QubitIndex>(k);  // Qiskit order
        switch (pauli[k]) {
            case 'I': ops[q] = PauliOp::I; break;
            case 'X': ops[q] = PauliOp::X; break;
            case 'Y': ops[q] = PauliOp::Y; break;
            case 'Z': ops[q] = PauliOp::Z; break;
            default:
                throw CircuitError{
                    std::string("expectation_value: invalid Pauli char '") +
                    pauli[k] + "' (only I/X/Y/Z allowed)"};
        }
    }
    return ops;
}

// Mask form for the dense contraction, derived from the per-qubit factors so
// that the two representations cannot drift apart (one parse, one validation).
PauliSpec compile_pauli(std::string_view pauli, QubitIndex n_qubits) {
    const std::vector<PauliOp> ops = compile_pauli_ops(pauli, n_qubits);

    PauliSpec s;
    int num_y = 0;
    for (QubitIndex q = 0; q < n_qubits; ++q) {
        switch (ops[q]) {
            case PauliOp::I: break;
            case PauliOp::X: s.flip_mask |= (StateIndex{1} << q); break;
            case PauliOp::Y: s.flip_mask |= (StateIndex{1} << q);
                             s.sign_mask |= (StateIndex{1} << q); ++num_y; break;
            case PauliOp::Z: s.sign_mask |= (StateIndex{1} << q); break;
        }
    }

    // Global phase i^num_y from the Y factors; the per-index bit dependence is
    // carried separately by sign_mask.
    switch (num_y & 3) {
        case 0: s.ph_re =  1.0; s.ph_im =  0.0; break;
        case 1: s.ph_re =  0.0; s.ph_im =  1.0; break;
        case 2: s.ph_re = -1.0; s.ph_im =  0.0; break;
        case 3: s.ph_re =  0.0; s.ph_im = -1.0; break;
    }
    return s;
}

std::vector<PauliSpec> compile_targets(const std::vector<std::string>& targets,
                                       QubitIndex n_qubits) {
    std::vector<PauliSpec> specs;
    specs.reserve(targets.size());
    for (const std::string& p : targets) {
        specs.push_back(compile_pauli(p, n_qubits));
    }
    return specs;
}

// <psi|P|psi> = sum_j conj(psi[j ^ flip]) * coeff(j) * psi[j], where
// coeff(j) = i^num_y * (-1)^popcount(j & sign_mask).
double pauli_expectation(const double* re, const double* im,
                         StateIndex dim, const PauliSpec& s) {
    const std::int64_t idim = static_cast<std::int64_t>(dim);
    double acc_re = 0.0;

#pragma omp parallel for reduction(+:acc_re) schedule(static)
    for (std::int64_t jj = 0; jj < idim; ++jj) {
        const StateIndex j  = static_cast<StateIndex>(jj);
        const StateIndex jp = j ^ s.flip_mask;
        // Parity over sign-mask bits; n <= ~35 so the low 64 bits hold all of them.
        const int parity = __builtin_popcountll(
            static_cast<std::uint64_t>(j & s.sign_mask)) & 1;
        const double sgn  = parity ? -1.0 : 1.0;
        const double c_re = sgn * s.ph_re;
        const double c_im = sgn * s.ph_im;
        const double bj_re = re[j],  bj_im = im[j];
        const double cp_re = re[jp], cp_im = -im[jp];  // conj(psi[jp])
        const double t1_re = c_re * bj_re - c_im * bj_im;
        const double t1_im = c_re * bj_im + c_im * bj_re;
        // P Hermitian -> the imaginary accumulator is numerical noise; skipping
        // it saves a flop per amplitude and cannot change the returned value.
        acc_re += cp_re * t1_re - cp_im * t1_im;
    }
    return acc_re;
}

}  // namespace detail
}  // namespace qplayer
