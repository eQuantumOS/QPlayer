/// @file tn_noise.cpp
/// @brief Kraus operator application on an MPS chain.
///
/// Both entry points are thin: the 1Q path is a physical-leg multiply and the 2Q
/// path is the existing gate path. That thinness is the point — reusing
/// MpsState::apply_1q / apply_2q means a channel and a gate go through the same
/// truncation policy and the same non-adjacent SWAP walk, so the two
/// cannot drift apart. What this file adds is validation and the arity dispatch.
///
/// See tn_noise.hpp for why the operator selection (the p_i draw) is NOT here.

#include "backend/tensor_network/tn_noise.hpp"

#include "backend/tensor_network/blas_loader.hpp"
#include "error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace qplayer {
namespace detail {

namespace {

constexpr const char* kBackend = "TensorNetwork";

using Cx = std::complex<double>;

// Row-major flatten with a squareness check. UnitaryMatrix is a nested vector,
// so a ragged operator is representable — reading it as dim*dim would walk off
// the short row and produce amplitudes instead of an error.
std::vector<Cx> flatten(const UnitaryMatrix& op, std::size_t expected_dim, const char* what) {
    if (op.size() != expected_dim) {
        throw SimulatorError{kBackend,
            std::string{what} + " expects a " + std::to_string(expected_dim) + "x" +
            std::to_string(expected_dim) + " operator, got " + std::to_string(op.size()) +
            " rows"};
    }
    std::vector<Cx> flat;
    flat.reserve(expected_dim * expected_dim);
    for (const std::vector<Cx>& row : op) {
        if (row.size() != expected_dim) {
            throw SimulatorError{kBackend,
                std::string{what} + " operator is not square: a row holds " +
                std::to_string(row.size()) + " entries, expected " +
                std::to_string(expected_dim)};
        }
        flat.insert(flat.end(), row.begin(), row.end());
    }
    return flat;
}

}  // namespace

void apply_kraus_1q(MpsState& state, const UnitaryMatrix& op, QubitIndex qubit) {
    const std::vector<Cx> flat = flatten(op, 2, "apply_kraus_1q");
    // Range checking lives in apply_1q; routing through it keeps one owner of
    // the site-index contract.
    state.apply_1q(flat, qubit);
}

void apply_kraus_2q(MpsState& state, const UnitaryMatrix& op, QubitIndex q0, QubitIndex q1) {
    const std::vector<Cx> flat = flatten(op, 4, "apply_kraus_2q");
    state.apply_2q(flat, q0, q1);
}

void apply_kraus_operator(MpsState& state, const KrausChannel& channel, std::size_t op_index) {
    if (channel.operators.empty()) {
        throw SimulatorError{kBackend, "Kraus channel carries no operators"};
    }
    if (op_index >= channel.operators.size()) {
        throw SimulatorError{kBackend,
            "Kraus operator index " + std::to_string(op_index) + " is out of range for a channel of " +
            std::to_string(channel.operators.size()) + " operators"};
    }

    // Dispatch on the FIRST operator's width, not the selected one: a channel
    // whose operators disagree on arity is malformed, and taking the width from
    // operators[0] matches the FullState dispatcher (full_state_cpu.cpp), so the
    // same channel is read the same way on both backends.
    const std::size_t dim = channel.operators.front().size();
    const UnitaryMatrix& op = channel.operators[op_index];

    if (dim == 2) {
        apply_kraus_1q(state, op, channel.target_qubit);
        return;
    }
    if (dim == 4) {
        apply_kraus_2q(state, op, channel.target_qubit, channel.target_qubit2);
        return;
    }
    throw SimulatorError{kBackend,
        "Kraus channel of dimension " + std::to_string(dim) +
        " is not supported; only 1-qubit (2x2) and 2-qubit (4x4) channels have an MPS path"};
}

// ── Branch probabilities and channel application ─────────────────────

namespace {

// Sites the channel acts on, in the order the OPERATOR indexes them: local bit 0
// is target_qubit. mps_reduced_density_matrix() orders its open bits by ascending
// site instead, so `swapped` records whether the two disagree.
struct ChannelSites {
    std::vector<QubitIndex> sites;    // as the RDM wants them (ascending)
    std::size_t             dim = 2;  // 2 or 4
    bool                    swapped = false;
};

constexpr std::uint32_t kLeft  = 0;
constexpr std::uint32_t kRight = 2;

// Row-major (left, physical, right) offset — the site tensor layout.
std::size_t site_at(std::size_t a, std::size_t s, std::size_t b, std::size_t right) {
    return (a * 2 + s) * right + b;
}


ChannelSites channel_sites(const KrausChannel& channel) {
    ChannelSites cs;
    cs.dim = channel.operators.front().size();
    if (cs.dim == 2) {
        cs.sites = {channel.target_qubit};
        return cs;
    }
    if (cs.dim == 4) {
        cs.swapped = channel.target_qubit > channel.target_qubit2;
        cs.sites = {channel.target_qubit, channel.target_qubit2};
        return cs;
    }
    throw SimulatorError{kBackend,
        "Kraus channel of dimension " + std::to_string(cs.dim) +
        " is not supported; only 1-qubit (2x2) and 2-qubit (4x4) channels have an MPS path"};
}

// Rewrites a 4x4 density matrix from ascending-site bit order into operand order
// by swapping local bits 0 and 1 — basis indices 1 and 2 trade places. Skipping
// this on a reversed operand pair silently evaluates the operator against the
// wrong marginal, which no norm check would catch.
void swap_basis_bits(std::vector<Cx>& rho) {
    for (std::size_t r = 0; r < 4; ++r) std::swap(rho[r * 4 + 1], rho[r * 4 + 2]);
    for (std::size_t c = 0; c < 4; ++c) std::swap(rho[1 * 4 + c], rho[2 * 4 + c]);
}

// Tr(K rho K^dagger) = sum_{a,b,c} K[a][b] * rho[b][c] * conj(K[a][c]).
double trace_of_conjugation(const std::vector<Cx>& flat_k, const std::vector<Cx>& rho,
                            std::size_t dim) {
    Cx acc{0.0, 0.0};
    for (std::size_t a = 0; a < dim; ++a) {
        for (std::size_t b = 0; b < dim; ++b) {
            const Cx kab = flat_k[a * dim + b];
            if (kab == Cx{0.0, 0.0}) continue;
            for (std::size_t c = 0; c < dim; ++c) {
                const Cx kac = flat_k[a * dim + c];
                if (kac == Cx{0.0, 0.0}) continue;
                acc += kab * rho[b * dim + c] * std::conj(kac);
            }
        }
    }
    // rho is Hermitian and K rho K^dagger is positive semidefinite, so the
    // imaginary part is numerical noise. Clamping at 0 keeps a -1e-18 from
    // becoming a negative probability downstream.
    return std::max(0.0, acc.real());
}

}  // namespace

// One-site reduced density matrix, contracted in TWO STEPS so the sweep is
// O(n * chi^3) rather than O(n * chi^4).
//
// This exists because the measurement demanded it. mps_reduced_density_matrix()
// and squared_norm() both nest left^2 x right^2, which is chi^4: at 40 sites and
// chi=32 that is ~250 ms for a single channel, and a device circuit attaches a
// channel to every gate. Splitting each environment step into (env x A) followed
// by (x A*) drops a factor of chi — the same restructure that makes DMRG
// environments tractable. Two-qubit channels still take the general RDM route:
// they are far rarer (one per 2Q gate) and a 4^k path is a separate shape.
//
// Exact for any gauge: the environments are built from the actual chain, so no
// canonical form is assumed (leaves canonicalisation open).
//
// Declared in tn_noise.hpp rather than kept TU-local since : measurement
// collapse reads the same 2x2 marginal, because a measurement IS the channel
// {|0><0|, |1><1|}.
// Smallest bond dimension at which the GEMM form of the environment sweep pays.
//
// Below this the scalar loops win, and by a lot: they skip zero entries (an MPS
// near |0...0> is mostly zeros, and BLAS multiplies those faithfully), they need
// no conjugated copy, and they pay no call overhead on a contraction that may be
// only a few dozen flops. Measured without this gate, large noisy circuits ran
// 5.7x faster while the noise test group ran 9x SLOWER (14.8 -> 134.8 s) --
// the same small-size trap, and D each had to close.
constexpr std::size_t kMarginalGemmMinDim = 32;

// Both environment sweeps contract a site tensor against a conjugated copy of
// itself. Materialising the conjugate once turns each step into a single GEMM:
// BLAS has transpose and conjugate-transpose ops but no conjugate-only one, and
// an O(chi^2) copy is nothing next to the O(chi^3) contraction it enables.
std::vector<Cx> conjugated(const Cx* src, std::size_t count) {
    std::vector<Cx> out(count);
    for (std::size_t i = 0; i < count; ++i) out[i] = std::conj(src[i]);
    return out;
}

// Thin BLAS wrapper. Every contraction below is derived in COLUMN-MAJOR terms
// using one identity: a row-major (m x n) buffer with row stride ld is the same
// memory as a column-major (n x m) matrix with leading dimension ld. That is
// what lets these sweeps become GEMMs with no repacking -- the same identity
// mps.cpp uses for the two-site merge.
//
// `accumulate` exists because the physical index s is interleaved INTO the row
// index of the site tensor. When s is a contraction index it cannot be folded
// into a single GEMM, so those steps run one GEMM per s and sum.
void gemm(const char* transa, const char* transb, int m, int n, int k,
          const Cx* a, int lda, const Cx* b, int ldb, Cx* c, int ldc,
          bool accumulate = false) {
    const Cx one{1.0, 0.0};
    const Cx zero{0.0, 0.0};
    qplayer::detail::zgemm(transa, transb, &m, &n, &k, &one, a, &lda, b, &ldb,
                           accumulate ? &one : &zero, c, &ldc);
}

namespace {

// One inward step of the left environment: L'[b][bp] = sum_(a,ap,s) L[a][ap]
// A[a,s,b] conj(A[ap,s,bp]). Shared by the free function and MpsState's cache
// so the two cannot compute a marginal differently.
// Step 1 of a left step: T[ap][s][b] = sum_a L[a][ap] * A[a,s,b], physical leg
// still open. Split out so the open step below shares it bit for bit.
std::vector<Cx> left_env_absorb_ket(const std::vector<Cx>& L, const TensorNode& A) {
    const std::size_t left  = A.indices[kLeft].dim;
    const std::size_t right = A.indices[kRight].dim;
    if (L.size() != left * left) {
        throw SimulatorError{kBackend, "site_marginal found an inconsistent bond"};
    }

    // Step 1: T[ap][s][b] = sum_a L[a][ap] * A[a,s,b]. The spectator index s
    // sits between a and b in the row index, so this is two GEMMs (one per
    // s) over strided views rather than one -- the stride is what `2*right`
    // as a leading dimension expresses.
    std::vector<Cx> T(left * 2 * right, Cx{0.0, 0.0});
    const int rr = static_cast<int>(right);
    const int ll = static_cast<int>(left);
    if (std::max(left, right) >= kMarginalGemmMinDim) {
        // col-major: Tv[b][ap] = sum_a Av[b][a] * Lv[ap][a]  =>  Av * Lv^T
        for (std::size_t sx = 0; sx < 2; ++sx) {
            gemm("N", "T", rr, ll, ll, A.data.data() + sx * right, 2 * rr,
                 L.data(), ll, T.data() + sx * right, 2 * rr);
        }
    } else {
        for (std::size_t a = 0; a < left; ++a) {
            for (std::size_t ap = 0; ap < left; ++ap) {
                const Cx l = L[a * left + ap];
                if (l == Cx{0.0, 0.0}) continue;
                for (std::size_t sx = 0; sx < 2; ++sx) {
                    for (std::size_t b = 0; b < right; ++b) {
                        T[(ap * 2 + sx) * right + b] += l * A.data[site_at(a, sx, b, right)];
                    }
                }
            }
        }
    }
    return T;
}

std::vector<Cx> left_env_step(const std::vector<Cx>& L, const TensorNode& A) {
    const std::size_t left  = A.indices[kLeft].dim;
    const std::size_t right = A.indices[kRight].dim;
    const std::vector<Cx> T = left_env_absorb_ket(L, A);
    const int rr = static_cast<int>(right);
    const int ll = static_cast<int>(left);

    // Step 2: L'[b][bp] = sum_{ap,s} T[ap][s][b] * conj(A[ap,s,bp]). Here the
    // contraction runs over the WHOLE row index (ap, s), so one GEMM does it.
    std::vector<Cx> next(right * right, Cx{0.0, 0.0});
    if (std::max(left, right) >= kMarginalGemmMinDim) {
        const std::vector<Cx> Ac = conjugated(A.data.data(), left * 2 * right);
        // col-major: Nv[bp][b] = sum_r Acv[bp][r] * Tv[b][r]  =>  Acv * Tv^T
        gemm("N", "T", rr, rr, 2 * ll, Ac.data(), rr, T.data(), rr, next.data(), rr);
    } else {
        for (std::size_t ap = 0; ap < left; ++ap) {
            for (std::size_t sx = 0; sx < 2; ++sx) {
                for (std::size_t b = 0; b < right; ++b) {
                    const Cx tv = T[(ap * 2 + sx) * right + b];
                    if (tv == Cx{0.0, 0.0}) continue;
                    for (std::size_t bp = 0; bp < right; ++bp) {
                        next[b * right + bp] += tv * std::conj(A.data[site_at(ap, sx, bp, right)]);
                    }
                }
            }
        }
    }
    return next;
}

// One inward step of the right environment: R'[a][ap] = sum_(b,bp,s) A[a,s,b]
// R[b][bp] conj(A[ap,s,bp]).
std::vector<Cx> right_env_step(const std::vector<Cx>& R, const TensorNode& A) {
    const std::size_t left  = A.indices[kLeft].dim;
    const std::size_t right = A.indices[kRight].dim;
    if (R.size() != right * right) {
        throw SimulatorError{kBackend, "site_marginal found an inconsistent bond"};
    }

    // Step 1: U[a][s][bp] = sum_b A[a,s,b] * R[b][bp]. Contraction is over the
    // trailing index of both, so the (a, s) rows come along unchanged.
    std::vector<Cx> U(left * 2 * right, Cx{0.0, 0.0});
    const int rr = static_cast<int>(right);
    const int ll = static_cast<int>(left);
    if (std::max(left, right) >= kMarginalGemmMinDim) {
        // Contraction is over the trailing index of both, so (a, s) rides
        // along untouched and one GEMM covers it.
        // col-major: Uv[bp][r] = sum_b Rv[bp][b] * Av[b][r]  =>  Rv * Av
        gemm("N", "N", rr, 2 * ll, rr, R.data(), rr, A.data.data(), rr, U.data(), rr);
    } else {
        for (std::size_t a = 0; a < left; ++a) {
            for (std::size_t sx = 0; sx < 2; ++sx) {
                for (std::size_t b = 0; b < right; ++b) {
                    const Cx av = A.data[site_at(a, sx, b, right)];
                    if (av == Cx{0.0, 0.0}) continue;
                    for (std::size_t bp = 0; bp < right; ++bp) {
                        U[(a * 2 + sx) * right + bp] += av * R[b * right + bp];
                    }
                }
            }
        }
    }
    // Step 2: R'[a][ap] = sum_{s,bp} U[a][s][bp] * conj(A[ap,s,bp]) -- again
    // over the whole (s, bp) trailing pair, so one GEMM per site.
    std::vector<Cx> next(left * left, Cx{0.0, 0.0});
    if (std::max(left, right) >= kMarginalGemmMinDim) {
        const std::vector<Cx> Ac = conjugated(A.data.data(), left * 2 * right);
        // s is a contraction index here AND part of both row indices, so this
        // is one GEMM per s, accumulated.
        // col-major: Nv[ap][a] = sum_bp Acv_s[bp][ap] * Uv_s[bp][a] => Acv_s^T * Uv_s
        for (std::size_t sx = 0; sx < 2; ++sx) {
            gemm("T", "N", ll, ll, rr, Ac.data() + sx * right, 2 * rr,
                 U.data() + sx * right, 2 * rr, next.data(), ll, /*accumulate=*/sx > 0);
        }
    } else {
        for (std::size_t a = 0; a < left; ++a) {
            for (std::size_t sx = 0; sx < 2; ++sx) {
                for (std::size_t bp = 0; bp < right; ++bp) {
                    const Cx uv = U[(a * 2 + sx) * right + bp];
                    if (uv == Cx{0.0, 0.0}) continue;
                    for (std::size_t ap = 0; ap < left; ++ap) {
                        next[a * left + ap] += uv * std::conj(A.data[site_at(ap, sx, bp, right)]);
                    }
                }
            }
        }
    }
    return next;
}

// Target site: rho[s][s'] = sum L[a][ap] A[a,s,b] R[b][bp] conj(A[ap,s',bp]),
// in two steps so nothing here is chi^4 either.
std::vector<Cx> contract_site_marginal(const std::vector<Cx>& L, const std::vector<Cx>& R,
                                       const TensorNode& A) {
    const std::size_t left  = A.indices[kLeft].dim;
    const std::size_t right = A.indices[kRight].dim;
    if (L.size() != left * left || R.size() != right * right) {
        throw SimulatorError{kBackend, "site_marginal found an inconsistent bond"};
    }

    // V[ap][s][b] = sum_a L[a][ap] * A[a,s,b].
    std::vector<Cx> V(left * 2 * right, Cx{0.0, 0.0});
    for (std::size_t a = 0; a < left; ++a) {
        for (std::size_t ap = 0; ap < left; ++ap) {
            const Cx l = L[a * left + ap];
            if (l == Cx{0.0, 0.0}) continue;
            for (std::size_t sx = 0; sx < 2; ++sx) {
                for (std::size_t b = 0; b < right; ++b) {
                    V[(ap * 2 + sx) * right + b] += l * A.data[site_at(a, sx, b, right)];
                }
            }
        }
    }
    // W[ap][s][bp] = sum_b V[ap][s][b] * R[b][bp].
    std::vector<Cx> W(left * 2 * right, Cx{0.0, 0.0});
    for (std::size_t ap = 0; ap < left; ++ap) {
        for (std::size_t sx = 0; sx < 2; ++sx) {
            for (std::size_t b = 0; b < right; ++b) {
                const Cx vv = V[(ap * 2 + sx) * right + b];
                if (vv == Cx{0.0, 0.0}) continue;
                for (std::size_t bp = 0; bp < right; ++bp) {
                    W[(ap * 2 + sx) * right + bp] += vv * R[b * right + bp];
                }
            }
        }
    }
    // rho[s][s'] = sum_{ap,bp} W[ap][s][bp] * conj(A[ap,s',bp]).
    std::vector<Cx> rho(4, Cx{0.0, 0.0});
    for (std::size_t ap = 0; ap < left; ++ap) {
        for (std::size_t sx = 0; sx < 2; ++sx) {
            for (std::size_t bp = 0; bp < right; ++bp) {
                const Cx wv = W[(ap * 2 + sx) * right + bp];
                if (wv == Cx{0.0, 0.0}) continue;
                for (std::size_t sp = 0; sp < 2; ++sp) {
                    rho[sx * 2 + sp] += wv * std::conj(A.data[site_at(ap, sp, bp, right)]);
                }
            }
        }
    }
    return rho;
}

// Left step with the physical leg left OPEN: out[s * 2 + s'][b][bp] =
// sum_(a,ap) L[a][ap] A[a,s,b] conj(A[ap,s',bp]). The two-site marginal carries
// these four chi x chi blocks from its first operand to its second.
std::array<std::vector<Cx>, 4> left_env_open_step(const std::vector<Cx>& L, const TensorNode& A) {
    const std::size_t left  = A.indices[kLeft].dim;
    const std::size_t right = A.indices[kRight].dim;
    const std::vector<Cx> T = left_env_absorb_ket(L, A);
    const int rr = static_cast<int>(right);
    const int ll = static_cast<int>(left);

    std::array<std::vector<Cx>, 4> out;
    const bool use_gemm = std::max(left, right) >= kMarginalGemmMinDim;
    const std::vector<Cx> Ac = use_gemm ? conjugated(A.data.data(), left * 2 * right)
                                        : std::vector<Cx>{};
    for (std::size_t sx = 0; sx < 2; ++sx) {
        for (std::size_t sp = 0; sp < 2; ++sp) {
            std::vector<Cx>& E = out[sx * 2 + sp];
            E.assign(right * right, Cx{0.0, 0.0});
            if (use_gemm) {
                // Same strided views as step 1: fixing s (s') picks rows ap with
                // stride 2*right. col-major: Ev[bp][b] = Acv_s'[bp][ap] * Tv_s[b][ap]^T
                gemm("N", "T", rr, rr, ll, Ac.data() + sp * right, 2 * rr,
                     T.data() + sx * right, 2 * rr, E.data(), rr);
            } else {
                for (std::size_t ap = 0; ap < left; ++ap) {
                    for (std::size_t b = 0; b < right; ++b) {
                        const Cx tv = T[(ap * 2 + sx) * right + b];
                        if (tv == Cx{0.0, 0.0}) continue;
                        for (std::size_t bp = 0; bp < right; ++bp) {
                            E[b * right + bp] += tv * std::conj(A.data[site_at(ap, sp, bp, right)]);
                        }
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace

std::vector<Cx> site_marginal(const TensorGraph& chain, std::size_t site) {
    const std::size_t n = chain.node_count();
    if (site >= n) {
        throw SimulatorError{kBackend,
            "kraus_branch_probabilities site " + std::to_string(site) + " out of range"};
    }

    // Left environment L[a * dim + ap]: ket bond a, bra bond ap.
    std::vector<Cx> L{Cx{1.0, 0.0}};
    for (std::size_t k = 0; k < site; ++k) {
        L = left_env_step(L, chain.node(static_cast<std::uint32_t>(k)));
    }
    // Right environment R[b * dim + bp], swept inward from the far end.
    std::vector<Cx> R{Cx{1.0, 0.0}};
    for (std::size_t k = n; k-- > site + 1;) {
        R = right_env_step(R, chain.node(static_cast<std::uint32_t>(k)));
    }
    return contract_site_marginal(L, R, chain.node(static_cast<std::uint32_t>(site)));
}

// Cached counterpart: only environments a mutation marked stale are
// rebuilt, from the nearest current one inward, with the kernels above.
std::vector<Cx> MpsState::site_marginal(QubitIndex site) const {
    if (site >= num_qubits_) {
        throw SimulatorError{kBackend,
            "kraus_branch_probabilities site " + std::to_string(site) + " out of range"};
    }
    for (; env_left_valid_ < site; ++env_left_valid_) {
        env_left_[env_left_valid_ + 1] = left_env_step(env_left_[env_left_valid_],
                                                       graph_.node(env_left_valid_));
    }
    for (; env_right_valid_ > site; --env_right_valid_) {
        env_right_[env_right_valid_ - 1] = right_env_step(env_right_[env_right_valid_],
                                                          graph_.node(env_right_valid_));
    }
    return contract_site_marginal(env_left_[site], env_right_[site], graph_.node(site));
}

// Two-site counterpart. Replaces the general RDM sweep for 2Q
// channels, which walked the whole chain at O(chi^4) per site on every call.
std::vector<Cx> MpsState::two_site_marginal(QubitIndex lo, QubitIndex hi) const {
    if (hi >= num_qubits_ || lo >= hi) {
        throw SimulatorError{kBackend,
            "two_site_marginal needs sites lo < hi < " + std::to_string(num_qubits_) +
            " (got " + std::to_string(lo) + ", " + std::to_string(hi) + ")"};
    }
    for (; env_left_valid_ < lo; ++env_left_valid_) {
        env_left_[env_left_valid_ + 1] = left_env_step(env_left_[env_left_valid_],
                                                       graph_.node(env_left_valid_));
    }
    for (; env_right_valid_ > hi; --env_right_valid_) {
        env_right_[env_right_valid_ - 1] = right_env_step(env_right_[env_right_valid_],
                                                          graph_.node(env_right_valid_));
    }

    // Sites strictly between the operands are traced exactly as in a plain left
    // step; only the blocks' outer index pair (s, s') rides along.
    std::array<std::vector<Cx>, 4> E = left_env_open_step(env_left_[lo], graph_.node(lo));
    for (QubitIndex k = lo + 1; k < hi; ++k) {
        for (std::vector<Cx>& block : E) block = left_env_step(block, graph_.node(k));
    }

    // Bit 0 is the lower site, the layout mps_reduced_density_matrix() returns.
    std::vector<Cx> rho(16, Cx{0.0, 0.0});
    for (std::size_t sx = 0; sx < 2; ++sx) {
        for (std::size_t sp = 0; sp < 2; ++sp) {
            const std::vector<Cx> block =
                contract_site_marginal(E[sx * 2 + sp], env_right_[hi], graph_.node(hi));
            for (std::size_t t = 0; t < 2; ++t) {
                for (std::size_t tp = 0; tp < 2; ++tp) {
                    rho[(sx | (t << 1)) * 4 + (sp | (tp << 1))] = block[t * 2 + tp];
                }
            }
        }
    }
    return rho;
}

std::vector<double> kraus_branch_probabilities(const MpsState& state,
                                               const KrausChannel& channel) {
    if (channel.operators.empty()) {
        throw SimulatorError{kBackend, "Kraus channel carries no operators"};
    }
    const ChannelSites cs = channel_sites(channel);

    // One marginal per channel, not one sweep per operator — the choice
    // section E records. Both arities read MpsState's cached environments
    // The 2-site case used the general RDM sweep until measurement
    // showed it was not rare in cost: one 4x4 channel per CX made a 20-qubit run
    // 110x slower than the same run without it.
    std::vector<Cx> rho;
    if (cs.dim == 2) {
        rho = state.site_marginal(cs.sites[0]);
    } else {
        if (cs.sites[0] == cs.sites[1]) {
            throw SimulatorError{kBackend, "reduced_density_matrix received a repeated site"};
        }
        rho = state.two_site_marginal(std::min(cs.sites[0], cs.sites[1]),
                                      std::max(cs.sites[0], cs.sites[1]));
    }
    if (cs.swapped) swap_basis_bits(rho);

    std::vector<double> probs;
    probs.reserve(channel.operators.size());
    for (const UnitaryMatrix& op : channel.operators) {
        const std::vector<Cx> flat = flatten(op, cs.dim, "kraus_branch_probabilities");
        probs.push_back(trace_of_conjugation(flat, rho, cs.dim));
    }
    return probs;
}

std::size_t apply_kraus_channel(MpsState& state, const KrausChannel& channel,
                                std::mt19937_64& rng) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return apply_kraus_channel_u(state, channel, dist(rng));
}

std::size_t apply_kraus_channel_u(MpsState& state, const KrausChannel& channel,
                                  double u01) {
    const std::vector<double> probs = kraus_branch_probabilities(state, channel);
    double total = 0.0;
    for (double p : probs) total += p;
    if (!std::isfinite(total) || total <= 0.0) {
        throw SimulatorError{kBackend,
            "Kraus channel has no branch with positive weight (total = " +
            std::to_string(total) + "); the state has diverged numerically"};
    }

    // Cumulative pick against one draw scaled by `total`, mirroring the FullState
    // selector including its fall-through to the last operator: comparing against
    // a renormalised copy would introduce a second rounding step and let the two
    // backends disagree on a boundary draw.
    const double u = u01 * total;
    double cumsum = 0.0;
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < probs.size(); ++i) {
        cumsum += probs[i];
        if (u < cumsum) { chosen = i; break; }
        chosen = i;
    }

    const double p_chosen = probs[chosen];
    if (!(p_chosen > 0.0)) {
        throw SimulatorError{kBackend,
            "Kraus branch " + std::to_string(chosen) +
            " was selected with zero weight; the state has diverged numerically"};
    }

    // Fold the renormalisation into the matrix: applying K_k * sqrt(total/p_k)
    // leaves the norm exactly where it was before the channel, so a
    // trace-preserving channel is norm-preserving on the chain too.
    const double scale = std::sqrt(total / p_chosen);
    UnitaryMatrix scaled = channel.operators[chosen];
    for (std::vector<Cx>& row : scaled) {
        for (Cx& v : row) v *= scale;
    }

    const ChannelSites cs = channel_sites(channel);
    if (cs.dim == 2) {
        apply_kraus_1q(state, scaled, channel.target_qubit);
    } else {
        apply_kraus_2q(state, scaled, channel.target_qubit, channel.target_qubit2);
    }
    return chosen;
}

}  // namespace detail
}  // namespace qplayer
