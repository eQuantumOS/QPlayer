/// @file tn_measure.cpp
/// @brief Mid-circuit measurement collapse and Reset on an MPS chain.
///
/// A measurement is the Kraus channel {|0><0|, |1><1|}, so this reuses exactly
/// the machinery built for channels: one site_marginal() sweep gives the
/// outcome weights, and the chosen projector goes on through the ordinary
/// one-site gate path. Nothing here is new physics — what is new is only that
/// the collapse now happens WHERE the measurement is, instead of being deferred
/// to the end of the circuit where a following gate would never see it.

#include "backend/tensor_network/tn_measure.hpp"

#include "backend/tensor_network/tn_noise.hpp"
#include "error.hpp"

#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace qplayer {
namespace detail {

namespace {

using Cx = std::complex<double>;

constexpr const char* kBackend = "TensorNetwork";

}  // namespace

int mps_measure_site(MpsState& state, std::size_t site, std::mt19937_64& rng) {
    // Diagonal of the site's marginal. Its trace is <psi|psi> rather than 1
    // which is why the draw below divides by p0 + p1 instead
    // treating p1 as a probability outright.
    const std::vector<Cx> rho = state.site_marginal(static_cast<QubitIndex>(site));
    // Rounding can push a vanishing branch a few ulp negative, and a negative
    // weight would flip the comparison. Clamp; real divergence is caught by the
    // finiteness test on the total.
    const double p0 = rho[0].real() > 0.0 ? rho[0].real() : 0.0;
    const double p1 = rho[3].real() > 0.0 ? rho[3].real() : 0.0;
    const double total = p0 + p1;
    if (!std::isfinite(total) || total <= 0.0) {
        throw SimulatorError{kBackend,
            "measurement hit a degenerate site at " + std::to_string(site) +
            " (p0 + p1 = " + std::to_string(total) + ")"};
    }

    // Exactly one uniform per measurement, so a fixed seed reproduces the shot
    // and the draw does not depend on how the weights were computed.
    //
    // The comparison is `u < p1`, matching apply_measure_rng() rather than the
    // `u * total < p0` of MpsSampler. The two are equally correct as
    // samplers, but this routine is the analogue of the statevector backends'
    // measurement, and using their form makes a noiseless per-shot run
    // bit-identical across backends at a fixed seed instead of merely equal in
    // distribution -- the sampler's counterpart is Result's whole-bitstring draw,
    // not this. Dividing by `total` is the one difference the chain forces:
    // truncation leaves <psi|psi> off 1, where a statevector is
    // normalised by construction.
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    const int outcome = (u01(rng) < p1 / total) ? 1 : 0;

    // |b><b| / sqrt(p_b): projecting alone would scale the chain by sqrt(p_b),
    // so folding the inverse into the same 2x2 leaves <psi|psi> where it was.
    // Dividing by p_b (not by total) is what keeps that true — the state carried
    // norm `total`, and the surviving branch carries `p_b` of it.
    const double picked = (outcome == 0) ? p0 : p1;
    if (!std::isfinite(picked) || picked <= 0.0) {
        throw SimulatorError{kBackend,
            "measurement drew a zero-weight outcome at site " + std::to_string(site) +
            "; the state has diverged numerically"};
    }
    const double scale = std::sqrt(total / picked);

    std::vector<Cx> projector(4, Cx{0.0, 0.0});
    projector[static_cast<std::size_t>(outcome) * 2 + static_cast<std::size_t>(outcome)] =
        Cx{scale, 0.0};
    // One site, so no SVD and no bond growth: a single-site operator cannot
    // create entanglement, and nothing is appended to the truncation ledger.
    state.apply_1q(projector, static_cast<QubitIndex>(site));

    return outcome;
}

void mps_reset_site(MpsState& state, std::size_t site, std::mt19937_64& rng) {
    // Reset is not a gate with a matrix; it is the measurement above plus a
    // conditional X, which is exactly how the statevector backends define it
    // (full_state_cpu.cpp apply_reset_rng). Routing it through a "unitary" is
    // what produced a [FullState]-tagged error from a TensorNetwork run.
    if (mps_measure_site(state, site, rng) == 1) {
        static const std::vector<Cx> kX = {Cx{0.0, 0.0}, Cx{1.0, 0.0},
                                           Cx{1.0, 0.0}, Cx{0.0, 0.0}};
        state.apply_1q(kX, static_cast<QubitIndex>(site));
    }
}

}  // namespace detail
}  // namespace qplayer
