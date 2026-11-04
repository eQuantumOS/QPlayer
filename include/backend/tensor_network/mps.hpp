/// @file mps.hpp
/// @brief Matrix Product State storage with SVD truncation.
/// @ingroup backend
///
/// MPS chain built on the TensorGraph types of tn_backend.hpp. One-qubit gates
/// act on a single site; two-qubit gates merge two neighbouring sites, apply the
/// 4x4 unitary, and split them again through an SVD whose small singular values
/// are discarded under the policy. Non-adjacent pairs are brought
/// together with SWAPs, which is what keeps the chain a chain.

#pragma once

#include "backend/tensor_network/tn_backend.hpp"
#include "backend/full_state/pauli_expectation.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace qplayer {
namespace detail {

/// Resolved truncation knobs for one MPS instance.
///
/// Three sources in increasing precedence: the constexpr defaults, the
/// QPLAYER_MPS_* environment variables, then set_mps_truncation(). Keeping them
/// in one value type means the SVD path reads a single resolved struct rather
/// than re-deciding precedence per call.
/// @ingroup backend
struct MpsTruncationConfig {
    /// Bond dimension cap — the resource axis.
    std::uint32_t max_bond = kMpsDefaultMaxBond;

    /// Relative amplitude-scale cutoff on the discarded tail — the accuracy axis.
    double cutoff = kMpsDefaultCutoff;

    /// Accumulated-error level at which the backend warns once.
    double error_budget_warn = kMpsErrorBudgetWarn;

    /// Offload the two-site SVD to cuSolver. Set from an explicit
    /// ProcType::GPU request; ProcType::Auto leaves it false because the
    /// evidence behind the offload is an SVD microbenchmark, not circuit-level
    /// measurement, and because it would change existing answers silently.
    bool gpu_svd = false;

    /// Gauge policy of the two-site split. With the gauge held each SVD
    /// truncation is globally optimal and the per-gate rescale is the global one;
    /// without it a lossy run can drift the norm and keep redundant rank until it
    /// returns a wrong answer that the divergence guard lets through.
    MpsGauge gauge = kMpsDefaultGauge;

    /// Reads QPLAYER_MPS_MAX_BOND / _CUTOFF / _ERROR_BUDGET / _GAUGE over the defaults.
    /// Unparseable or out-of-range values are ignored with a log::warn rather
    /// than throwing: an env typo should not kill a long run.
    static MpsTruncationConfig from_env();

    /// Rejects settings that cannot be honoured, so a bad value fails at the
    /// call that set it instead of surfacing as a strange result much later.
    /// throws: CircuitError — max_bond is 0 or above kMpsMaxBondCeiling, or cutoff is negative.
    void validate() const;
};

/// A quantum state as an MPS chain: site q carries physical qubit q.
///
/// Amplitude ordering matches the statevector backends — basis index bit q is
/// qubit q — so a contracted MPS can be compared against FullState directly.
/// @ingroup backend
class MpsState {
public:
    /// Builds |0...0> with every bond at chi = 1.
    /// throws: SimulatorError — num_qubits is 0 or above kMaxTensorNetworkQubits.
    /// throws: CircuitError — the config is not honourable (see MpsTruncationConfig::validate).
    explicit MpsState(QubitIndex num_qubits, MpsTruncationConfig config = {});

    /// Applies a 2x2 unitary to one site. No SVD and no truncation: a one-qubit
    /// gate cannot create entanglement, so the bond dimensions are untouched.
    /// throws: SimulatorError — qubit out of range, or matrix is not 4 entries.
    void apply_1q(const std::vector<std::complex<double>>& u, QubitIndex qubit);

    /// Applies a 4x4 unitary given in OPERAND order (local bit 0 = `q0`).
    /// Adjacent sites merge-apply-SVD-split in place; distant pairs are walked
    /// together with SWAPs and walked back, so the caller never sees the chain
    /// reordered. Records one truncation error per call.
    /// throws: SimulatorError — qubit out of range, q0 == q1, or matrix is not 16 entries.
    void apply_2q(const std::vector<std::complex<double>>& u, QubitIndex q0, QubitIndex q1);

    /// Applies a three-qubit gate by running its FIXED decomposition through the
    /// 1Q/2Q paths. Operands follow the IR convention: CCX has
    /// `q0`,`q1` as controls and `q2` as target; CSwap has `q0` as control and
    /// swaps `q1` with `q2`.
    ///
    /// Takes a GateType rather than a matrix -- the asymmetry with apply_1q /
    /// apply_2q is deliberate. A fixed decomposition is selected by gate
    /// IDENTITY, not by matrix, and QPlayer's IR has exactly two 3Q gates
    /// so this is a two-row table and not gate synthesis. Reset
    /// carries the same asymmetry for the same kind of reason.
    ///
    /// Records ONE truncation error for the whole decomposition, so the ledger
    /// keeps meaning one entry per user gate. This is why the
    /// decomposition lives here and not in tn_fusion.cpp: folding needs the
    /// private apply_2q_walked() / note_truncation() pair.
    /// throws: SimulatorError — qubit out of range, operands not distinct, or
    ///   `gate` is not GateType::CCX / GateType::CSwap.
    void apply_3q(GateType gate, QubitIndex q0, QubitIndex q1, QubitIndex q2);

    /// @return Number of qubits (sites) in the chain.
    QubitIndex num_qubits() const noexcept { return num_qubits_; }

    /// @return Largest bond dimension currently in the chain — how entangled the state has become.
    ///
    /// This is a snapshot, NOT the peak: a bond shrinks when a later split needs
    /// fewer singular values, so reading this at the end of a run understates
    /// what the run had to hold. Use peak_bond_dimension() for the resource
    /// question.
    std::uint32_t max_bond_dimension() const noexcept { return graph_.max_bond_dimension(); }

    /// @return Largest bond dimension reached at ANY point during this state's
    /// life — the resource the run actually required.
    ///
    /// Measured 2x the end-of-run value on rotated surface code d=3/5/7 (8/32/64
    /// vs 4/16/32): apply_2q()'s SWAP round trip and syndrome extraction both
    /// entangle sites transiently and then undo it.
    std::uint32_t peak_bond_dimension() const noexcept { return peak_bond_; }

    /// @return Per-2Q-gate truncation errors in application order.
    const std::vector<double>& truncation_errors() const noexcept { return truncation_errors_; }

    /// Accumulated error sqrt(sum of squared step errors) — the budget quantity.
    double total_truncation_error() const noexcept;

    /// Accumulated error as the plain sum of step errors (L1).
    /// Distinct from total_truncation_error(): the L2 form is what the
    /// budget warns on, while the distance between the truncated state and the
    /// exact one accumulates like this sum. The two can order trajectories
    /// differently, so neither derives from the other.
    double total_truncation_error_l1() const noexcept { return error_sum_; }

    /// Contracts the whole chain into a dense statevector, for verification and
    /// for get_statevector(). Cost is O(2^n), which is
    /// why the cap exists — it is a debugging and export path, not the point of MPS.
    /// throws: SimulatorError — num_qubits exceeds MAX_DENSE_EXPORT_QUBITS.
    std::vector<std::complex<double>> to_dense() const;

    /// Squared norm of the state, read as the trace of a cached one-site marginal
    /// -- O(chi^3) steps, only as many as the last mutation left stale.
    /// On a chain that is not canonical the per-gate
    /// rescale is LOCAL, so a lossy truncation moves this away from 1 — measured
    /// up to 1.69 at chi_max 16. It is therefore an accuracy signal,
    /// not only a divergence check.
    double squared_norm() const;

    /// Scales the whole chain by `factor`, folded into one site tensor (O(chi^2)).
    /// The caller restores |psi|^2 = 1 with 1/sqrt(squared_norm()) once per run,
    /// which is what makes "the stored state is normalised" true again for the
    /// observables that read it as stored.
    /// throws: SimulatorError — factor is not finite.
    void rescale(double factor);

    /// Brings the chain to mixed-canonical form with the orthogonality centre at
    /// `centre`: sites left of it become left-orthonormal, sites right of it
    /// right-orthonormal, and the centre carries the whole norm.
    ///
    /// This does NOT change the state -- `to_dense()` is invariant across the
    /// call, which is the property the tests assert. What changes is the GAUGE,
    /// and three things follow from it:
    ///   - a site marginal at the centre is local (O(chi^2)) instead of a
    ///     full-chain sweep (O(n*chi^3)),
    ///   - `squared_norm()` is the centre tensor's own norm,
    ///   - an SVD truncation of a block at the centre is GLOBALLY optimal, which
    ///     is what makes the reported step error bound the state error rather
    /// than only the local block error (measures the block).
    ///
    /// Bond dimensions may SHRINK: the sweep drops rank the chain was carrying
    /// redundantly. Nothing is truncated beyond that -- no cutoff is applied and
    /// no truncation error is recorded, because no weight is discarded.
    /// throws: SimulatorError — centre is out of range, or a factorisation fails.
    void canonicalize(QubitIndex centre);

    /// Site the orthogonality centre sits at, or `num_qubits()` when the chain
    /// has not been canonicalised (or a later gate invalidated the gauge).
    /// @return the centre site, or num_qubits() for "no gauge held".
    QubitIndex orthogonality_centre() const noexcept { return centre_; }

    /// Unnormalised 2x2 reduced density matrix of `site`, row-major rho[s * 2 + s'],
    /// equal to detail::site_marginal(graph(), site) but served from cached
    /// environments.
    ///
    /// The free function rebuilds both environments from the chain ends on every
    /// call, O(n * chi^3); a noisy trajectory asks once per 1Q channel, so that
    /// sweep dominated the run and its many small GEMMs serialised trajectory
    /// threads on OpenBLAS's global buffer lock. Here only the environments a
    /// mutation actually invalidated are recomputed, with the same step kernels,
    /// so a query after a left-to-right run of gates costs O(1) steps amortised.
    ///
    /// Logically const: the cache is an implementation detail of the query.
    /// NOT safe to call concurrently on one instance — every trajectory owns its
    /// MpsState, and the backend's shared chain is only queried single-threaded.
    /// Defined in tn_noise.cpp, next to the step kernels it shares with the free
    /// function.
    /// throws: SimulatorError — site out of range, or a bond disagrees between
    ///   neighbouring sites.
    std::vector<std::complex<double>> site_marginal(QubitIndex site) const;

    /// Unnormalised 4x4 reduced density matrix of sites lo < hi, row-major, bit 0
    /// of each index being site `lo` -- the layout mps_reduced_density_matrix()
    /// returns, so the two are interchangeable for a 2Q channel.
    /// Uses the same cached environments as site_marginal(); the sites between
    /// the operands cost four plain left steps each, O(chi^3), where the general
    /// RDM sweep paid O(chi^4) at every site of the chain. Same concurrency rule.
    /// throws: SimulatorError — hi out of range or lo >= hi.
    std::vector<std::complex<double>> two_site_marginal(QubitIndex lo, QubitIndex hi) const;

    /// @return The resolved truncation configuration in force.
    const MpsTruncationConfig& config() const noexcept { return config_; }

    /// Read access to the chain, for the query routines in tn_amplitude.cpp and
    /// tn_rdm.cpp — they read site tensors but never mutate the state.
    const TensorGraph& graph() const noexcept { return graph_; }

    /// Adjacent SWAPs spent walking distant operands together. Pure diagnostics,
    /// but it is the only observable that shows whether a site layout actually
    /// helped — a good chain order drives this to zero.
    std::size_t swap_steps() const noexcept { return swap_steps_; }

private:
    /// SWAP-walks `q0`,`q1` together, applies `u`, walks them home. RETURNS the
    /// accumulated step error instead of recording it, which is what lets one
    /// user gate that expands into several 2Q steps leave a single ledger entry
    /// (apply_2q for the walk, apply_3q for a decomposition --).
    double apply_2q_walked(const std::vector<std::complex<double>>& u,
                           QubitIndex q0, QubitIndex q1);

    /// Merge-apply-SVD-split on sites (site, site+1). Returns the step error.
    /// With the canonical gauge the centre is first brought into the
    /// block, and `centre_left` decides which site keeps it after the split: a
    /// leftward sequence of steps passes true so the next block already holds it.
    double apply_2q_adjacent(const std::vector<std::complex<double>>& u,
                             QubitIndex site, bool swap_operands, bool centre_left = false);

    /// True when splits must keep the gauge: always under Canonical, after the
    /// first lossy split under Auto.
    bool gauge_active() const noexcept {
        return config_.gauge == MpsGauge::Canonical ||
               (config_.gauge == MpsGauge::Auto && gauge_armed_);
    }

    /// One step of the centre moving left / right. Each factorises one
    /// site and folds the carry into its neighbour; the state is unchanged.
    void make_right_orthonormal(std::size_t k);
    void make_left_orthonormal(std::size_t k);

    /// Brings the centre to `target`, one site per factorisation. Falls back to a
    /// full sweep when no gauge is held.
    void move_centre(std::size_t target);

    /// Warns once when a truncation actually loses weight above `cutoff`, and
    /// once when the accumulated budget is first exceeded.
    void note_truncation(double step_error, bool bond_capped);

    /// Marks the cached environments that depend on sites [lo, hi] as stale
    /// Every site-tensor write must call this; a missed call returns
    /// a marginal of an older state, which is a wrong answer rather than a slow one.
    void invalidate_environments(QubitIndex lo, QubitIndex hi) noexcept {
        if (lo < env_left_valid_)  env_left_valid_  = lo;
        if (hi > env_right_valid_) env_right_valid_ = hi;
    }

    QubitIndex          num_qubits_ = 0;
    TensorGraph         graph_;
    MpsTruncationConfig config_{};
    std::vector<double> truncation_errors_;
    std::uint32_t       peak_bond_         = 1;   // updated in apply_2q_adjacent
    double              squared_error_sum_ = 0.0;
    double              error_sum_         = 0.0;   // L1 companion
    bool                warned_lossy_      = false;
    bool                warned_budget_     = false;
    std::size_t         swap_steps_        = 0;

    /// Set at the first lossy split under MpsGauge::Auto; from then
    /// on the gauge is maintained exactly as under MpsGauge::Canonical.
    bool                gauge_armed_       = false;

    /// Orthogonality centre, or num_qubits_ for "no gauge held". Every mutating
    /// path must either maintain this or drop it to num_qubits_ -- a stale
    /// centre is worse than none, because callers would trust a gauge that is
    /// no longer there.
    QubitIndex          centre_            = 0;

    // Environment cache for site_marginal(). env_left_[k] contracts
    // sites [0, k) at site k's left bond; env_right_[k] contracts sites (k, n) at
    // its right bond. env_left_[0..env_left_valid_] and
    // env_right_[env_right_valid_..n-1] are current; the rest are stale.
    mutable std::vector<std::vector<std::complex<double>>> env_left_;
    mutable std::vector<std::vector<std::complex<double>>> env_right_;
    mutable QubitIndex  env_left_valid_    = 0;
    mutable QubitIndex  env_right_valid_   = 0;
};

/// Single amplitude <x|psi> of an MPS chain, without building the statevector.
///
/// Sweeps the chain contracting each site against the fixed physical value, so
/// the cost is O(n * chi^2) and there is no 2^n anywhere — that is the whole
/// point of the amplitude mode. `bits` is in SITE order: bits[i] is the value of
/// the physical leg on site i.
/// throws: SimulatorError — bits length does not match the chain length.
/// @ingroup backend
/// Is site `k` left-orthonormal, i.e. does the (left*2) x right reshape satisfy
/// M^H M = I? Verification only -- the canonical sweep is trusted by its effect
/// on to_dense(), and this checks the mechanism rather than the outcome.
bool mps_is_left_orthonormal(const TensorGraph& chain, std::size_t site, double tol = 1e-10);

/// Is site `k` right-orthonormal, i.e. does the left x (2*right) reshape satisfy
/// M M^H = I? Counterpart of mps_is_left_orthonormal.
bool mps_is_right_orthonormal(const TensorGraph& chain, std::size_t site, double tol = 1e-10);

std::complex<double> mps_amplitude(const TensorGraph& chain, const std::vector<int>& bits);

/// <psi|P|psi> for a Pauli string P, without building the statevector.
///
/// P factorises over sites, so one left-environment sweep carries the pair of
/// bonds (ket, bra) and folds in each site's 2x2 factor: the cost is
/// O(n * chi^3), the same shape as squared_norm(), and no 2^n array is ever
/// allocated. `ops` is in SITE order: ops[i] is
/// the factor on site i.
///
/// P is Hermitian, so only the real part is meaningful and the imaginary part is
/// numerical noise. The state is used as stored: truncation renormalises after
/// every SVD, so no extra division by the norm is applied here — the
/// same convention as the FullState backends.
/// throws: SimulatorError — ops length does not match the chain length, or the
///   chain is malformed.
/// @ingroup backend
double mps_expectation_value(const TensorGraph& chain, const std::vector<PauliOp>& ops);

/// Reduced density matrix of a subset of sites, traced over the rest.
///
/// Contracts the chain against its own conjugate, leaving the requested sites'
/// physical legs open on both the ket and the bra side. Returned row-major as a
/// 2^k x 2^k matrix whose bit i (LSB first) is `sites[i]` after sorting — the
/// subset has no meaningful "qubit q at bit q" convention, so the order is
/// stated rather than assumed.
/// throws: SimulatorError — a site is out of range, repeated, or the subset
///   exceeds kMaxRdmSites (4^k entries grow past any budget quickly).
/// @ingroup backend
std::vector<std::complex<double>> mps_reduced_density_matrix(const TensorGraph& chain,
                                                             std::vector<QubitIndex> sites);

}  // namespace detail
}  // namespace qplayer
