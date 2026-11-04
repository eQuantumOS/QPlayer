/// @file tn_sampling.cpp
/// @brief Perfect sampling from an MPS chain.
///
/// An MPS factorises p(s_0..s_{n-1}) into conditionals, so a shot is drawn one
/// site at a time: pick s_0 from its marginal, then s_1 given s_0, and so on.
/// Every draw is exact — there is no rejection step and no 2^n array — which is
/// what puts counts() past MAX_DENSE_EXPORT_QUBITS for this backend.
///
/// Getting p(s_i | prefix) needs the rest of the chain summed out, so the right
/// environments are built once up front. Both sweeps here are split into two
/// steps ((env x A) then (x A*)) so the cost is O(n * chi^3) and not the
/// O(n * chi^4) that a nested left^2 x right^2 loop would pay — the same
/// restructure site_marginal() needed in tn_noise.cpp, for the same reason.

#include "backend/tensor_network/tn_sampling.hpp"

#include "error.hpp"

#include <cmath>
#include <string>

namespace qplayer {
namespace detail {

namespace {

using Cx = std::complex<double>;

constexpr const char* kBackend = "TensorNetwork";

// Site tensor index positions — the [left, physical, right] layout.
constexpr std::uint32_t kLeft  = 0;
constexpr std::uint32_t kRight = 2;

// Row-major offset inside a [left, 2, right] site tensor.
inline std::size_t site_at(std::size_t a, std::size_t s, std::size_t b, std::size_t right) {
    return (a * 2 + s) * right + b;
}

}  // namespace

MpsSampler::MpsSampler(const TensorGraph& chain)
    : chain_(&chain), n_(chain.node_count()) {
    // Structural check first: every loop below indexes indices[0..2] and assumes
    // neighbouring bonds agree. A malformed graph would otherwise read out of
    // bounds or, worse, contract something meaningless into a plausible number.
    if (!chain.is_mps_chain()) {
        throw SimulatorError{kBackend, "sampling requires a well-formed MPS chain"};
    }

    right_.assign(n_ + 1, std::vector<Cx>{});
    right_[n_] = {Cx{1.0, 0.0}};  // closed right boundary, dim 1

    // R[k][b * dim + bp] = sum over sites k..n-1 of A x conj(A), swept inward.
    for (std::size_t k = n_; k-- > 0;) {
        const TensorNode&  A     = chain.node(static_cast<std::uint32_t>(k));
        const std::size_t  left  = A.indices[kLeft].dim;
        const std::size_t  right = A.indices[kRight].dim;
        const std::vector<Cx>& R = right_[k + 1];
        if (R.size() != right * right) {
            throw SimulatorError{kBackend, "sampling found an inconsistent bond at site " +
                                           std::to_string(k)};
        }

        // Step 1: U[a][s][bp] = sum_b A[a,s,b] * R[b][bp].
        std::vector<Cx> U(left * 2 * right, Cx{0.0, 0.0});
        for (std::size_t a = 0; a < left; ++a) {
            for (std::size_t s = 0; s < 2; ++s) {
                for (std::size_t b = 0; b < right; ++b) {
                    const Cx av = A.data[site_at(a, s, b, right)];
                    if (av == Cx{0.0, 0.0}) continue;
                    for (std::size_t bp = 0; bp < right; ++bp) {
                        U[(a * 2 + s) * right + bp] += av * R[b * right + bp];
                    }
                }
            }
        }
        // Step 2: R'[a][ap] = sum_{s,bp} U[a][s][bp] * conj(A[ap,s,bp]).
        std::vector<Cx> next(left * left, Cx{0.0, 0.0});
        for (std::size_t a = 0; a < left; ++a) {
            for (std::size_t s = 0; s < 2; ++s) {
                for (std::size_t bp = 0; bp < right; ++bp) {
                    const Cx uv = U[(a * 2 + s) * right + bp];
                    if (uv == Cx{0.0, 0.0}) continue;
                    for (std::size_t ap = 0; ap < left; ++ap) {
                        next[a * left + ap] += uv * std::conj(A.data[site_at(ap, s, bp, right)]);
                    }
                }
            }
        }
        right_[k] = std::move(next);
    }

    // The left boundary is dim 1, so R[0] is the 1x1 scalar <psi|psi>.
    squared_norm_ = right_[0].empty() ? 0.0 : right_[0][0].real();
    if (!std::isfinite(squared_norm_) || squared_norm_ <= 0.0) {
        throw SimulatorError{kBackend,
            "cannot sample a chain with |psi|^2 = " + std::to_string(squared_norm_)};
    }
}

std::vector<int> MpsSampler::sample(std::mt19937_64& rng) const {
    std::vector<int> bits;
    sample_into(rng, bits);
    return bits;
}

void MpsSampler::sample_into(std::mt19937_64& rng, std::vector<int>& bits) const {
    bits.assign(n_, 0);

    // v carries the prefix already drawn, contracted into the ket bond, and is
    // renormalised at every site so that <v|R[i]|v> == 1 going in.
    //
    // The draw does not need this: p0/(p0+p1) is invariant under any scaling of
    // v, and at the 128-qubit structural ceiling an unnormalised prefix weight
    // only reaches 2^-128, nowhere near underflow. What it buys is that `total`
    // below stays O(1), so the degeneracy guard tests divergence against a fixed
    // scale instead of against a number that legitimately shrinks with depth.
    std::vector<Cx> v{Cx{1.0, 0.0}};
    std::vector<Cx> w0, w1, t;
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    for (std::size_t site = 0; site < n_; ++site) {
        const TensorNode& A     = chain_->node(static_cast<std::uint32_t>(site));
        const std::size_t left  = A.indices[kLeft].dim;
        const std::size_t right = A.indices[kRight].dim;
        if (v.size() != left) {
            throw SimulatorError{kBackend, "sampling found an inconsistent bond at site " +
                                           std::to_string(site)};
        }

        // w_s[b] = sum_a v[a] * A[a,s,b] — the prefix extended by physical value s.
        w0.assign(right, Cx{0.0, 0.0});
        w1.assign(right, Cx{0.0, 0.0});
        for (std::size_t a = 0; a < left; ++a) {
            const Cx va = v[a];
            if (va == Cx{0.0, 0.0}) continue;
            for (std::size_t b = 0; b < right; ++b) {
                w0[b] += va * A.data[site_at(a, 0, b, right)];
                w1[b] += va * A.data[site_at(a, 1, b, right)];
            }
        }

        // p_s = <w_s| R[site+1] |w_s>, in two steps so this stays O(chi^2).
        const std::vector<Cx>& R = right_[site + 1];
        const auto branch_weight = [&](const std::vector<Cx>& w) {
            t.assign(right, Cx{0.0, 0.0});
            for (std::size_t b = 0; b < right; ++b) {
                const Cx wb = w[b];
                if (wb == Cx{0.0, 0.0}) continue;
                for (std::size_t bp = 0; bp < right; ++bp) t[bp] += wb * R[b * right + bp];
            }
            double acc = 0.0;
            for (std::size_t bp = 0; bp < right; ++bp) acc += (t[bp] * std::conj(w[bp])).real();
            // Rounding can push a vanishing branch a few ulp negative; a negative
            // probability would flip the comparison below, so clamp rather than
            // propagate. Genuine divergence is caught by the finiteness test.
            return acc > 0.0 ? acc : 0.0;
        };

        const double p0 = branch_weight(w0);
        const double p1 = branch_weight(w1);
        const double total = p0 + p1;
        if (!std::isfinite(total) || total <= 0.0) {
            throw SimulatorError{kBackend,
                "sampling hit a degenerate conditional at site " + std::to_string(site) +
                " (p0 + p1 = " + std::to_string(total) + ")"};
        }
        // total is the prefix weight, recomputed one site further in. Dividing by
        // it is what makes the draw conditional; at site 0 it is <psi|psi>, which
        // truncation leaves slightly off 1, so it is never assumed to be 1.
        // Exactly one uniform per site: the seed contract does not depend on the
        // state.
        const int chosen = (u01(rng) * total < p0) ? 0 : 1;
        bits[site] = chosen;

        const std::vector<Cx>& w = (chosen == 0) ? w0 : w1;
        const double picked = (chosen == 0) ? p0 : p1;
        const double scale  = 1.0 / std::sqrt(picked);
        v.assign(right, Cx{0.0, 0.0});
        for (std::size_t b = 0; b < right; ++b) v[b] = w[b] * scale;
    }
}

std::uint64_t MpsSampler::environment_bytes() const noexcept {
    std::uint64_t bytes = 0;
    for (const std::vector<Cx>& r : right_) {
        bytes += static_cast<std::uint64_t>(r.size()) * sizeof(Cx);
    }
    return bytes;
}

std::uint64_t mps_sampling_environment_bytes(QubitIndex num_qubits, std::uint32_t max_bond) {
    const std::uint64_t chi = static_cast<std::uint64_t>(max_bond);
    // n + 1 environments (the closed right boundary is one of them), each a
    // chi x chi complex matrix at worst.
    return (static_cast<std::uint64_t>(num_qubits) + 1ull) * chi * chi * sizeof(Cx);
}

}  // namespace detail
}  // namespace qplayer
