// kraus_apply_2q.hpp — Monte Carlo application of a two-qubit (4x4) Kraus channel
// to a SoA state vector. Header-only + detail-scoped so the
// FullState CPU trajectory path and the GPU host-side Kraus path invoke the exact
// same routine, guaranteeing CPU<->GPU statistical parity (contract).
//
// Algorithm mirrors the 1x1 (2x2) path in full_state_cpu.cpp:
//   1. For each Kraus operator K_i compute p_i = ||K_i|psi>||^2 over all quads.
//   2. Draw u ~ U[0,1) and pick the first operator whose cumulative p exceeds u.
//   3. Apply the chosen operator in place and renormalise by sqrt(p_chosen).
// Local bit convention: q0 = bit 0, q1 = bit 1, so the gathered amplitude index
// is x = 2*bit(q1) + bit(q0) (matches ecr_matrix_data / depolarizing_2q_ops).

#pragma once

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "types.hpp"
#include "noise/noise_model.hpp"   // UnitaryMatrix

namespace qplayer::detail {

// Apply a 4x4 Kraus channel `ops` on qubits (q0, q1) to the SoA buffers.
inline void apply_kraus_2q(double* re, double* im, StateIndex dim,
                           QubitIndex q0, QubitIndex q1,
                           const std::vector<UnitaryMatrix>& ops,
                           std::mt19937_64& rng) {
    const QubitIndex lo = q0 < q1 ? q0 : q1;
    const QubitIndex hi = q0 < q1 ? q1 : q0;
    const StateIndex lo_mask  = (StateIndex{1} << lo) - 1;
    const StateIndex mid_mask = ((StateIndex{1} << (hi - 1)) - 1) & ~lo_mask;
    const StateIndex bit0 = StateIndex{1} << q0;
    const StateIndex bit1 = StateIndex{1} << q1;
    const std::int64_t quads = static_cast<std::int64_t>(dim >> 2);
    const std::size_t nops = ops.size();

    auto quad_index = [&](std::int64_t qq, StateIndex out[4]) {
        const StateIndex g = static_cast<StateIndex>(qq);
        const StateIndex base = (g & lo_mask)
            | ((g & mid_mask) << 1)
            | ((g >> (hi - 1)) << (hi + 1));
        out[0] = base;
        out[1] = base | bit0;
        out[2] = base | bit1;
        out[3] = base | bit0 | bit1;
    };

    // Step 1: p_i = ||K_i|psi>||^2.
    std::vector<double> probs(nops, 0.0);
    for (std::size_t ki = 0; ki < nops; ++ki) {
        const UnitaryMatrix& K = ops[ki];
        double pi = 0.0;
        for (std::int64_t qq = 0; qq < quads; ++qq) {
            StateIndex idx[4];
            quad_index(qq, idx);
            double ar[4], ai[4];
            for (int c = 0; c < 4; ++c) { ar[c] = re[idx[c]]; ai[c] = im[idx[c]]; }
            for (int r = 0; r < 4; ++r) {
                double nr = 0.0, ni = 0.0;
                for (int c = 0; c < 4; ++c) {
                    const double kr = K[r][c].real(), kimag = K[r][c].imag();
                    nr += kr * ar[c] - kimag * ai[c];
                    ni += kr * ai[c] + kimag * ar[c];
                }
                pi += std::fma(nr, nr, ni * ni);
            }
        }
        probs[ki] = pi;
    }

    // Step 2-3: pick operator by cumulative probability (one draw, as the 2x2 path).
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    const double u = dist(rng);
    double cumsum = 0.0;
    std::size_t chosen = 0;
    for (std::size_t ki = 0; ki < nops; ++ki) {
        cumsum += probs[ki];
        if (u < cumsum) { chosen = ki; break; }
        chosen = ki;   // fallback to last on floating-point rounding
    }

    // Step 4: apply chosen operator in place.
    const UnitaryMatrix& Kc = ops[chosen];
    for (std::int64_t qq = 0; qq < quads; ++qq) {
        StateIndex idx[4];
        quad_index(qq, idx);
        double ar[4], ai[4];
        for (int c = 0; c < 4; ++c) { ar[c] = re[idx[c]]; ai[c] = im[idx[c]]; }
        for (int r = 0; r < 4; ++r) {
            double nr = 0.0, ni = 0.0;
            for (int c = 0; c < 4; ++c) {
                const double kr = Kc[r][c].real(), kimag = Kc[r][c].imag();
                nr += kr * ar[c] - kimag * ai[c];
                ni += kr * ai[c] + kimag * ar[c];
            }
            re[idx[r]] = nr; im[idx[r]] = ni;
        }
    }

    // Renormalise by ||K_chosen|psi>|| = sqrt(p_chosen).
    const double p_chosen = probs[chosen];
    if (p_chosen < 1e-15) return;
    const double inv_norm = 1.0 / std::sqrt(p_chosen);
    const std::int64_t idim = static_cast<std::int64_t>(dim);
    for (std::int64_t ii = 0; ii < idim; ++ii) { re[ii] *= inv_norm; im[ii] *= inv_norm; }
}

}  // namespace qplayer::detail
