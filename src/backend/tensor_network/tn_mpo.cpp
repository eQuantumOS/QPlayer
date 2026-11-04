/// @file tn_mpo.cpp
/// @brief Density matrix as an MPO — exact channel application.
///
/// One site holds a qubit's ket and bra legs as a single physical index of
/// dimension 4 (`ket * 2 + bra`), which collapses unitaries and channels into one
/// primitive: `rho -> sum_i K_i rho K_i^dagger` on the site's 2x2 (or the pair's
/// 4x4) block. A unitary is just that sum with a single operator, so there is no
/// second code path for it.
///
/// The chain machinery mirrors mps.cpp — merge two neighbours, act, zgesvd_,
/// split — because the structure genuinely is the same. What differs is stated in
/// and is all in this file: the physical dimension is 4 not 2, the
/// truncation normaliser is ||rho||_F rather than the fixed ||psi|| = 1, the
/// renormalisation restores Tr(rho) = 1 rather than the Frobenius norm, and
/// truncation can leave the state non-positive.

#include "backend/tensor_network/tn_mpo.hpp"

#include "error.hpp"
#include "backend/tensor_network/blas_loader.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdlib>
#include <string>

// LAPACK complex SVD, resolved from OpenBLAS at run time rather than linked:
// linking it would build a thread pool in every process, including those that
// never touch TensorNetwork.

namespace qplayer {
namespace detail {

namespace {

using Cx = std::complex<double>;

constexpr const char*  kBackend  = "TensorNetwork";
constexpr std::uint32_t kLeft     = 0;
constexpr std::uint32_t kPhysical = 1;
constexpr std::uint32_t kRight    = 2;
constexpr std::size_t   kPhysDim  = 4;   // (ket, bra)

[[noreturn]] void reject(const std::string& what) {
    throw SimulatorError{kBackend, what};
}

/// Row-major offset inside a [left, 4, right] site tensor.
inline std::size_t site_offset(std::size_t a, std::size_t p, std::size_t b, std::size_t right) {
    return (a * kPhysDim + p) * right + b;
}

/// Physical index from a (ket, bra) pair.
inline std::size_t phys(std::size_t ket, std::size_t bra) { return ket * 2 + bra; }

bool env_double(const char* key, double* out) {
    const char* raw = std::getenv(key);
    if (raw == nullptr || *raw == '\0') return false;
    try {
        std::size_t consumed = 0;
        const double v = std::stod(raw, &consumed);
        if (consumed != std::string{raw}.size() || !(v >= 0.0)) {
            log::warn(std::string{key} + ": ignoring unusable value '" + raw + "'");
            return false;
        }
        *out = v;
        return true;
    } catch (const std::exception&) {
        log::warn(std::string{key} + ": ignoring unparseable value '" + raw + "'");
        return false;
    }
}

/// Flattens a d x d UnitaryMatrix, rejecting a ragged or wrong-sized one. The
/// row-count check alone is not enough — a ragged operator can still flatten to
/// the right length (the trap 's tests documented).
std::vector<Cx> flatten_operator(const UnitaryMatrix& k, std::size_t dim, const char* what) {
    if (k.size() != dim) {
        reject(std::string{what} + " expects " + std::to_string(dim) + "x" + std::to_string(dim) +
               " Kraus operators");
    }
    std::vector<Cx> flat;
    flat.reserve(dim * dim);
    for (const std::vector<Cx>& row : k) {
        if (row.size() != dim) {
            reject(std::string{what} + " received a ragged Kraus operator");
        }
        flat.insert(flat.end(), row.begin(), row.end());
    }
    return flat;
}

/// The 4x4 SWAP in operand order, as a single-operator channel.
const std::vector<Cx>& swap_matrix() {
    static const std::vector<Cx> kSwap = [] {
        std::vector<Cx> m(16, Cx{0.0, 0.0});
        m[0 * 4 + 0] = Cx{1.0, 0.0};
        m[1 * 4 + 2] = Cx{1.0, 0.0};
        m[2 * 4 + 1] = Cx{1.0, 0.0};
        m[3 * 4 + 3] = Cx{1.0, 0.0};
        return m;
    }();
    return kSwap;
}

/// <bra|P|ket> for one factor. Row is the bra (output) index, column the ket.
Cx pauli_element(PauliOp op, std::size_t bra, std::size_t ket) {
    switch (op) {
        case PauliOp::I: return bra == ket ? Cx{1.0, 0.0} : Cx{0.0, 0.0};
        case PauliOp::X: return bra != ket ? Cx{1.0, 0.0} : Cx{0.0, 0.0};
        case PauliOp::Y:
            if (bra == ket) return Cx{0.0, 0.0};
            return bra == 1 ? Cx{0.0, 1.0} : Cx{0.0, -1.0};
        case PauliOp::Z:
            if (bra != ket) return Cx{0.0, 0.0};
            return bra == 0 ? Cx{1.0, 0.0} : Cx{-1.0, 0.0};
    }
    return Cx{0.0, 0.0};
}

UnitaryMatrix unflatten(const std::vector<Cx>& flat, std::size_t dim) {
    UnitaryMatrix m(dim, std::vector<Cx>(dim, Cx{0.0, 0.0}));
    for (std::size_t r = 0; r < dim; ++r) {
        for (std::size_t c = 0; c < dim; ++c) m[r][c] = flat[r * dim + c];
    }
    return m;
}

}  // namespace

// ── MpoTruncationConfig ─────────────────────────────────────────────────────

MpoTruncationConfig MpoTruncationConfig::from_env() {
    MpoTruncationConfig cfg;

    double bond = 0.0;
    if (env_double("QPLAYER_MPO_MAX_BOND", &bond)) {
        const auto rounded = static_cast<std::uint32_t>(bond);
        if (rounded >= 1 && rounded <= kMpoMaxBondCeiling && static_cast<double>(rounded) == bond) {
            cfg.max_bond = rounded;
        } else {
            log::warn("QPLAYER_MPO_MAX_BOND must be an integer in [1, " +
                      std::to_string(kMpoMaxBondCeiling) + "]; keeping default " +
                      std::to_string(cfg.max_bond));
        }
    }

    double cutoff = 0.0;
    if (env_double("QPLAYER_MPO_CUTOFF", &cutoff)) cfg.cutoff = cutoff;

    double budget = 0.0;
    if (env_double("QPLAYER_MPO_ERROR_BUDGET", &budget)) cfg.error_budget_warn = budget;

    return cfg;
}

void MpoTruncationConfig::validate() const {
    if (max_bond == 0) {
        throw CircuitError{"mpo max_bond must be at least 1"};
    }
    if (max_bond > kMpoMaxBondCeiling) {
        throw CircuitError{"mpo max_bond " + std::to_string(max_bond) +
                           " exceeds the ceiling " + std::to_string(kMpoMaxBondCeiling) +
                           " (a 4chi x 4chi SVD at that width costs minutes per gate)"};
    }
    if (!(cutoff >= 0.0)) {
        throw CircuitError{"mpo cutoff must be non-negative"};
    }
}

// ── Chain construction and validation ───────────────────────────────────────

TensorGraph mpo_zero_state(QubitIndex num_qubits) {
    if (num_qubits == 0) reject("mpo_zero_state requires at least one qubit");
    if (num_qubits > kMaxTensorNetworkQubits) {
        reject("mpo_zero_state exceeds kMaxTensorNetworkQubits");
    }

    TensorGraph graph;
    for (QubitIndex q = 0; q < num_qubits; ++q) {
        TensorNode node;
        // Boundary bonds stay explicit at dim 1 so every site is a uniform
        // 3-index tensor — the same choice mps_zero_state() made, for the same
        // reason (otherwise every later routine special-cases the ends).
        node.indices = {TensorIndex{TensorIndex::Kind::Bond,     1, q},
                        TensorIndex{TensorIndex::Kind::Physical, 4, q},
                        TensorIndex{TensorIndex::Kind::Bond,     1, q + 1}};
        node.data.assign(kPhysDim, Cx{0.0, 0.0});
        node.data[phys(0, 0)] = Cx{1.0, 0.0};   // |0><0| on every site
        graph.add_node(std::move(node));
    }
    for (QubitIndex q = 0; q + 1 < num_qubits; ++q) {
        graph.connect(q, kRight, q + 1, kLeft);
    }
    return graph;
}

bool is_mpo_chain(const TensorGraph& graph) noexcept {
    const std::size_t n = graph.node_count();
    if (n == 0) return false;
    if (graph.bond_count() != n - 1) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const TensorNode& site = graph.nodes()[i];
        if (site.indices.size() != 3) return false;
        if (site.indices[kLeft].kind     != TensorIndex::Kind::Bond)     return false;
        if (site.indices[kRight].kind    != TensorIndex::Kind::Bond)     return false;
        if (site.indices[kPhysical].kind != TensorIndex::Kind::Physical) return false;
        if (site.indices[kPhysical].dim != kPhysDim) return false;
        if (site.indices[kPhysical].id != static_cast<std::uint32_t>(i)) return false;
    }
    if (graph.nodes().front().indices[kLeft].dim  != 1) return false;
    if (graph.nodes().back().indices[kRight].dim != 1) return false;
    return true;
}

// ── Construction ────────────────────────────────────────────────────────────

MpoState::MpoState(QubitIndex num_qubits, MpoTruncationConfig config)
    : num_qubits_(num_qubits),
      graph_(mpo_zero_state(num_qubits)),
      config_(config) {
    config_.validate();
}

// ── One-site operations (bond invariant, no SVD) ────────────────────────────

void MpoState::apply_channel_1q(const std::vector<UnitaryMatrix>& kraus, QubitIndex site) {
    if (site >= num_qubits_) reject("apply_channel_1q site " + std::to_string(site) + " out of range");
    if (kraus.empty())       reject("apply_channel_1q received an empty operator list");

    std::vector<std::vector<Cx>> ops;
    ops.reserve(kraus.size());
    for (const UnitaryMatrix& k : kraus) ops.push_back(flatten_operator(k, 2, "apply_channel_1q"));

    TensorNode& node = graph_.node(site);
    const std::size_t left  = node.indices[kLeft].dim;
    const std::size_t right = node.indices[kRight].dim;

    // rho' = sum_i K_i rho K_i^dagger on the site's 2x2 block. Both bonds are
    // spectators, which is why a one-site channel never needs truncation — the
    // MPS path's "1Q is free" property survives into the density matrix.
    for (std::size_t a = 0; a < left; ++a) {
        for (std::size_t b = 0; b < right; ++b) {
            Cx in[2][2];
            for (std::size_t ket = 0; ket < 2; ++ket) {
                for (std::size_t bra = 0; bra < 2; ++bra) {
                    in[ket][bra] = node.data[site_offset(a, phys(ket, bra), b, right)];
                }
            }
            Cx out[2][2] = {{Cx{0,0}, Cx{0,0}}, {Cx{0,0}, Cx{0,0}}};
            for (const std::vector<Cx>& K : ops) {
                for (std::size_t ket = 0; ket < 2; ++ket) {
                    for (std::size_t bra = 0; bra < 2; ++bra) {
                        Cx acc{0.0, 0.0};
                        for (std::size_t kp = 0; kp < 2; ++kp) {
                            for (std::size_t bp = 0; bp < 2; ++bp) {
                                acc += K[ket * 2 + kp] * in[kp][bp] * std::conj(K[bra * 2 + bp]);
                            }
                        }
                        out[ket][bra] += acc;
                    }
                }
            }
            for (std::size_t ket = 0; ket < 2; ++ket) {
                for (std::size_t bra = 0; bra < 2; ++bra) {
                    node.data[site_offset(a, phys(ket, bra), b, right)] = out[ket][bra];
                }
            }
        }
    }
}

void MpoState::apply_unitary_1q(const std::vector<Cx>& u, QubitIndex site) {
    if (u.size() != 4) reject("apply_unitary_1q expects a 2x2 matrix (4 entries)");
    apply_channel_1q({unflatten(u, 2)}, site);
}

// ── Two-site operations ─────────────────────────────────────────────────────

double MpoState::apply_channel_2q_adjacent(const std::vector<UnitaryMatrix>& kraus,
                                           QubitIndex site, bool swap_operands) {
    std::vector<std::vector<Cx>> ops;
    ops.reserve(kraus.size());
    for (const UnitaryMatrix& k : kraus) ops.push_back(flatten_operator(k, 4, "apply_channel_2q"));

    TensorNode& A = graph_.node(site);
    TensorNode& B = graph_.node(site + 1);

    const std::size_t l = A.indices[kLeft].dim;
    const std::size_t m = A.indices[kRight].dim;
    const std::size_t r = B.indices[kRight].dim;

    const int rows = static_cast<int>(l * kPhysDim);
    const int cols = static_cast<int>(kPhysDim * r);
    const int k    = std::min(rows, cols);

    // Merge into a column-major (l*4) x (4*r) matrix: row (a, p1), column (p2, b).
    std::vector<Cx> mat(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), Cx{0, 0});
    for (std::size_t a = 0; a < l; ++a) {
        for (std::size_t p1 = 0; p1 < kPhysDim; ++p1) {
            for (std::size_t p2 = 0; p2 < kPhysDim; ++p2) {
                for (std::size_t b = 0; b < r; ++b) {
                    Cx acc{0, 0};
                    for (std::size_t c = 0; c < m; ++c) {
                        acc += A.data[site_offset(a, p1, c, m)] * B.data[site_offset(c, p2, b, r)];
                    }
                    mat[(a * kPhysDim + p1) +
                        (p2 * r + b) * static_cast<std::size_t>(rows)] = acc;
                }
            }
        }
    }

    // Act on the (ket, bra) pair of the two sites. The operand-order convention
    // matches the MPS path: local bit 0 is the first operand, so when it sits on
    // the right site the two roles swap (getting this backwards silently
    // transposes control and target).
    const auto pack = [swap_operands](std::size_t left_bit, std::size_t right_bit) -> std::size_t {
        return swap_operands ? (right_bit + 2 * left_bit) : (left_bit + 2 * right_bit);
    };
    for (std::size_t a = 0; a < l; ++a) {
        for (std::size_t b = 0; b < r; ++b) {
            // in[ket][bra] over the joint 4-dimensional ket and bra spaces.
            Cx in[4][4];
            for (std::size_t p1 = 0; p1 < kPhysDim; ++p1) {
                for (std::size_t p2 = 0; p2 < kPhysDim; ++p2) {
                    const std::size_t ket = pack(p1 / 2, p2 / 2);
                    const std::size_t bra = pack(p1 % 2, p2 % 2);
                    in[ket][bra] = mat[(a * kPhysDim + p1) +
                                       (p2 * r + b) * static_cast<std::size_t>(rows)];
                }
            }
            Cx out[4][4];
            for (auto& row : out) for (Cx& v : row) v = Cx{0.0, 0.0};
            for (const std::vector<Cx>& K : ops) {
                // tmp = K * rho, then out += tmp * K^dagger.
                Cx tmp[4][4];
                for (std::size_t i = 0; i < 4; ++i) {
                    for (std::size_t j = 0; j < 4; ++j) {
                        Cx acc{0.0, 0.0};
                        for (std::size_t t = 0; t < 4; ++t) acc += K[i * 4 + t] * in[t][j];
                        tmp[i][j] = acc;
                    }
                }
                for (std::size_t i = 0; i < 4; ++i) {
                    for (std::size_t j = 0; j < 4; ++j) {
                        Cx acc{0.0, 0.0};
                        for (std::size_t t = 0; t < 4; ++t) acc += tmp[i][t] * std::conj(K[j * 4 + t]);
                        out[i][j] += acc;
                    }
                }
            }
            for (std::size_t p1 = 0; p1 < kPhysDim; ++p1) {
                for (std::size_t p2 = 0; p2 < kPhysDim; ++p2) {
                    const std::size_t ket = pack(p1 / 2, p2 / 2);
                    const std::size_t bra = pack(p1 % 2, p2 % 2);
                    mat[(a * kPhysDim + p1) +
                        (p2 * r + b) * static_cast<std::size_t>(rows)] = out[ket][bra];
                }
            }
        }
    }

    std::vector<double> sv(static_cast<std::size_t>(k), 0.0);
    std::vector<Cx> U(static_cast<std::size_t>(rows) * static_cast<std::size_t>(k), Cx{0, 0});
    std::vector<Cx> VT(static_cast<std::size_t>(k) * static_cast<std::size_t>(cols), Cx{0, 0});
    std::vector<double> rwork(static_cast<std::size_t>(5 * k), 0.0);
    int info = 0;

    Cx work_query{0, 0};
    int lwork = -1;
    qplayer::detail::zgesvd("S", "S", &rows, &cols, mat.data(), &rows, sv.data(),
            U.data(), &rows, VT.data(), &k, &work_query, &lwork, rwork.data(), &info);
    if (info != 0) reject("zgesvd workspace query failed with info=" + std::to_string(info));

    lwork = static_cast<int>(work_query.real());
    std::vector<Cx> work(static_cast<std::size_t>(std::max(lwork, 1)), Cx{0, 0});
    qplayer::detail::zgesvd("S", "S", &rows, &cols, mat.data(), &rows, sv.data(),
            U.data(), &rows, VT.data(), &k, work.data(), &lwork, rwork.data(), &info);
    if (info != 0) reject("zgesvd failed to converge (info=" + std::to_string(info) + ")");

    // Truncation rule: same functional form as, but
    // the normaliser is ||rho||_F, which is what makes the cutoff mean the same
    // thing at every n — an absolute threshold would discard the whole state
    // once ||rho||_F falls to 2^(-n/2).
    double total_sq = 0.0;
    for (int i = 0; i < k; ++i) total_sq += sv[static_cast<std::size_t>(i)] * sv[static_cast<std::size_t>(i)];
    const double norm = std::sqrt(total_sq);

    int keep_cutoff = k;
    if (norm > 0.0) {
        double tail = 0.0;
        for (int i = k - 1; i >= 1; --i) {
            tail += sv[static_cast<std::size_t>(i)] * sv[static_cast<std::size_t>(i)];
            if (std::sqrt(tail) / norm > config_.cutoff) break;
            keep_cutoff = i;
        }
    }
    const int keep = std::max(1, std::min<int>(static_cast<int>(config_.max_bond), keep_cutoff));
    const bool bond_capped = keep < keep_cutoff;

    double kept_sq = 0.0;
    for (int i = 0; i < keep; ++i) kept_sq += sv[static_cast<std::size_t>(i)] * sv[static_cast<std::size_t>(i)];
    const double discarded_sq = std::max(0.0, total_sq - kept_sq);
    const double step_error   = (norm > 0.0) ? std::sqrt(discarded_sq) / norm : 0.0;

    const std::size_t chi = static_cast<std::size_t>(keep);

    std::vector<Cx> a_data(l * kPhysDim * chi, Cx{0, 0});
    for (std::size_t a = 0; a < l; ++a) {
        for (std::size_t p1 = 0; p1 < kPhysDim; ++p1) {
            for (std::size_t c = 0; c < chi; ++c) {
                a_data[site_offset(a, p1, c, chi)] =
                    U[(a * kPhysDim + p1) + c * static_cast<std::size_t>(rows)];
            }
        }
    }
    std::vector<Cx> b_data(chi * kPhysDim * r, Cx{0, 0});
    for (std::size_t c = 0; c < chi; ++c) {
        for (std::size_t p2 = 0; p2 < kPhysDim; ++p2) {
            for (std::size_t b = 0; b < r; ++b) {
                b_data[site_offset(c, p2, b, r)] =
                    sv[c] * VT[c + (p2 * r + b) * static_cast<std::size_t>(k)];
            }
        }
    }

    A.data = std::move(a_data);
    A.indices[kRight].dim = static_cast<std::uint32_t>(chi);
    B.data = std::move(b_data);
    B.indices[kLeft].dim = static_cast<std::uint32_t>(chi);

    if (bond_capped && step_error > config_.cutoff && !warned_lossy_) {
        warned_lossy_ = true;
        log::warn("MPO bond cap " + std::to_string(config_.max_bond) +
                  " forced a lossy truncation (step error " + std::to_string(step_error) +
                  "); raise QPLAYER_MPO_MAX_BOND or inspect Result::truncation_errors()");
    }
    return step_error;
}

void MpoState::apply_channel_2q(const std::vector<UnitaryMatrix>& kraus,
                                QubitIndex s0, QubitIndex s1) {
    if (s0 >= num_qubits_ || s1 >= num_qubits_) reject("apply_channel_2q site out of range");
    if (s0 == s1)                               reject("apply_channel_2q needs two distinct sites");
    if (kraus.empty())                          reject("apply_channel_2q received an empty operator list");

    const QubitIndex lo = std::min(s0, s1);
    const QubitIndex hi = std::max(s0, s1);
    const bool swap_operands = (s0 > s1);

    if (hi == lo + 1) {
        const double e = apply_channel_2q_adjacent(kraus, lo, swap_operands);
        note_truncation(e, /*bond_capped=*/false);
        if (e > 0.0) normalize_trace();
        return;
    }

    // Distant operands: walk `hi` down to lo+1 with SWAPs, act, walk back. The
    // SWAPs are unitary channels through the same primitive, so they obey the
    // same truncation policy the gate does.
    const std::vector<UnitaryMatrix> swap_ops{unflatten(swap_matrix(), 4)};
    double squared = 0.0;
    for (QubitIndex s = hi; s > lo + 1; --s) {
        const double e = apply_channel_2q_adjacent(swap_ops, s - 1, false);
        squared += e * e;
        ++swap_steps_;
    }
    const double gate_error = apply_channel_2q_adjacent(kraus, lo, swap_operands);
    squared += gate_error * gate_error;
    // Mirror of the outbound walk — stopping one site short would leave the
    // chain permuted and every later operation on the wrong qubit.
    for (QubitIndex s = lo + 1; s < hi; ++s) {
        const double e = apply_channel_2q_adjacent(swap_ops, s, false);
        squared += e * e;
        ++swap_steps_;
    }
    const double total_error = std::sqrt(squared);
    note_truncation(total_error, /*bond_capped=*/false);
    if (total_error > 0.0) normalize_trace();
}

void MpoState::apply_unitary_2q(const std::vector<Cx>& u, QubitIndex s0, QubitIndex s1) {
    if (u.size() != 16) reject("apply_unitary_2q expects a 4x4 matrix (16 entries)");
    apply_channel_2q({unflatten(u, 4)}, s0, s1);
}

void MpoState::note_truncation(double step_error, bool /*bond_capped*/) {
    // One entry per 2Q CALL, not per internal SVD — a distant pair's SWAP walk
    // folds into the same number (same convention as the MPS path).
    truncation_errors_.push_back(step_error);
    squared_error_sum_ += step_error * step_error;
    if (!warned_budget_ && total_truncation_error() > config_.error_budget_warn) {
        warned_budget_ = true;
        log::warn("MPO accumulated truncation error " + std::to_string(total_truncation_error()) +
                  " exceeds the budget " + std::to_string(config_.error_budget_warn) +
                  "; the density matrix may no longer be positive");
    }
}

double MpoState::total_truncation_error() const noexcept {
    return std::sqrt(squared_error_sum_);
}

// ── Traces, expectations, sampling ──────────────────────────────────────────

namespace {

/// Sweeps the chain right to left, closing each physical leg with `weight`, and
/// returns the environments env[i] over site i's LEFT bond. env[n] is the scalar 1.
///
/// `weight(site, ket, bra)` is the factor contracted into that site's physical
/// leg — delta(ket, bra) for a trace, a Pauli element for <P>, a projector for a
/// sampling conditional. One sweep shape serves all three, so they cannot drift.
std::vector<std::vector<Cx>> right_environments(
        const TensorGraph& chain,
        const std::function<Cx(std::size_t, std::size_t, std::size_t)>& weight) {
    const std::size_t n = chain.node_count();
    std::vector<std::vector<Cx>> env(n + 1);
    env[n] = {Cx{1.0, 0.0}};
    for (std::size_t s = n; s-- > 0;) {
        const TensorNode& A = chain.node(static_cast<std::uint32_t>(s));
        const std::size_t left  = A.indices[kLeft].dim;
        const std::size_t right = A.indices[kRight].dim;
        if (env[s + 1].size() != right) {
            reject("mpo environment found an inconsistent bond at site " + std::to_string(s));
        }
        std::vector<Cx> next(left, Cx{0.0, 0.0});
        for (std::size_t ket = 0; ket < 2; ++ket) {
            for (std::size_t bra = 0; bra < 2; ++bra) {
                const Cx w = weight(s, ket, bra);
                if (w == Cx{0.0, 0.0}) continue;
                const std::size_t p = phys(ket, bra);
                for (std::size_t a = 0; a < left; ++a) {
                    Cx acc{0.0, 0.0};
                    for (std::size_t b = 0; b < right; ++b) {
                        acc += A.data[site_offset(a, p, b, right)] * env[s + 1][b];
                    }
                    next[a] += w * acc;
                }
            }
        }
        env[s] = std::move(next);
    }
    return env;
}

}  // namespace

double MpoState::trace() const {
    const auto env = right_environments(
        graph_, [](std::size_t, std::size_t ket, std::size_t bra) {
            return (ket == bra) ? Cx{1.0, 0.0} : Cx{0.0, 0.0};
        });
    return env[0].empty() ? 0.0 : env[0][0].real();
}

void MpoState::normalize_trace() {
    const double t = trace();
    if (!std::isfinite(t) || std::abs(t) < 1e-300) {
        reject("mpo trace collapsed to " + std::to_string(t) + "; the chain has diverged");
    }
    TensorNode& first = graph_.node(0);
    const double scale = 1.0 / t;
    for (Cx& v : first.data) v *= scale;
}

double MpoState::expectation_value(const std::vector<PauliOp>& ops) const {
    if (ops.size() != graph_.node_count()) {
        reject("mpo expectation_value received " + std::to_string(ops.size()) +
               " Pauli factors for a chain of " + std::to_string(graph_.node_count()) + " sites");
    }
    // Tr(rho P) = sum over ket,bra of rho[ket][bra] * P[bra][ket] — the physical
    // leg is closed with P transposed, which is the only difference from trace().
    const auto env = right_environments(
        graph_, [&ops](std::size_t site, std::size_t ket, std::size_t bra) {
            return pauli_element(ops[site], bra, ket);
        });
    return env[0].empty() ? 0.0 : env[0][0].real();
}

std::vector<int> MpoState::sample(std::mt19937_64& rng, double* negative_weight) const {
    const std::size_t n = graph_.node_count();
    // Diagonal environments: every un-sampled site is traced out, so the
    // environment is a VECTOR over one bond, not the (ket, bra) matrix the MPS
    // sampler carries. That is the one place the density matrix is cheaper.
    const auto env = right_environments(
        graph_, [](std::size_t, std::size_t ket, std::size_t bra) {
            return (ket == bra) ? Cx{1.0, 0.0} : Cx{0.0, 0.0};
        });

    std::vector<int> bits(n, 0);
    std::vector<Cx> v{Cx{1.0, 0.0}};
    std::vector<Cx> w0, w1;
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    double most_negative = 0.0;

    for (std::size_t site = 0; site < n; ++site) {
        const TensorNode& A     = graph_.node(static_cast<std::uint32_t>(site));
        const std::size_t left  = A.indices[kLeft].dim;
        const std::size_t right = A.indices[kRight].dim;
        if (v.size() != left) {
            reject("mpo sampling found an inconsistent bond at site " + std::to_string(site));
        }

        w0.assign(right, Cx{0.0, 0.0});
        w1.assign(right, Cx{0.0, 0.0});
        for (std::size_t a = 0; a < left; ++a) {
            const Cx va = v[a];
            if (va == Cx{0.0, 0.0}) continue;
            for (std::size_t b = 0; b < right; ++b) {
                w0[b] += va * A.data[site_offset(a, phys(0, 0), b, right)];
                w1[b] += va * A.data[site_offset(a, phys(1, 1), b, right)];
            }
        }

        const auto close = [&](const std::vector<Cx>& w) {
            Cx acc{0.0, 0.0};
            for (std::size_t b = 0; b < right; ++b) acc += w[b] * env[site + 1][b];
            return acc.real();
        };
        const double p0 = close(w0);
        const double p1 = close(w1);
        // Truncation does not preserve positivity, so a conditional can come out
        // negative. It is recorded and reported rather than clamped away
        // — but the draw still needs a usable weight, so the
        // negative branch is treated as unreachable for THIS shot.
        most_negative = std::min({most_negative, p0, p1});
        const double q0 = std::max(p0, 0.0);
        const double q1 = std::max(p1, 0.0);
        const double total = q0 + q1;
        if (!std::isfinite(total) || total <= 0.0) {
            reject("mpo sampling hit a degenerate conditional at site " + std::to_string(site) +
                   " (p0 = " + std::to_string(p0) + ", p1 = " + std::to_string(p1) + ")");
        }

        const int chosen = (u01(rng) * total < q0) ? 0 : 1;
        bits[site] = chosen;
        const std::vector<Cx>& w = (chosen == 0) ? w0 : w1;
        const double picked = (chosen == 0) ? q0 : q1;
        v.assign(right, Cx{0.0, 0.0});
        for (std::size_t b = 0; b < right; ++b) v[b] = w[b] / picked;
    }

    if (negative_weight != nullptr) *negative_weight = most_negative;
    return bits;
}

std::vector<Cx> MpoState::to_dense() const {
    if (num_qubits_ > MAX_DENSE_EXPORT_QUBITS) {
        reject("mpo to_dense exceeds MAX_DENSE_EXPORT_QUBITS");
    }
    const std::size_t dim = std::size_t{1} << num_qubits_;
    // Sweep left to right carrying [ket, bra, bond]; the physical legs stay open.
    std::vector<Cx> acc{Cx{1.0, 0.0}};
    std::size_t rows = 1;   // 2^site basis states accumulated on each side
    std::size_t bond = 1;

    for (QubitIndex site = 0; site < num_qubits_; ++site) {
        const TensorNode& A     = graph_.node(site);
        const std::size_t left  = A.indices[kLeft].dim;
        const std::size_t right = A.indices[kRight].dim;
        if (left != bond) reject("mpo to_dense found an inconsistent bond");

        std::vector<Cx> next(rows * 2 * rows * 2 * right, Cx{0.0, 0.0});
        for (std::size_t kr = 0; kr < rows; ++kr) {
            for (std::size_t br = 0; br < rows; ++br) {
                for (std::size_t a = 0; a < left; ++a) {
                    const Cx cur = acc[(kr * rows + br) * left + a];
                    if (cur == Cx{0.0, 0.0}) continue;
                    for (std::size_t ket = 0; ket < 2; ++ket) {
                        for (std::size_t bra = 0; bra < 2; ++bra) {
                            const std::size_t p = phys(ket, bra);
                            // Bit `site` of the basis index — same convention as
                            // the statevector backends (bit q is qubit q).
                            const std::size_t nk = kr | (ket << site);
                            const std::size_t nb = br | (bra << site);
                            for (std::size_t b = 0; b < right; ++b) {
                                next[(nk * rows * 2 + nb) * right + b] +=
                                    cur * A.data[site_offset(a, p, b, right)];
                            }
                        }
                    }
                }
            }
        }
        acc  = std::move(next);
        rows *= 2;
        bond = right;
    }
    if (bond != 1) reject("mpo to_dense chain did not close");

    std::vector<Cx> rho(dim * dim, Cx{0.0, 0.0});
    for (std::size_t kr = 0; kr < dim; ++kr) {
        for (std::size_t br = 0; br < dim; ++br) rho[kr * dim + br] = acc[(kr * dim + br) * 1];
    }
    return rho;
}

}  // namespace detail
}  // namespace qplayer
