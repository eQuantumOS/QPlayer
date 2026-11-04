// gate_matrix.hpp — 2x2 complex gate matrix derivation shared by FullState CPU
// and SparseState backends. Extracted from full_state_cpu.cpp so the
// sparse-state apply_gate() can reuse the same table without code duplication.
// Header-only (inline + constexpr): no target_sources registration required.
// All symbols in qplayer::detail to keep them off the public API surface
// (— internal helpers use detail:: namespace).

#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "circuit/gate_op.hpp"  // GateType

namespace qplayer::detail {

// ECR (Echoed Cross-Resonance) 4x4 unitary, single source of truth shared by
// the FullState CPU dense-2Q kernel, runtime fusion, and the GPU launcher so the
// numbers never drift. Row-major 32 doubles = 16 (re,im) pairs;
// element [r][c] = data[8*r + 2*c] + i*data[8*r + 2*c + 1]. Basis index bit 0 =
// operand qubit 0 (qubits[0], LSB), bit 1 = operand qubit 1 (qubits[1]).
// ECR = (1/sqrt2)(IX - XY) = (1/sqrt2)[[0,1,0,i],[1,0,-i,0],[0,i,0,1],[-i,0,1,0]]
// (Qiskit ECRGate convention). Not self-inverse, so inverse is handled explicitly.
inline const std::array<double, 32>& ecr_matrix_data() {
    static const double s = M_SQRT1_2;
    static const std::array<double, 32> m = {{
        0,0,   s,0,   0,0,   0,s,     // row 0: [0,  1,  0,  i]
        s,0,   0,0,   0,-s,  0,0,     // row 1: [1,  0, -i,  0]
        0,0,   0,s,   0,0,   s,0,     // row 2: [0,  i,  0,  1]
        0,-s,  0,0,   s,0,   0,0,     // row 3: [-i, 0,  1,  0]
    }};
    return m;
}

// iSWAP 4x4 unitary, single source of truth. Qiskit/Cirq convention:
// [[1,0,0,0],[0,0,i,0],[0,i,0,0],[0,0,0,1]]. Not self-inverse (iSWAP^2 ≠ I);
// iSWAP^† = iSWAP* (conjugate). Row-major 32 doubles, same layout as ecr_matrix_data().
inline const std::array<double, 32>& iswap_matrix_data() {
    static const std::array<double, 32> m = {{
        1,0,  0,0,  0,0,  0,0,   // row 0: [1, 0, 0, 0]
        0,0,  0,0,  0,1,  0,0,   // row 1: [0, 0, i, 0]
        0,0,  0,1,  0,0,  0,0,   // row 2: [0, i, 0, 0]
        0,0,  0,0,  0,0,  1,0,   // row 3: [0, 0, 0, 1]
    }};
    return m;
}

// sqrt_iSWAP 4x4 unitary, single source of truth. Qiskit/Cirq convention:
// [[1,0,0,0],[0,1/sqrt2,i/sqrt2,0],[0,i/sqrt2,1/sqrt2,0],[0,0,0,1]]. Not self-inverse;
// applying twice yields iSWAP. Row-major 32 doubles, same layout as ecr_matrix_data().
inline const std::array<double, 32>& sqrt_iswap_matrix_data() {
    static const double s = M_SQRT1_2;
    static const std::array<double, 32> m = {{
        1,0,  0,0,  0,0,  0,0,   // row 0: [1,      0,      0, 0]
        0,0,  s,0,  0,s,  0,0,   // row 1: [0, 1/√2, i/√2,  0]
        0,0,  0,s,  s,0,  0,0,   // row 2: [0, i/√2,  1/√2, 0]
        0,0,  0,0,  0,0,  1,0,   // row 3: [0,      0,      0, 1]
    }};
    return m;
}

// fSim(theta, phi) 4x4 unitary. Parametric — not a static constant.
// [[1,0,0,0],[0,cosθ,-i sinθ,0],[0,-i sinθ,cosθ,0],[0,0,0,e^{-iφ}]].
// e^{-iφ} = cosφ - i sinφ. Sycamore = fSim(pi/2, pi/6).
// Row-major 32 doubles, same layout as ecr_matrix_data(). Not self-inverse.
inline std::array<double, 32> fsim_matrix_data(double theta, double phi) {
    const double ct  = std::cos(theta);
    const double st  = std::sin(theta);
    const double cp  = std::cos(phi);
    const double sp  = std::sin(phi);
    return {{
        1,0,    0,0,     0,0,    0,0,    // row 0: [1,      0,        0, 0    ]
        0,0,    ct,0,    0,-st,  0,0,    // row 1: [0,  cosθ, -i sinθ, 0    ]
        0,0,    0,-st,   ct,0,   0,0,    // row 2: [0, -i sinθ,  cosθ, 0    ]
        0,0,    0,0,     0,0,    cp,-sp, // row 3: [0,      0,        0, e^{-iφ}]
    }};
}

// 2x2 complex matrix stored as four pairs of real/imaginary components.
// Row-major layout: U = [[u00, u01], [u10, u11]] where U[i][j] = uIJr + i*uIJi.
// Real and imaginary parts are separated for FMA-friendly computation.
struct Mat2x2 {
    double u00r, u00i;  // U[0][0]
    double u01r, u01i;  // U[0][1]
    double u10r, u10i;  // U[1][0]
    double u11r, u11i;  // U[1][1]
};

// Derive the 2x2 unitary sub-matrix for a gate type.
// For controlled gates (CX, CY, ..., CCX), returns the target-qubit sub-unitary;
// control masking is the caller's responsibility.
// SWAP / CSwap / ECR / ISWAP / SQRT_ISWAP / FSIM / Reset / Opaque are NOT
// handled here — the caller must dispatch those before calling gate_matrix().
// ECR/ISWAP/SQRT_ISWAP/FSIM are genuine 4x4; see ecr_matrix_data(),
// iswap_matrix_data(), sqrt_iswap_matrix_data(), fsim_matrix_data() (0610).
// Param order: RX/RY/RZ = [theta], U1 = [lambda], U2 = [phi, lambda],
// U3 = [theta, phi, lambda] (matches qelib1.inc argument order).
inline Mat2x2 gate_matrix(GateType type,
                           const std::vector<double>& params) {
    // 1/sqrt(2) = M_SQRT1_2; avoids repeated std::sqrt(0.5) calls.
    const double INV_SQRT2 = M_SQRT1_2;

    switch (type) {
        // I gate — identity, no change.
        case GateType::I:
            return {1,0, 0,0, 0,0, 1,0};

        case GateType::X:
            return {0,0, 1,0, 1,0, 0,0};

        // Y = [[0,-i],[i,0]]: U[0][1] = -i => (0,-1), U[1][0] = i => (0,+1).
        case GateType::Y:
            return {0,0, 0,-1, 0,1, 0,0};

        case GateType::Z:
            return {1,0, 0,0, 0,0, -1,0};

        // H = (1/sqrt(2)) * [[1,1],[1,-1]]
        case GateType::H:
            return {INV_SQRT2,0, INV_SQRT2,0, INV_SQRT2,0, -INV_SQRT2,0};

        // S = [[1,0],[0,i]]
        case GateType::S:
            return {1,0, 0,0, 0,0, 0,1};

        // Sdg = [[1,0],[0,-i]]
        case GateType::Sdg:
            return {1,0, 0,0, 0,0, 0,-1};

        // T = [[1,0],[0,e^{i*pi/4}]] = [[1,0],[0,(1+i)/sqrt(2)]]
        case GateType::T:
            return {1,0, 0,0, 0,0, INV_SQRT2, INV_SQRT2};

        // Tdg = [[1,0],[0,e^{-i*pi/4}]] = [[1,0],[0,(1-i)/sqrt(2)]]
        case GateType::Tdg:
            return {1,0, 0,0, 0,0, INV_SQRT2, -INV_SQRT2};

        // SX = sqrt(X) = (1/2)[[1+i,1-i],[1-i,1+i]]. SX*SX = X.
        case GateType::SX:
            return {0.5,0.5, 0.5,-0.5, 0.5,-0.5, 0.5,0.5};

        // SXdg = SX dagger = (1/2)[[1-i,1+i],[1+i,1-i]]. SX*SXdg = I.
        case GateType::SXdg:
            return {0.5,-0.5, 0.5,0.5, 0.5,0.5, 0.5,-0.5};

        // RX(theta) = [[cos(t/2), -i*sin(t/2)], [-i*sin(t/2), cos(t/2)]]
        case GateType::RX: {
            const double t = params[0];
            const double c = std::cos(t * 0.5);
            const double s = std::sin(t * 0.5);
            return {c,0, 0,-s, 0,-s, c,0};
        }

        // RY(theta) = [[cos(t/2), -sin(t/2)], [sin(t/2), cos(t/2)]]
        case GateType::RY: {
            const double t = params[0];
            const double c = std::cos(t * 0.5);
            const double s = std::sin(t * 0.5);
            return {c,0, -s,0, s,0, c,0};
        }

        // RZ(theta) = [[e^{-i*t/2},0],[0,e^{i*t/2}]]
        // e^{-i*t/2} = cos(t/2) - i*sin(t/2), e^{i*t/2} = cos(t/2) + i*sin(t/2).
        case GateType::RZ: {
            const double t = params[0];
            const double c = std::cos(t * 0.5);
            const double s = std::sin(t * 0.5);
            return {c,-s, 0,0, 0,0, c,s};
        }

        // U1(lambda) = [[1,0],[0,e^{i*lambda}]]
        case GateType::U1: {
            const double lam = params[0];
            return {1,0, 0,0, 0,0, std::cos(lam), std::sin(lam)};
        }

        // U2(phi,lambda) = (1/sqrt(2)) * [[1,-e^{i*lam}],[e^{i*phi},e^{i*(phi+lam)}]]
        case GateType::U2: {
            const double phi = params[0];
            const double lam = params[1];
            const double cp  = std::cos(phi),   sp  = std::sin(phi);
            const double cl  = std::cos(lam),   sl  = std::sin(lam);
            const double cpl = std::cos(phi + lam), spl = std::sin(phi + lam);
            return {INV_SQRT2,0,
                    -INV_SQRT2*cl, -INV_SQRT2*sl,
                    INV_SQRT2*cp,   INV_SQRT2*sp,
                    INV_SQRT2*cpl,  INV_SQRT2*spl};
        }

        // U3(theta,phi,lambda): general single-qubit gate (qelib1.inc).
        // [[cos(t/2),          -e^{i*lam}*sin(t/2)],
        //  [e^{i*phi}*sin(t/2), e^{i*(phi+lam)}*cos(t/2)]]
        // Verify: U3(pi,0,pi)=X, U2(0,pi)=H, U1(pi)=Z.
        case GateType::U3: {
            const double th  = params[0];
            const double phi = params[1];
            const double lam = params[2];
            const double ct  = std::cos(th * 0.5);
            const double st  = std::sin(th * 0.5);
            const double cp  = std::cos(phi),   sp  = std::sin(phi);
            const double cl  = std::cos(lam),   sl  = std::sin(lam);
            const double cpl = std::cos(phi + lam), spl = std::sin(phi + lam);
            return {ct,    0,
                    -cl*st, -sl*st,
                    cp*st,   sp*st,
                    cpl*ct,  spl*ct};
        }

        // Controlled 1Q gates: return target-qubit sub-unitary only.
        // Control masking is handled by the calling kernel.
        case GateType::CX:   return {0,0, 1,0, 1,0, 0,0};         // X
        case GateType::CY:   return {0,0, 0,-1, 0,1, 0,0};        // Y
        case GateType::CZ:   return {1,0, 0,0, 0,0, -1,0};        // Z
        case GateType::CH:   return {INV_SQRT2,0, INV_SQRT2,0,     // H
                                     INV_SQRT2,0, -INV_SQRT2,0};

        // CRZ(lambda) — target is RZ(lambda).
        case GateType::CRZ: {
            const double lam = params[0];
            const double c   = std::cos(lam * 0.5);
            const double s   = std::sin(lam * 0.5);
            return {c,-s, 0,0, 0,0, c,s};
        }

        // CU1(lambda) — target is U1(lambda).
        case GateType::CU1: {
            const double lam = params[0];
            return {1,0, 0,0, 0,0, std::cos(lam), std::sin(lam)};
        }

        // CU3(theta,phi,lambda) — target is U3(theta,phi,lambda).
        case GateType::CU3: {
            const double th  = params[0];
            const double phi = params[1];
            const double lam = params[2];
            const double ct  = std::cos(th * 0.5);
            const double st  = std::sin(th * 0.5);
            const double cp  = std::cos(phi),   sp  = std::sin(phi);
            const double cl  = std::cos(lam),   sl  = std::sin(lam);
            const double cpl = std::cos(phi + lam), spl = std::sin(phi + lam);
            return {ct,    0,
                    -cl*st, -sl*st,
                    cp*st,   sp*st,
                    cpl*ct,  spl*ct};
        }

        // CCX (Toffoli): target sub-unitary is X.
        case GateType::CCX:  return {0,0, 1,0, 1,0, 0,0};

        default:
            // SWAP, CSwap, ECR, ISWAP, SQRT_ISWAP, FSIM, Reset, Opaque must be
            // handled before gate_matrix() — they are genuine 4x4 gates and have
            // no 2x2 sub-unitary. Return identity only to satisfy the compiler;
            // reaching this branch is a caller contract violation.
            return {1,0, 0,0, 0,0, 1,0};
    }
}

}  // namespace qplayer::detail
