/// @file tn_mpo.hpp
/// @brief Density matrix as a Matrix Product Operator.
/// @ingroup backend
///
/// The other half of : instead of averaging over sampled trajectories,
/// carry the density matrix itself and apply each channel exactly. One MPO site
/// holds both the ket and the bra leg of one qubit as a single physical index of
/// dimension 4, so every quantum operation — unitary or channel — is the same
/// primitive, `rho -> sum_i K_i rho K_i^dagger`, applied to one or two sites.
///
/// What this buys and what it costs is settled in : strong noise can
/// bound the bond dimension in a way no Monte Carlo run can, but the physical
/// index of 4 makes every two-site SVD eight times the work of the MPS one at
/// the same chi, and truncation no longer preserves positivity — a failure mode
/// the MPS path structurally cannot have.

#pragma once

#include "backend/tensor_network/tn_backend.hpp"
#include "backend/full_state/pauli_expectation.hpp"
#include "noise/noise_model.hpp"

#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace qplayer {

// ── MPO truncation policy constants ────────────────────────
//
// Deliberately NOT the MPS numbers. A two-site MPO merge is a 4*chi x 4*chi
// problem, so chi_rho = 128 is the SVD-size twin of the MPS chi = 256 that
// costed; and the norm here is ||rho||_F = sqrt(Tr rho^2), which
// collapses towards 2^(-n/2) as the state mixes, so only a RELATIVE cutoff
// carries the same meaning at every n.

/// Default bond cap: the 4chi x 4chi SVD matches MPS chi=256's 512x512.
constexpr std::uint32_t kMpoDefaultMaxBond = 128;

/// Largest bond a user may request — the 8192x8192 SVD twin of 's 4096.
constexpr std::uint32_t kMpoMaxBondCeiling = 2048;

/// Relative cutoff on the discarded tail, normalised by ||rho||_F.
constexpr double kMpoDefaultCutoff = 1e-12;

/// Accumulated-error level at which the backend warns once.
constexpr double kMpoErrorBudgetWarn = 0.1;

/// Below this a computed probability is reported as negative rather than treated
/// as rounding dust. Truncation can push eigenvalues genuinely negative.
constexpr double kMpoNegativeProbabilityTolerance = 1e-9;

/// A site conditional below this is a broken distribution, not a perturbed one:
/// conditionals are normalised so p0 + p1 = 1, hence -0.5 means the sibling
/// branch exceeds 1.5. Measured regimes fall cleanly on either side — gentle
/// cutoff-driven truncation reached -0.49, cap saturation -1.95 to -6.99 — so
/// counts are refused above this magnitude rather than drawn from clamped
/// weights.
constexpr double kMpoNegativeProbabilityFatal = 0.5;

namespace detail {

/// Resolved truncation knobs for one MPO instance.
///
/// Separate from MpsTruncationConfig on purpose: the five axes that differ
/// (physical dim, memory, SVD size, norm, error meaning) mean one number cannot
/// carry both meanings.
/// @ingroup backend
struct MpoTruncationConfig {
    /// Bond cap on the density-matrix chain — the resource axis.
    std::uint32_t max_bond = kMpoDefaultMaxBond;

    /// Relative discarded-tail cutoff, normalised by ||rho||_F.
    double cutoff = kMpoDefaultCutoff;

    /// Accumulated-error level at which the state warns once.
    double error_budget_warn = kMpoErrorBudgetWarn;

    /// Reads QPLAYER_MPO_MAX_BOND / _CUTOFF / _ERROR_BUDGET over the defaults.
    /// Bad values are ignored with a log::warn rather than throwing (convention).
    static MpoTruncationConfig from_env();

    /// throws: CircuitError — max_bond is 0 or above kMpoMaxBondCeiling, or cutoff is negative.
    void validate() const;
};

/// A density matrix as an MPO chain: site q carries qubit q's (ket, bra) pair.
///
/// The physical index is `ket * 2 + bra`, so the site tensor is [chi, 4, chi].
/// Basis ordering matches the statevector backends — bit q of a basis index is
/// qubit q — which is what lets to_dense() be compared against FullState.
/// @ingroup backend
class MpoState {
public:
    /// Builds |0...0><0...0| with every bond at chi = 1.
    /// throws: SimulatorError — num_qubits is 0 or above kMaxTensorNetworkQubits.
    /// throws: CircuitError — the config is not honourable.
    explicit MpoState(QubitIndex num_qubits, MpoTruncationConfig config = {});

    /// rho -> U rho U^dagger on one site. Bonds are untouched: a one-site
    /// superoperator cannot create correlation, so there is no SVD here.
    /// throws: SimulatorError — site out of range, or matrix is not 4 entries.
    void apply_unitary_1q(const std::vector<std::complex<double>>& u, QubitIndex site);

    /// rho -> U rho U^dagger on two sites, matrix in OPERAND order (local bit 0 = `s0`).
    /// Distant pairs are walked together with SWAPs and walked back.
    /// throws: SimulatorError — site out of range, s0 == s1, or matrix is not 16 entries.
    void apply_unitary_2q(const std::vector<std::complex<double>>& u, QubitIndex s0, QubitIndex s1);

    /// rho -> sum_i K_i rho K_i^dagger on one site — the channel applied EXACTLY,
    /// with no sampling. Like the unitary case it leaves every bond alone, which
    /// is why single-qubit device noise is free on this path too.
    /// throws: SimulatorError — site out of range, or an operator is not 2x2.
    void apply_channel_1q(const std::vector<UnitaryMatrix>& kraus, QubitIndex site);

    /// rho -> sum_i K_i rho K_i^dagger on two sites, operators in OPERAND order.
    /// This is the only operation that can grow the bond, so it is the only one
    /// that truncates (one recorded step error per call).
    /// throws: SimulatorError — site out of range, s0 == s1, or an operator is not 4x4.
    void apply_channel_2q(const std::vector<UnitaryMatrix>& kraus, QubitIndex s0, QubitIndex s1);

    /// Tr(rho) — exactly 1 for an untruncated run, so it doubles as a health check.
    /// Costs O(n * chi^2): the physical legs are closed with a delta, not expanded.
    double trace() const;

    /// Rescales so Tr(rho) == 1. Channels are trace preserving, so only truncation
    /// makes this necessary — and after truncation it is the physically meaningful
    /// normalisation, not the Frobenius one the MPS path uses.
    /// throws: SimulatorError — the trace is non-finite or has collapsed to 0.
    void normalize_trace();

    /// Tr(rho P) for a Pauli string, in SITE order. O(n * chi^2), no 2^n anywhere.
    /// throws: SimulatorError — ops length does not match the chain.
    double expectation_value(const std::vector<PauliOp>& ops) const;

    /// Draws one basis state from the diagonal of rho, in SITE order.
    ///
    /// Same conditional sweep as the MPS sampler, but the environments close the
    /// physical legs with a trace instead of pairing ket and bra, so they are
    /// vectors rather than matrices. `negative_weight` reports the most negative
    /// conditional seen — truncation can make one, and hiding it would be exactly
    /// the silent approximation refuses.
    /// throws: SimulatorError — a conditional pair sums to zero or is non-finite.
    std::vector<int> sample(std::mt19937_64& rng, double* negative_weight = nullptr) const;

    /// Full density matrix, row-major 2^n x 2^n, for verification against FullState.
    /// throws: SimulatorError — num_qubits exceeds MAX_DENSE_EXPORT_QUBITS.
    std::vector<std::complex<double>> to_dense() const;

    /// @return Number of qubits (sites).
    QubitIndex num_qubits() const noexcept { return num_qubits_; }

    /// @return Largest bond currently in the chain — the quantity noise is supposed to bound.
    std::uint32_t max_bond_dimension() const noexcept { return graph_.max_bond_dimension(); }

    /// @return Per-truncation-step relative errors in application order.
    const std::vector<double>& truncation_errors() const noexcept { return truncation_errors_; }

    /// Accumulated sqrt(sum of squared step errors) — the budget quantity.
    double total_truncation_error() const noexcept;

    /// @return The resolved truncation configuration in force.
    const MpoTruncationConfig& config() const noexcept { return config_; }

    /// Read access to the chain, for tests that inspect bond structure directly.
    const TensorGraph& graph() const noexcept { return graph_; }

    /// Adjacent SWAPs spent walking distant operands together — diagnostics only.
    std::size_t swap_steps() const noexcept { return swap_steps_; }

private:
    /// Merge-apply-SVD-split on sites (site, site+1). Returns the step error.
    double apply_channel_2q_adjacent(const std::vector<UnitaryMatrix>& kraus,
                                     QubitIndex site, bool swap_operands);

    /// Warns once on the first lossy truncation and once on budget exhaustion.
    void note_truncation(double step_error, bool bond_capped);

    QubitIndex          num_qubits_ = 0;
    TensorGraph         graph_;
    MpoTruncationConfig config_{};
    std::vector<double> truncation_errors_;
    double              squared_error_sum_ = 0.0;
    bool                warned_lossy_      = false;
    bool                warned_budget_     = false;
    std::size_t         swap_steps_        = 0;
};

/// Builds |0...0><0...0| as an MPO chain of [1, 4, 1] sites.
/// throws: SimulatorError — num_qubits is 0 or exceeds kMaxTensorNetworkQubits.
/// @ingroup backend
TensorGraph mpo_zero_state(QubitIndex num_qubits);

/// True when the graph is a well-formed MPO chain: uniform [bond, physical(4), bond]
/// sites, physical ids 0..n-1 in order, dim-1 boundary bonds, n-1 neighbour bonds.
/// @ingroup backend
bool is_mpo_chain(const TensorGraph& graph) noexcept;

}  // namespace detail
}  // namespace qplayer
