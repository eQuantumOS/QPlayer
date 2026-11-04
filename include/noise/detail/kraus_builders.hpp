/// @file kraus_builders.hpp
/// @brief Inline single-qubit Kraus operator constructors shared
/// @ingroup noise
///
/// by NoiseModel (channel registration) and T1T2Calculator (T1/T2 -> Kraus,
///). Header-only so both translation units build the same 2x2 operators
/// without duplication. detail namespace:
/// not part of the public API.

#pragma once

#include <cmath>
#include <complex>
#include <vector>

#include "noise/noise_model.hpp"  // UnitaryMatrix

namespace qplayer::detail {

/// Amplitude type of every Kraus operator entry.
using Complex = std::complex<double>;

// Row-major 2x2 single-qubit operator builder.
inline UnitaryMatrix mat2(Complex a, Complex b, Complex c, Complex d) {
    return {{a, b}, {c, d}};
}

// Depolarizing: {sqrt(1-p) I, sqrt(p/3) X, sqrt(p/3) Y, sqrt(p/3) Z}.
// Qiskit-compatible convention where p is the total error probability.
inline std::vector<UnitaryMatrix> depolarizing_ops(double p) {
    const double a = std::sqrt(1.0 - p);
    const double b = std::sqrt(p / 3.0);
    return {
        mat2({a, 0}, {0, 0}, {0, 0}, {a, 0}),    // sqrt(1-p) I
        mat2({0, 0}, {b, 0}, {b, 0}, {0, 0}),    // sqrt(p/3) X
        mat2({0, 0}, {0, -b}, {0, b}, {0, 0}),   // sqrt(p/3) Y
        mat2({b, 0}, {0, 0}, {0, 0}, {-b, 0}),   // sqrt(p/3) Z
    };
}

// Two-qubit depolarizing: {sqrt(1-p) I⊗I} plus the 15
// non-identity 2-qubit Paulis each scaled by sqrt(p/15). Row-major 4x4 operators;
// local bit 0 = first operand, bit 1 = second. Average gate error r = p*d/(d+1)
// with d=4 (r = 4p/5), matching the Pauli-p convention of depolarizing_ops.
inline std::vector<UnitaryMatrix> depolarizing_2q_ops(double p) {
    // Single-qubit Pauli basis {I, X, Y, Z} as 2x2 complex.
    const UnitaryMatrix paulis[4] = {
        mat2({1, 0}, {0, 0}, {0, 0}, {1, 0}),    // I
        mat2({0, 0}, {1, 0}, {1, 0}, {0, 0}),    // X
        mat2({0, 0}, {0, -1}, {0, 1}, {0, 0}),   // Y
        mat2({1, 0}, {0, 0}, {0, 0}, {-1, 0}),   // Z
    };
    // Kron(A, B): 4x4 with local bit 1 = A's qubit (high), bit 0 = B's qubit (low),
    // i.e. row/col index = 2*bitA + bitB. element [2a+b][2c+d] = A[a][c]*B[b][d].
    auto kron = [](const UnitaryMatrix& A, const UnitaryMatrix& B, double s) {
        UnitaryMatrix M(4, std::vector<Complex>(4, Complex{0, 0}));
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b)
                for (int c = 0; c < 2; ++c)
                    for (int d = 0; d < 2; ++d)
                        M[2 * a + b][2 * c + d] = s * A[a][c] * B[b][d];
        return M;
    };
    const double s_id  = std::sqrt(1.0 - p);
    const double s_err = std::sqrt(p / 15.0);
    std::vector<UnitaryMatrix> ops;
    ops.reserve(16);
    ops.push_back(kron(paulis[0], paulis[0], s_id));   // sqrt(1-p) I⊗I
    for (int hi = 0; hi < 4; ++hi) {
        for (int lo = 0; lo < 4; ++lo) {
            if (hi == 0 && lo == 0) continue;          // skip I⊗I
            ops.push_back(kron(paulis[hi], paulis[lo], s_err));
        }
    }
    return ops;
}

// Amplitude damping, gamma: K0 = diag(1, sqrt(1-g)), K1 = sqrt(g)|0><1|.
inline std::vector<UnitaryMatrix> amplitude_damping_ops(double gamma) {
    const double s = std::sqrt(1.0 - gamma);
    const double g = std::sqrt(gamma);
    return {
        mat2({1, 0}, {0, 0}, {0, 0}, {s, 0}),
        mat2({0, 0}, {g, 0}, {0, 0}, {0, 0}),
    };
}

// Phase damping / dephasing, lam: K0 = diag(1, sqrt(1-l)), K1 = sqrt(l)|1><1|.
inline std::vector<UnitaryMatrix> phase_damping_ops(double lam) {
    const double s = std::sqrt(1.0 - lam);
    const double l = std::sqrt(lam);
    return {
        mat2({1, 0}, {0, 0}, {0, 0}, {s, 0}),
        mat2({0, 0}, {0, 0}, {0, 0}, {l, 0}),
    };
}

// Bit flip: {sqrt(1-p) I, sqrt(p) X}.
inline std::vector<UnitaryMatrix> bit_flip_ops(double p) {
    const double a = std::sqrt(1.0 - p);
    const double b = std::sqrt(p);
    return {
        mat2({a, 0}, {0, 0}, {0, 0}, {a, 0}),
        mat2({0, 0}, {b, 0}, {b, 0}, {0, 0}),
    };
}

// Phase flip: {sqrt(1-p) I, sqrt(p) Z}.
inline std::vector<UnitaryMatrix> phase_flip_ops(double p) {
    const double a = std::sqrt(1.0 - p);
    const double b = std::sqrt(p);
    return {
        mat2({a, 0}, {0, 0}, {0, 0}, {a, 0}),
        mat2({b, 0}, {0, 0}, {0, 0}, {-b, 0}),
    };
}

}  // namespace qplayer::detail
