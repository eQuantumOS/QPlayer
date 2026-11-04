/// @file mps.cpp
/// @brief MPS gate application and SVD truncation.
///
/// A two-qubit gate is the only operation that can grow entanglement, so it is
/// the only one that needs an SVD: merge the two neighbouring site tensors,
/// multiply the 4x4 unitary in, split with zgesvd_, and drop the singular values
/// the policy says to drop. One-qubit gates touch a single site and
/// leave every bond dimension alone.
///
/// zgesvd_ (not dgesvd_) because site tensors are complex<double>.
/// LAPACK is column-major, so merged tensors are built column-major directly
/// rather than transposed afterwards.

#include "backend/tensor_network/mps.hpp"

#include "error.hpp"
#include "backend/tensor_network/blas_loader.hpp"
#include "backend/tensor_network/tn_svd_gpu.hpp"
#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <cstdlib>
#include <string>

// LAPACK complex SVD, resolved from OpenBLAS at run time rather than linked:
// linking it would build a thread pool in every process, including those that
// never touch TensorNetwork.

namespace qplayer {
namespace detail {

namespace {

using Cx = std::complex<double>;

constexpr const char* kBackend = "TensorNetwork";

// Site tensor index positions — mirrors the layout tn_cpu.cpp builds.
constexpr std::uint32_t kLeft     = 0;
constexpr std::uint32_t kPhysical = 1;
constexpr std::uint32_t kRight    = 2;

[[noreturn]] void reject(const std::string& what) {
    throw SimulatorError{kBackend, what};
}

// Row-major offset inside a [left, 2, right] site tensor.
inline std::size_t site_offset(std::size_t a, std::size_t s, std::size_t b, std::size_t right) {
    return (a * 2 + s) * right + b;
}

// Is this 2x2 unitary? u^H u == I within rounding. Cheap enough to ask on every
// 1Q gate, and the answer decides whether the canonical gauge survives it.
bool is_unitary_2x2(const std::vector<Cx>& u) {
    constexpr double kTol = 1e-12;
    const Cx c00 = std::conj(u[0]) * u[0] + std::conj(u[2]) * u[2];
    const Cx c11 = std::conj(u[1]) * u[1] + std::conj(u[3]) * u[3];
    const Cx c01 = std::conj(u[0]) * u[1] + std::conj(u[2]) * u[3];
    return std::abs(c00 - Cx{1, 0}) <= kTol && std::abs(c11 - Cx{1, 0}) <= kTol &&
           std::abs(c01) <= kTol;
}


// Step error above which a split counts as lossy for MpsGauge::Auto.
// Rank-deficient blocks leave tails at rounding level (~1e-15 relative); a real
// truncation under the default cutoff 1e-12 or a binding cap is far above this.
constexpr double kGaugeArmStepError = 1e-10;

// Reads one env var as a positive double; returns false when unset or unusable.
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

}  // namespace

// ── MpsTruncationConfig ─────────────────────────────────────────────────────

MpsTruncationConfig MpsTruncationConfig::from_env() {
    MpsTruncationConfig cfg;

    double bond = 0.0;
    if (env_double("QPLAYER_MPS_MAX_BOND", &bond)) {
        const auto rounded = static_cast<std::uint32_t>(bond);
        // Out-of-range env values fall back instead of throwing: the process may
        // be mid-way through a long job and a startup abort helps nobody.
        if (rounded >= 1 && rounded <= kMpsMaxBondCeiling && static_cast<double>(rounded) == bond) {
            cfg.max_bond = rounded;
        } else {
            log::warn("QPLAYER_MPS_MAX_BOND must be an integer in [1, " +
                      std::to_string(kMpsMaxBondCeiling) + "]; keeping default " +
                      std::to_string(cfg.max_bond));
        }
    }

    double cutoff = 0.0;
    if (env_double("QPLAYER_MPS_CUTOFF", &cutoff)) cfg.cutoff = cutoff;

    double budget = 0.0;
    if (env_double("QPLAYER_MPS_ERROR_BUDGET", &budget)) cfg.error_budget_warn = budget;

    // the escape hatch back to the pre-gauge split, e.g. to reproduce
    // counts recorded before the gauge was wired.
    if (const char* raw = std::getenv("QPLAYER_MPS_GAUGE"); raw != nullptr && *raw != '\0') {
        const std::string v{raw};
        if (v == "auto") {
            cfg.gauge = MpsGauge::Auto;
        } else if (v == "canonical") {
            cfg.gauge = MpsGauge::Canonical;
        } else if (v == "none") {
            cfg.gauge = MpsGauge::None;
        } else {
            log::warn("QPLAYER_MPS_GAUGE: ignoring unknown value '" + v +
                      "' (expected 'auto', 'canonical' or 'none')");
        }
    }

    return cfg;
}

void MpsTruncationConfig::validate() const {
    if (max_bond == 0) {
        throw CircuitError{"mps max_bond must be at least 1"};
    }
    if (max_bond > kMpsMaxBondCeiling) {
        throw CircuitError{"mps max_bond " + std::to_string(max_bond) +
                           " exceeds the ceiling " + std::to_string(kMpsMaxBondCeiling) +
                           " (one SVD at that width costs minutes per gate)"};
    }
    if (!(cutoff >= 0.0)) {
        throw CircuitError{"mps cutoff must be non-negative"};
    }
}

// ── Construction ────────────────────────────────────────────────────────────

MpsState::MpsState(QubitIndex num_qubits, MpsTruncationConfig config)
    : num_qubits_(num_qubits),
      graph_(TensorGraph::mps_zero_state(num_qubits)),
      config_(config) {
    config_.validate();
    // Only the two trivial end environments exist before the first query.
    if (num_qubits_ > 0) {
        env_left_.assign(num_qubits_, {});
        env_right_.assign(num_qubits_, {});
        env_left_.front()  = {Cx{1.0, 0.0}};
        env_right_.back()  = {Cx{1.0, 0.0}};
        env_right_valid_   = num_qubits_ - 1;
    }
}

// ── One-qubit gates ─────────────────────────────────────────────────────────

void MpsState::apply_1q(const std::vector<Cx>& u, QubitIndex qubit) {
    if (qubit >= num_qubits_) reject("apply_1q qubit " + std::to_string(qubit) + " out of range");
    if (u.size() != 4)        reject("apply_1q expects a 2x2 matrix (4 entries)");

    // A non-unitary factor (Kraus branch, projector) is absorbed AT the centre so
    // the gauge survives it; elsewhere it would have to be dropped.
    const bool unitary = is_unitary_2x2(u);
    if (gauge_active() && !unitary && centre_ != qubit) move_centre(qubit);

    TensorNode& site = graph_.node(qubit);
    const std::size_t left  = site.indices[kLeft].dim;
    const std::size_t right = site.indices[kRight].dim;

    // Contracting only the physical leg leaves both bonds untouched, which is
    // why a 1Q gate can never need truncation.
    for (std::size_t a = 0; a < left; ++a) {
        for (std::size_t b = 0; b < right; ++b) {
            const Cx v0 = site.data[site_offset(a, 0, b, right)];
            const Cx v1 = site.data[site_offset(a, 1, b, right)];
            site.data[site_offset(a, 0, b, right)] = u[0] * v0 + u[1] * v1;
            site.data[site_offset(a, 1, b, right)] = u[2] * v0 + u[3] * v1;
        }
    }

    // Gauge bookkeeping. A UNITARY 1Q gate preserves orthonormality
    // for free: the reshaped M becomes (u (x) I) M, so M^H M is unchanged. A
    // non-unitary one -- a Kraus operator, a measurement projector -- does not,
    // and then only the centre may absorb it. Everywhere else the gauge is
    // DROPPED rather than left stale, because a stale centre would make the next
    // truncation silently non-optimal while claiming otherwise.
    if (centre_ < num_qubits_ && qubit != centre_ && !unitary) {
        centre_ = num_qubits_;
    }

    // A unitary leaves every environment through this site unchanged, because
    // sum_s (uA)_s^H E (uA)_s = sum_s A_s^H E A_s when u^H u = I.
    // A Kraus branch rescaled by sqrt(total/p) is often exactly unitary (the
    // Pauli branches of depolarizing), and it is then right to keep the cache.
    if (!unitary) invalidate_environments(qubit, qubit);
}

// ── Two-qubit gates ─────────────────────────────────────────────────────────

double MpsState::apply_2q_adjacent(const std::vector<Cx>& u, QubitIndex site, bool swap_operands,
                                   bool centre_left) {
    // a split is globally optimal only when the centre lies inside the
    // block, because only then are both environments isometries. The walk below
    // leaves it there already, so this moves at most once per user gate.
    if (gauge_active()) {
        if (centre_ >= num_qubits_)  canonicalize(site);
        else if (centre_ < site)     move_centre(site);
        else if (centre_ > site + 1) move_centre(site + 1);
    }
    TensorNode& A = graph_.node(site);
    TensorNode& B = graph_.node(site + 1);

    const std::size_t l = A.indices[kLeft].dim;
    const std::size_t m = A.indices[kRight].dim;
    const std::size_t r = B.indices[kRight].dim;

    const int rows = static_cast<int>(l * 2);
    const int cols = static_cast<int>(2 * r);
    const int k    = std::min(rows, cols);

    // Merge into a column-major (l*2) x (2*r) matrix: row (a, s1), column (s2, b).
    // Built column-major up front so LAPACK can read it without a transpose.
    //
    // This contraction over the shared bond IS a GEMM, and the stored layouts
    // let one call do it with no repacking. site_offset() makes A a row-major
    // (l*2) x m matrix and B a row-major m x (2*r) one; a row-major matrix read
    // as column-major is its own transpose, so passing both with "T" recovers
    // the intended product and writes it column-major straight into `mat`.
    // Hand-rolling the four loops instead cost 24.8% of MPS time in
    // profiling, doing scalar work that a blocked, vectorised kernel does far
    // better -- and BLAS is already loaded here for zgesvd_.
    std::vector<Cx> mat(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), Cx{0, 0});
    {
        const int      kk    = static_cast<int>(m);
        const int      lda   = kk;             // A stored column-major m x (l*2)
        const int      ldb   = cols;           // B stored column-major (2*r) x m
        const Cx       one{1.0, 0.0};
        const Cx       zero{0.0, 0.0};
        qplayer::detail::zgemm("T", "T", &rows, &cols, &kk, &one,
                               A.data.data(), &lda, B.data.data(), &ldb,
                               &zero, mat.data(), &rows);
    }

    // Apply the gate on the (s1, s2) pair for every (a, b). The matrix is in
    // OPERAND order — local bit 0 is q0 — so when q0 sits on the right site the
    // two physical bits swap roles. Getting this backwards silently transposes
    // control and target (the trap gate_fusion.cpp documents).
    const auto pack = [swap_operands](std::size_t s_left, std::size_t s_right) -> std::size_t {
        return swap_operands ? (s_right + 2 * s_left) : (s_left + 2 * s_right);
    };
    for (std::size_t a = 0; a < l; ++a) {
        for (std::size_t b = 0; b < r; ++b) {
            Cx in[4];
            for (std::size_t s1 = 0; s1 < 2; ++s1) {
                for (std::size_t s2 = 0; s2 < 2; ++s2) {
                    const std::size_t row = a * 2 + s1;
                    const std::size_t col = s2 * r + b;
                    in[pack(s1, s2)] = mat[row + col * static_cast<std::size_t>(rows)];
                }
            }
            Cx out[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
            for (std::size_t o = 0; o < 4; ++o) {
                for (std::size_t i = 0; i < 4; ++i) out[o] += u[o * 4 + i] * in[i];
            }
            for (std::size_t s1 = 0; s1 < 2; ++s1) {
                for (std::size_t s2 = 0; s2 < 2; ++s2) {
                    const std::size_t row = a * 2 + s1;
                    const std::size_t col = s2 * r + b;
                    mat[row + col * static_cast<std::size_t>(rows)] = out[pack(s1, s2)];
                }
            }
        }
    }

    // SVD: mat = U * diag(S) * VT, thin form ('S') because only k columns matter.
    std::vector<double> sv(static_cast<std::size_t>(k), 0.0);
    std::vector<Cx> U(static_cast<std::size_t>(rows) * static_cast<std::size_t>(k), Cx{0, 0});
    std::vector<Cx> VT(static_cast<std::size_t>(k) * static_cast<std::size_t>(cols), Cx{0, 0});
    // LAPACK destroys its input; the cuSolver path does not (tn_svd_gpu.hpp).
    // That asymmetry is deliberate -- it lets the degeneracy fallback below
    // re-run on the CPU without keeping a backup copy of `mat`.
    auto run_cpu_svd = [&]() {
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
    };

    // GPU offload: declines by itself when the build has no CUDA, the
    // host has no device, or min(rows, cols) is below the measured break-even.
    const bool used_gpu =
        config_.gpu_svd &&
        qplayer::detail::gpu_zgesvd_thin(rows, cols, mat.data(), sv.data(), U.data(), VT.data());
    if (!used_gpu) run_cpu_svd();

    // Truncation rule (A): keep the FEWEST singular values whose
    // discarded tail stays within cutoff, then cap that at max_bond.
    double total_sq    = 0.0;
    double norm        = 0.0;
    int    keep_cutoff = k;
    auto decide_keep = [&]() -> int {
        total_sq = 0.0;
        for (int i = 0; i < k; ++i)
            total_sq += sv[static_cast<std::size_t>(i)] * sv[static_cast<std::size_t>(i)];
        norm        = std::sqrt(total_sq);
        keep_cutoff = k;
        if (norm > 0.0) {
            double tail = 0.0;
            for (int i = k - 1; i >= 1; --i) {
                tail += sv[static_cast<std::size_t>(i)] * sv[static_cast<std::size_t>(i)];
                if (std::sqrt(tail) / norm > config_.cutoff) break;
                keep_cutoff = i;
            }
        }
        return std::max(1, std::min<int>(static_cast<int>(config_.max_bond), keep_cutoff));
    };
    int keep = decide_keep();

    // E: inside a degenerate block the retained SUBSPACE is not unique,
    // so cuSolver and LAPACK may keep different -- and equally valid -- bases.
    // Measured: the truncated matrix then differs by 0.46~0.67 in relative
    // Frobenius norm while the discarded weight is identical. QPlayer does not
    // let the processor change the answer, so that one call goes back to the
    // CPU. The check is O(1), and outside stabilizer circuits with a binding cap
    // it is measure-zero.
    //
    // The gap is measured RELATIVE TO THE BOUNDARY VALUE, not to sv[0]. Scaling
    // by the largest singular value makes any two neighbours in the tail of a
    // smooth spectrum look degenerate -- measured: a random brickwork circuit
    // tripped the fallback on every run and lost the whole speedup, with no
    // degeneracy anywhere in it.
    const double boundary = sv[static_cast<std::size_t>(keep) - 1];
    const bool splits_degenerate =
        keep < k && boundary > 0.0 &&
        (boundary - sv[static_cast<std::size_t>(keep)]) <=
            qplayer::detail::kGpuSvdDegenerateTol * boundary;
    if (used_gpu && splits_degenerate) {
        run_cpu_svd();
        keep = decide_keep();
        static std::once_flag warned;
        std::call_once(warned, [] {
            log::warn("MPS GPU SVD: a truncation boundary split a degenerate block; "
                      "recomputing on the CPU so the result does not depend on the "
                      "processor. Expect no GPU speedup on this circuit.");
        });
    }
    const bool bond_capped = keep < keep_cutoff;

    double kept_sq = 0.0;
    for (int i = 0; i < keep; ++i) kept_sq += sv[static_cast<std::size_t>(i)] * sv[static_cast<std::size_t>(i)];
    const double discarded_sq = std::max(0.0, total_sq - kept_sq);
    const double step_error   = (norm > 0.0) ? std::sqrt(discarded_sq) / norm : 0.0;

    // MpsGauge::Auto: the first split that really discards weight arms
    // the gauge and is redone at the centre. Nothing has been written yet, so the
    // redo starts from the same state. Below the threshold the discarded tail is
    // rounding noise of a rank-deficient block, where the gauge cannot matter.
    if (config_.gauge == MpsGauge::Auto && !gauge_armed_ &&
        (bond_capped || step_error > kGaugeArmStepError)) {
        gauge_armed_ = true;
        canonicalize(site);
        return apply_2q_adjacent(u, site, swap_operands, centre_left);
    }

    // Renormalise: without this the lost weight silently deflates every later
    // probability (A).
    const double kept_norm = std::sqrt(kept_sq);
    const double rescale   = (kept_norm > 0.0) ? (norm / kept_norm) : 1.0;

    const std::size_t chi = static_cast<std::size_t>(keep);

    // Split back: A' = U[:, :chi], B' = diag(S[:chi]) * VT[:chi, :] -- or, when
    // the centre is to stay on the left, A' = U * diag(S), B' = VT.
    const bool centre_on_left = gauge_active() && centre_left;
    std::vector<Cx> a_data(l * 2 * chi, Cx{0, 0});
    for (std::size_t a = 0; a < l; ++a) {
        for (std::size_t s1 = 0; s1 < 2; ++s1) {
            for (std::size_t c = 0; c < chi; ++c) {
                const std::size_t row = a * 2 + s1;
                const double w = centre_on_left ? sv[c] * rescale : 1.0;
                a_data[site_offset(a, s1, c, chi)] = w * U[row + c * static_cast<std::size_t>(rows)];
            }
        }
    }
    std::vector<Cx> b_data(chi * 2 * r, Cx{0, 0});
    for (std::size_t c = 0; c < chi; ++c) {
        const double scaled = centre_on_left ? 1.0 : sv[c] * rescale;
        for (std::size_t s2 = 0; s2 < 2; ++s2) {
            for (std::size_t b = 0; b < r; ++b) {
                const std::size_t col = s2 * r + b;
                b_data[site_offset(c, s2, b, r)] =
                    scaled * VT[c + col * static_cast<std::size_t>(k)];
            }
        }
    }

    A.data = std::move(a_data);
    A.indices[kRight].dim = static_cast<std::uint32_t>(chi);
    B.data = std::move(b_data);
    B.indices[kLeft].dim = static_cast<std::uint32_t>(chi);
    // The site that took S holds the whole norm; the other is an isometry. Without
    // the canonical gauge the environments were not isometries to begin with, so
    // the split leaves no gauge behind.
    if (gauge_active()) {
        centre_ = centre_on_left ? site : site + 1;
    } else {
        centre_ = num_qubits_;
    }
    invalidate_environments(site, site + 1);

    // Peak bond over the state's life. This is the only place a bond
    // is resized, so a running max here sees every value the chain ever held --
    // including the ones a SWAP round trip puts up and takes back down, which is
    // exactly what an end-of-run read misses.
    peak_bond_ = std::max(peak_bond_, static_cast<std::uint32_t>(chi));

    if (bond_capped && step_error > config_.cutoff && !warned_lossy_) {
        warned_lossy_ = true;
        log::warn("MPS bond cap " + std::to_string(config_.max_bond) +
                  " forced a lossy truncation (step error " + std::to_string(step_error) +
                  "); raise QPLAYER_MPS_MAX_BOND or inspect Result::truncation_errors()");
    }
    return step_error;
}

double MpsState::apply_2q_walked(const std::vector<Cx>& u, QubitIndex q0, QubitIndex q1) {
    // SWAP as a plain permutation unitary: it exchanges the two sites' states
    // while site q keeps carrying qubit q, so no index bookkeeping is needed.
    static const std::vector<Cx> kSwap = {
        {1, 0}, {0, 0}, {0, 0}, {0, 0},
        {0, 0}, {0, 0}, {1, 0}, {0, 0},
        {0, 0}, {1, 0}, {0, 0}, {0, 0},
        {0, 0}, {0, 0}, {0, 0}, {1, 0},
    };

    const QubitIndex lo = std::min(q0, q1);
    const QubitIndex hi = std::max(q0, q1);

    // Walk the far qubit next to its partner, act, then walk it home. The errors
    // of the SWAP steps belong to the CALLER's gate, so they are returned rather
    // than recorded -- the caller folds them into the single entry
    // specifies (one per user gate, not one per internal SVD).
    double squared = 0.0;
    for (QubitIndex s = hi; s > lo + 1; --s) {
        // Leftward steps leave the centre on the left site, which is inside the
        // next block, so the walk costs no centre moves.
        const double e = apply_2q_adjacent(kSwap, s - 1, /*swap_operands=*/false,
                                           /*centre_left=*/true);
        squared += e * e;
        ++swap_steps_;
    }

    const bool q0_is_right = (q0 == hi);
    const double gate_error = apply_2q_adjacent(u, lo, q0_is_right);
    squared += gate_error * gate_error;

    // Mirror of the outbound walk: swaps at (lo+1, lo+2) ... (hi-1, hi), which
    // is the same count. Stopping one site short leaves the chain permuted and
    // every later gate lands on the wrong qubit.
    for (QubitIndex s = lo + 1; s < hi; ++s) {
        const double e = apply_2q_adjacent(kSwap, s, /*swap_operands=*/false);
        squared += e * e;
        ++swap_steps_;
    }

    return std::sqrt(squared);
}

void MpsState::apply_2q(const std::vector<Cx>& u, QubitIndex q0, QubitIndex q1) {
    if (q0 >= num_qubits_ || q1 >= num_qubits_) {
        reject("apply_2q qubit out of range");
    }
    if (q0 == q1) reject("apply_2q needs two distinct qubits");
    if (u.size() != 16) reject("apply_2q expects a 4x4 matrix (16 entries)");

    note_truncation(apply_2q_walked(u, q0, q1), /*bond_capped=*/false);
}

// ── Three-qubit gates: fixed decomposition ───────────────────────

namespace {

// Operand-order 4x4 CX with the FIRST operand as control. Same convention as
// kSwap above: row-major, row = output basis index, column = input, and the
// basis index is `b0 + 2*b1` with b0 the first operand's bit.
const std::vector<Cx>& cx_first_is_control() {
    static const std::vector<Cx> m = {
        {1, 0}, {0, 0}, {0, 0}, {0, 0},
        {0, 0}, {0, 0}, {0, 0}, {1, 0},
        {0, 0}, {0, 0}, {1, 0}, {0, 0},
        {0, 0}, {1, 0}, {0, 0}, {0, 0},
    };
    return m;
}

const std::vector<Cx>& h_1q() {
    static const double r = 1.0 / std::sqrt(2.0);
    static const std::vector<Cx> m = {{r, 0}, {r, 0}, {r, 0}, {-r, 0}};
    return m;
}

// T = diag(1, e^{i pi/4}) and its adjoint. Written as literals rather than
// through GateType so this file stays free of the IR's matrix builder.
const std::vector<Cx>& t_1q() {
    static const double c = std::cos(M_PI / 4.0);
    static const std::vector<Cx> m = {{1, 0}, {0, 0}, {0, 0}, {c, c}};
    return m;
}

const std::vector<Cx>& tdg_1q() {
    static const double c = std::cos(M_PI / 4.0);
    static const std::vector<Cx> m = {{1, 0}, {0, 0}, {0, 0}, {c, -c}};
    return m;
}

}  // namespace

void MpsState::apply_3q(GateType gate, QubitIndex q0, QubitIndex q1, QubitIndex q2) {
    if (q0 >= num_qubits_ || q1 >= num_qubits_ || q2 >= num_qubits_) {
        reject("apply_3q qubit out of range");
    }
    if (q0 == q1 || q0 == q2 || q1 == q2) {
        reject("apply_3q needs three distinct qubits");
    }

    double squared = 0.0;
    // Only 2Q steps can truncate: a 1Q gate leaves both bonds untouched, so it
    // contributes nothing to the folded error (see apply_1q).
    const auto cx = [&](QubitIndex c, QubitIndex tgt) {
        const double e = apply_2q_walked(cx_first_is_control(), c, tgt);
        squared += e * e;
    };

    // CCX(a, b, t) as the standard 6-CX + 7 T/Tdg + 2 H template. Verified
    // against the dense Toffoli at max|delta| = 3.14e-16.
    const auto ccx = [&](QubitIndex a, QubitIndex b, QubitIndex tgt) {
        apply_1q(h_1q(), tgt);
        cx(b, tgt);   apply_1q(tdg_1q(), tgt);
        cx(a, tgt);   apply_1q(t_1q(),   tgt);
        cx(b, tgt);   apply_1q(tdg_1q(), tgt);
        cx(a, tgt);
        apply_1q(t_1q(), b);
        apply_1q(t_1q(), tgt);
        apply_1q(h_1q(), tgt);
        cx(a, b);
        apply_1q(t_1q(),   a);
        apply_1q(tdg_1q(), b);
        cx(a, b);
    };

    switch (gate) {
        case GateType::CCX:
            ccx(q0, q1, q2);
            break;

        case GateType::CSwap:
            // Fredkin(c; x, y) = CX(y->x) . CCX(c, x, y) . CX(y->x), exact
            // (verified max|delta| = 0). Reducing to CCX keeps ONE decomposition
            // template in the file instead of two that can drift apart.
            cx(q2, q1);
            ccx(q0, q1, q2);
            cx(q2, q1);
            break;

        default:
            reject("apply_3q supports only CCX and CSwap");
    }

    // One entry for the whole decomposition. bond_capped
    // is false because the per-step cap warning already fired inside whichever
    // apply_2q_adjacent hit the cap.
    note_truncation(std::sqrt(squared), /*bond_capped=*/false);
}

namespace {

// Row-major (rows x cols) -> column-major, for LAPACK. O(chi^2) against an
// O(chi^3) factorisation, and cheap insurance: deriving the transposed SVD in
// place is where sign and conjugation mistakes hide.
std::vector<Cx> pack_colmajor(const Cx* src, int rows, int cols) {
    std::vector<Cx> out(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j)
            out[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * rows] =
                src[static_cast<std::size_t>(i) * cols + j];
    return out;
}

// Thin SVD of a column-major rows x cols matrix (destroys `a`).
void thin_svd(std::vector<Cx>& a, int rows, int cols, std::vector<double>& sv,
              std::vector<Cx>& u, std::vector<Cx>& vt) {
    const int k = std::min(rows, cols);
    sv.assign(static_cast<std::size_t>(k), 0.0);
    u.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(k), Cx{0, 0});
    vt.assign(static_cast<std::size_t>(k) * static_cast<std::size_t>(cols), Cx{0, 0});
    std::vector<double> rwork(static_cast<std::size_t>(5 * k), 0.0);
    int info = 0, lwork = -1;
    Cx query{0, 0};
    qplayer::detail::zgesvd("S", "S", &rows, &cols, a.data(), &rows, sv.data(),
                            u.data(), &rows, vt.data(), &k, &query, &lwork, rwork.data(), &info);
    if (info != 0) {
        throw SimulatorError{"TensorNetwork",
            "canonicalize: zgesvd workspace query failed with info=" + std::to_string(info)};
    }
    lwork = static_cast<int>(query.real());
    std::vector<Cx> work(static_cast<std::size_t>(std::max(lwork, 1)), Cx{0, 0});
    qplayer::detail::zgesvd("S", "S", &rows, &cols, a.data(), &rows, sv.data(),
                            u.data(), &rows, vt.data(), &k, work.data(), &lwork, rwork.data(), &info);
    if (info != 0) {
        throw SimulatorError{"TensorNetwork",
            "canonicalize: zgesvd failed to converge (info=" + std::to_string(info) + ")"};
    }
}

}  // namespace

bool mps_is_left_orthonormal(const TensorGraph& chain, std::size_t site, double tol) {
    const TensorNode& A = chain.node(static_cast<std::uint32_t>(site));
    const std::size_t rows = A.indices[kLeft].dim * 2;
    const std::size_t cols = A.indices[kRight].dim;
    for (std::size_t c = 0; c < cols; ++c) {
        for (std::size_t d = 0; d < cols; ++d) {
            Cx acc{0, 0};
            for (std::size_t i = 0; i < rows; ++i)
                acc += std::conj(A.data[i * cols + c]) * A.data[i * cols + d];
            const Cx want = (c == d) ? Cx{1, 0} : Cx{0, 0};
            if (std::abs(acc - want) > tol) return false;
        }
    }
    return true;
}

bool mps_is_right_orthonormal(const TensorGraph& chain, std::size_t site, double tol) {
    const TensorNode& A = chain.node(static_cast<std::uint32_t>(site));
    const std::size_t rows = A.indices[kLeft].dim;
    const std::size_t cols = A.indices[kRight].dim * 2;
    for (std::size_t a = 0; a < rows; ++a) {
        for (std::size_t b = 0; b < rows; ++b) {
            Cx acc{0, 0};
            for (std::size_t j = 0; j < cols; ++j)
                acc += A.data[a * cols + j] * std::conj(A.data[b * cols + j]);
            const Cx want = (a == b) ? Cx{1, 0} : Cx{0, 0};
            if (std::abs(acc - want) > tol) return false;
        }
    }
    return true;
}

void MpsState::canonicalize(QubitIndex centre) {
    if (centre >= num_qubits_) {
        throw SimulatorError{"TensorNetwork",
            "canonicalize centre " + std::to_string(centre) + " is out of range for " +
            std::to_string(num_qubits_) + " sites"};
    }
    const Cx one{1.0, 0.0};
    const Cx zero{0.0, 0.0};
    std::vector<double> sv;
    std::vector<Cx> u, vt;

    // Right-to-left first, so the left sweep below sees the shrunk bonds.
    for (std::size_t k = num_qubits_; k-- > static_cast<std::size_t>(centre) + 1;) {
        make_right_orthonormal(k);
    }
    for (std::size_t k = 0; k < static_cast<std::size_t>(centre); ++k) {
        make_left_orthonormal(k);
    }
    centre_ = centre;
}

// Makes site k right-orthonormal and folds the carry into k-1. This is one step
// of the centre moving LEFT, and the same operation the right-to-left sweep of
// canonicalize() repeats.
void MpsState::make_right_orthonormal(std::size_t k) {
    const Cx one{1.0, 0.0};
    const Cx zero{0.0, 0.0};
    std::vector<double> sv;
    std::vector<Cx> u, vt;
    invalidate_environments(static_cast<QubitIndex>(k - 1), static_cast<QubitIndex>(k));
    {
        TensorNode& A    = graph_.node(static_cast<std::uint32_t>(k));
        TensorNode& prev = graph_.node(static_cast<std::uint32_t>(k - 1));
        const int rows = static_cast<int>(A.indices[kLeft].dim);
        const int cols = static_cast<int>(2 * A.indices[kRight].dim);
        const int rank = std::min(rows, cols);
        const int right = static_cast<int>(A.indices[kRight].dim);

        std::vector<Cx> mat = pack_colmajor(A.data.data(), rows, cols);
        thin_svd(mat, rows, cols, sv, u, vt);

        // A <- V^H: right-orthonormal because (V^H)(V^H)^H = I over the new left bond.
        std::vector<Cx> a_new(static_cast<std::size_t>(rank) * cols, Cx{0, 0});
        for (int c = 0; c < rank; ++c)
            for (int j = 0; j < cols; ++j)
                a_new[static_cast<std::size_t>(c) * cols + j] =
                    vt[static_cast<std::size_t>(c) + static_cast<std::size_t>(j) * rank];

        // carry = U * S, folded left. col-major rows x rank.
        for (int c = 0; c < rank; ++c)
            for (int i = 0; i < rows; ++i)
                u[static_cast<std::size_t>(i) + static_cast<std::size_t>(c) * rows] *=
                    sv[static_cast<std::size_t>(c)];

        // prev <- prev * carry. Row-major (left'*2 x rows) * (rows x rank).
        // col-major: C^T = carry^T * prev^T  =>  ("T","N", rank, left'*2, rows).
        const int prev_rows = static_cast<int>(prev.indices[kLeft].dim) * 2;
        std::vector<Cx> p_new(static_cast<std::size_t>(prev_rows) * rank, Cx{0, 0});
        qplayer::detail::zgemm("T", "N", &rank, &prev_rows, &rows, &one,
                               u.data(), &rows, prev.data.data(), &rows, &zero,
                               p_new.data(), &rank);

        A.data = std::move(a_new);
        A.indices[kLeft].dim = static_cast<std::uint32_t>(rank);
        prev.data = std::move(p_new);
        prev.indices[kRight].dim = static_cast<std::uint32_t>(rank);
        static_cast<void>(right);
    }
}

// Makes site k left-orthonormal and folds the carry into k+1 -- one step of the
// centre moving RIGHT.
void MpsState::make_left_orthonormal(std::size_t k) {
    const Cx one{1.0, 0.0};
    const Cx zero{0.0, 0.0};
    std::vector<double> sv;
    std::vector<Cx> u, vt;
    invalidate_environments(static_cast<QubitIndex>(k), static_cast<QubitIndex>(k + 1));
    {
        TensorNode& A    = graph_.node(static_cast<std::uint32_t>(k));
        TensorNode& next = graph_.node(static_cast<std::uint32_t>(k + 1));
        const int rows = static_cast<int>(A.indices[kLeft].dim) * 2;
        const int cols = static_cast<int>(A.indices[kRight].dim);
        const int rank = std::min(rows, cols);

        std::vector<Cx> mat = pack_colmajor(A.data.data(), rows, cols);
        thin_svd(mat, rows, cols, sv, u, vt);

        // A <- U: left-orthonormal because U^H U = I over the new right bond.
        std::vector<Cx> a_new(static_cast<std::size_t>(rows) * rank, Cx{0, 0});
        for (int i = 0; i < rows; ++i)
            for (int c = 0; c < rank; ++c)
                a_new[static_cast<std::size_t>(i) * rank + c] =
                    u[static_cast<std::size_t>(i) + static_cast<std::size_t>(c) * rows];

        // carry = S * V^H, folded right. col-major rank x cols.
        for (int j = 0; j < cols; ++j)
            for (int c = 0; c < rank; ++c)
                vt[static_cast<std::size_t>(c) + static_cast<std::size_t>(j) * rank] *=
                    sv[static_cast<std::size_t>(c)];

        // next <- carry * next. Row-major (rank x cols) * (cols x 2*right').
        // col-major: C^T = next^T * carry^T  =>  ("N","T", 2*right', rank, cols).
        const int next_cols = 2 * static_cast<int>(next.indices[kRight].dim);
        std::vector<Cx> n_new(static_cast<std::size_t>(rank) * next_cols, Cx{0, 0});
        qplayer::detail::zgemm("N", "T", &next_cols, &rank, &cols, &one,
                               next.data.data(), &next_cols, vt.data(), &rank, &zero,
                               n_new.data(), &next_cols);

        A.data = std::move(a_new);
        A.indices[kRight].dim = static_cast<std::uint32_t>(rank);
        next.data = std::move(n_new);
        next.indices[kLeft].dim = static_cast<std::uint32_t>(rank);
    }
}

// Brings the orthogonality centre to `target`, paying one factorisation per site
// crossed. With no gauge held it falls back to a full sweep -- which is why a
// DROPPED gauge is safe and a STALE one is not.
void MpsState::move_centre(std::size_t target) {
    if (centre_ >= num_qubits_) {
        canonicalize(static_cast<QubitIndex>(target));
        return;
    }
    while (static_cast<std::size_t>(centre_) < target) {
        make_left_orthonormal(static_cast<std::size_t>(centre_));
        ++centre_;
    }
    while (static_cast<std::size_t>(centre_) > target) {
        make_right_orthonormal(static_cast<std::size_t>(centre_));
        --centre_;
    }
}

void MpsState::note_truncation(double step_error, bool /*bond_capped*/) {
    truncation_errors_.push_back(step_error);
    squared_error_sum_ += step_error * step_error;
    error_sum_ += step_error;

    if (!warned_budget_ && total_truncation_error() > config_.error_budget_warn) {
        warned_budget_ = true;
        // Warn and keep going: the error is quantified and recorded, so the user
        // can judge the result rather than lose it (C).
        log::warn("MPS accumulated truncation error " +
                  std::to_string(total_truncation_error()) + " exceeds the budget " +
                  std::to_string(config_.error_budget_warn) +
                  "; results remain available via Result::truncation_errors()");
    }
}

double MpsState::total_truncation_error() const noexcept {
    return std::sqrt(squared_error_sum_);
}

void MpsState::rescale(double factor) {
    if (!std::isfinite(factor)) {
        reject("rescale factor is not finite");
    }
    // One site carries the whole scale: the chain is a product of tensors, so
    // spreading factor^(1/n) over every site would cost n times as much and
    // introduce rounding at every step for no gain.
    // Fold into the orthogonality centre when a gauge is held: scaling any other
    // site would destroy its orthonormality, and the centre is the one tensor
    // allowed to carry weight.
    const std::uint32_t target = (centre_ < num_qubits_) ? centre_ : 0u;
    TensorNode& site = graph_.node(target);
    for (Cx& z : site.data) z *= factor;
    invalidate_environments(target, target);
}

// ── Inspection ──────────────────────────────────────────────────────────────

double MpsState::squared_norm() const {
    // <psi|psi> is the trace of any one-site marginal, so this reads the cached
    // environments. The sweep it replaces was commented O(n*chi^3)
    // but nested b, bp, a, ap -- O(n*chi^4) -- and at chi 256 on 49 sites it was
    // where every noisy surface-code trajectory spent its time.
    if (num_qubits_ == 0) return 1.0;
    const std::vector<Cx> rho = site_marginal(num_qubits_ - 1);
    return rho[0].real() + rho[3].real();
}

std::vector<Cx> MpsState::to_dense() const {
    if (num_qubits_ > MAX_DENSE_EXPORT_QUBITS) {
        reject("to_dense exceeds MAX_DENSE_EXPORT_QUBITS (" +
               std::to_string(MAX_DENSE_EXPORT_QUBITS) + ")");
    }

    // Sweep left to right carrying (partial basis index, open bond). Site q's
    // physical bit is placed at bit q so the result matches the statevector
    // backends' ordering exactly — otherwise cross-checks compare permutations.
    std::vector<Cx> cur{Cx{1, 0}};
    std::size_t bond = 1;

    for (QubitIndex q = 0; q < num_qubits_; ++q) {
        const TensorNode& site = graph_.node(q);
        const std::size_t left  = site.indices[kLeft].dim;
        const std::size_t right = site.indices[kRight].dim;
        const std::size_t span  = std::size_t{1} << q;
        if (left != bond) reject("to_dense found an inconsistent bond dimension");

        std::vector<Cx> next(span * 2 * right, Cx{0, 0});
        for (std::size_t idx = 0; idx < span; ++idx) {
            for (std::size_t s = 0; s < 2; ++s) {
                const std::size_t out_idx = idx + (s << q);
                for (std::size_t b = 0; b < right; ++b) {
                    Cx acc{0, 0};
                    for (std::size_t a = 0; a < left; ++a) {
                        acc += cur[idx * left + a] * site.data[site_offset(a, s, b, right)];
                    }
                    next[out_idx * right + b] = acc;
                }
            }
        }
        cur = std::move(next);
        bond = right;
    }

    std::vector<Cx> dense(std::size_t{1} << num_qubits_, Cx{0, 0});
    for (std::size_t i = 0; i < dense.size(); ++i) dense[i] = cur[i * bond];
    return dense;
}

}  // namespace detail
}  // namespace qplayer
