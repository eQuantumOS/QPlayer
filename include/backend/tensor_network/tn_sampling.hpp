/// @file tn_sampling.hpp
/// @brief Perfect sampling from an MPS chain.
/// @ingroup backend
///
/// Drawing a shot from an MPS does not need the statevector. The chain factorises
/// the joint distribution into conditionals, so one left-to-right sweep picks
/// site 0's bit from its marginal, then site 1's from its marginal *given* that
/// bit, and so on — an exact draw from |psi|^2 with no rejection and no 2^n array
/// anywhere. That is what lifts counts() past MAX_DENSE_EXPORT_QUBITS, which is
/// the only reason a 100-qubit measurement distribution is reachable at all.
///
/// The conditionals need the *other* side of the chain, so the right
/// environments are built once (O(n * chi^3)) and every subsequent shot costs
/// O(n * chi^2). Amortising that build across shots is what makes the sampler an
/// object rather than a function.

#pragma once

#include "backend/tensor_network/tn_backend.hpp"

#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace qplayer {
namespace detail {

/// Exact sampler over the basis states of an MPS chain.
///
/// Build once per state, then draw as many shots as needed. The chain must
/// outlive the sampler and must not be mutated in between: the environments are
/// a snapshot, and a gate applied behind the sampler's back would leave it
/// sampling a state that no longer exists.
/// @ingroup backend
class MpsSampler {
public:
    /// Builds the right environments for every site — the O(n * chi^3) half.
    /// throws: SimulatorError — the graph is not a well-formed MPS chain, or a
    ///   bond dimension disagrees between neighbouring sites.
    explicit MpsSampler(const TensorGraph& chain);

    /// Draws one basis state, in SITE order: `bits[i]` is site i's physical value.
    ///
    /// Site order, not qubit order, because the sampler sees only the chain — the
    /// site layout permutation belongs to the backend that chose it.
    /// Consumes exactly one uniform draw per site, so a fixed seed reproduces a
    /// shot exactly.
    /// throws: SimulatorError — a conditional norm is non-finite or non-positive,
    ///   which means the chain diverged rather than that the draw was unlucky.
    std::vector<int> sample(std::mt19937_64& rng) const;

    /// Same draw, writing into a caller-owned buffer so a shot loop allocates once.
    /// throws: SimulatorError — see sample().
    void sample_into(std::mt19937_64& rng, std::vector<int>& bits) const;

    /// <psi|psi>, read off the fully contracted right environment.
    ///
    /// Free here, and O(n * chi^3) against MpsState::squared_norm()'s O(n * chi^4):
    /// the environment sweep already computed exactly this number, so a caller
    /// that samples should take the norm from here rather than sweeping again.
    double squared_norm() const noexcept { return squared_norm_; }

    /// Bytes held by the environments — n matrices of chi^2 complex entries.
    std::uint64_t environment_bytes() const noexcept;

private:
    /// Right environment covering sites [site, n), indexed [ket * dim + bra].
    /// right_[n] is the 1x1 boundary scalar, so right_[i + 1] is always readable.
    std::vector<std::vector<std::complex<double>>> right_;
    const TensorGraph*                             chain_ = nullptr;
    std::size_t                                    n_     = 0;
    double                                         squared_norm_ = 0.0;
};

/// Worst-case sampler footprint, for the memory budget that sizes trajectory
/// threads (shape, MPS quantities). One chi x chi complex matrix
/// site — half a chain tensor, since an environment has no physical leg.
/// @ingroup backend
std::uint64_t mps_sampling_environment_bytes(QubitIndex num_qubits, std::uint32_t max_bond);

}  // namespace detail
}  // namespace qplayer
