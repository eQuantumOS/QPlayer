/// @file tn_noise.hpp
/// @brief Kraus operator application on an MPS chain.
/// @ingroup backend
///
/// These primitives apply ONE Kraus operator. Choosing which operator — the
/// `p_i = ||K_i psi||^2` draw that the FullState routine performs inline — is
/// deliberately left to the trajectory loop, because reading that norm
/// off an MPS requires a gauge decision (canonical form versus paying
/// squared_norm() per channel) that defers to measurement.
///
/// A Kraus operator is not unitary in general, and here that is load-bearing
/// rather than merely tolerated: ||K psi|| IS the branch probability, so the
/// norm change must survive the application. It does on both paths — the 1Q path
/// only touches a physical leg, and the 2Q path's post-SVD rescale restores the
/// PRE-truncation norm rather than 1 (mps.cpp), so the Kraus reduction is not
/// silently normalised away.
///
/// Cost asymmetry is the reason the two arities are separate entry points:
/// a 1-qubit operator cannot create entanglement, so it leaves every bond
/// untouched and runs no SVD, while a 2-qubit operator pays a full
/// merge/SVD/split cycle. Most device noise (T1/T2 relaxation, dephasing,
/// 1Q depolarizing) is 1-qubit and therefore free of truncation on an MPS
/// (context).

#pragma once

#include "backend/tensor_network/mps.hpp"
#include "noise/noise_model.hpp"

#include <complex>
#include <cstddef>
#include <random>
#include <vector>

namespace qplayer {
namespace detail {

/// Applies a 2x2 Kraus operator to one site's physical leg.
///
/// Bond dimensions are unchanged, no SVD runs, and nothing is appended to
/// MpsState::truncation_errors() — a single-site operator cannot create
/// entanglement, so there is no tail to discard.
/// throws: SimulatorError — operator is not 2x2, or qubit is out of range.
void apply_kraus_1q(MpsState& state, const UnitaryMatrix& op, QubitIndex qubit);

/// Applies a 4x4 Kraus operator to a qubit pair, reusing the two-qubit gate path
/// (merge -> operator -> zgesvd_ -> truncate -> split).
///
/// `op` is in OPERAND order, local bit 0 = `q0` — the same convention as
/// KrausChannel::target_qubit / target_qubit2 and the dense 2Q kernels
/// (noise_model.hpp). Because it runs the gate path it appends one entry to
/// MpsState::truncation_errors() per call, exactly as a 2Q gate does: a
/// channel-induced truncation costs the same accuracy budget as a gate-induced
/// one, so hiding it would make 's error budget under-report.
/// throws: SimulatorError — operator is not 4x4, q0 == q1, or a qubit is out of range.
void apply_kraus_2q(MpsState& state, const UnitaryMatrix& op, QubitIndex q0, QubitIndex q1);

/// Applies operator `op_index` of `channel`, dispatching on its dimension
/// (2 -> apply_kraus_1q, 4 -> apply_kraus_2q) and taking the target qubits from
/// the channel. Mirrors the FullState dispatcher so that a channel means the
/// same thing on both backends.
///
/// Wider operators are rejected rather than decomposed: the MPS path implements
/// 1- and 2-site application only, and FullState likewise dispatches 2x2/4x4
/// only, so a 3-qubit channel has no kernel on either side.
/// throws: SimulatorError — empty operator list, op_index out of range, a ragged
///   or non-square operator, or an operator wider than 4x4.
void apply_kraus_operator(MpsState& state, const KrausChannel& channel, std::size_t op_index);

/// Unnormalised 2x2 reduced density matrix of one site, row-major rho[s * 2 + s'].
///
/// Its trace is <psi|psi>, not 1: truncation leaves the chain slightly off norm
/// so every caller divides by the trace rather than assuming it.
/// Exact for any gauge — the chain is contracted against its own conjugate, so
/// no canonical form is presumed.
///
/// Two sweeps of shape (env x A) then (x A*), which is O(n * chi^3) where the
/// general RDM route would be O(n * chi^4). At 40 sites and chi = 32 that
/// difference is ~250 ms per call, and a device circuit asks for one per gate.
/// `site` is a SITE index.
///
/// Hot paths call MpsState::site_marginal() instead, which returns the same
/// value from cached environments. This uncached form rebuilds both
/// environments from the chain ends and is kept as the reference the cache is
/// tested against.
/// throws: SimulatorError — site out of range, or a bond disagrees between
///   neighbouring sites.
/// @ingroup backend
std::vector<std::complex<double>> site_marginal(const TensorGraph& chain, std::size_t site);

/// Branch probabilities p_i = ||K_i psi||^2 for every operator of `channel`,
/// unnormalised — their sum is <psi|psi> for a trace-preserving channel, and the
/// caller divides by it rather than assuming 1.
///
/// The target fields are SITE indices, not qubit indices: the backend owns the
/// qubit->site mapping and hands site space in, exactly as it does for
/// mps_amplitude() and mps_reduced_density_matrix().
///
/// **How, and why this way.** p_i = Tr(K_i rho K_i^dagger)
/// where rho is the reduced density matrix of the channel's site(s). So ONE
/// mps_reduced_density_matrix() sweep — O(n * chi^3), exact for any gauge because
/// it contracts the chain against its own conjugate — serves EVERY operator of
/// the channel, after which each p_i is 2x2 or 4x4 arithmetic. The obvious
/// alternative (copy the chain, apply K_i, read squared_norm()) pays that sweep
/// once PER OPERATOR plus a chain copy each time, which is 2x for a damping
/// channel and 4x for depolarizing. The third option, maintaining a canonical
/// form so the norm is readable locally, is asymptotically better still but
/// changes MpsState's invariants, so leaves it to a later measurement.
/// throws: SimulatorError — empty operator list, ragged or non-square operator,
///   an operator wider than 4x4, or a site out of range.
std::vector<double> kraus_branch_probabilities(const MpsState& state,
                                               const KrausChannel& channel);

/// Samples one branch of `channel` and applies it, rescaled so the state keeps
/// the norm it had before the channel. Returns the index of the chosen operator.
///
/// Renormalising is folded into the applied matrix (K_k * sqrt(total / p_k))
/// rather than done as a separate pass over the chain: it costs nothing extra and
/// keeps MpsState free of a public scaling entry point that only this caller
/// would use.
///
/// Selection follows the FullState routine — cumulative probability against one
/// uniform draw, falling back to the last operator on floating-point rounding —
/// so the same channel, state and seed pick the same branch on both backends.
/// throws: SimulatorError — the drawn branch has zero weight (numerical
/// divergence), or any error from kraus_branch_probabilities().
std::size_t apply_kraus_channel(MpsState& state, const KrausChannel& channel,
                                std::mt19937_64& rng);

/// Same as apply_kraus_channel() with the uniform already drawn by the caller
/// the noise-site hook needs the draw without copying the RNG.
/// apply_kraus_channel() draws once and calls this, so its stream is unchanged.
/// @param u01 Uniform in [0, 1); scaled by the branch-weight total inside.
/// @return The chosen branch.
std::size_t apply_kraus_channel_u(MpsState& state, const KrausChannel& channel,
                                  double u01);

}  // namespace detail
}  // namespace qplayer
