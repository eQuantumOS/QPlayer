// t1t2_calculator.cpp — T1/T2 -> relaxation Kraus implementation
// See header for the formulas and clamping policy.

#include "noise/t1t2_calculator.hpp"

#include <cmath>
#include <utility>

#include "noise/detail/kraus_builders.hpp"

namespace qplayer {

double T1T2Calculator::amplitude_damping_gamma(double t1, double t_gate) {
    // Infinite or non-positive T1 -> no relaxation. exp(-t/T1) is in (0, 1] for
    // finite positive T1 and t_gate >= 0, so gamma lands in [0, 1).
    if (!std::isfinite(t1) || t1 <= 0.0) return 0.0;
    return 1.0 - std::exp(-t_gate / t1);
}

double T1T2Calculator::dephasing_lambda(double t1, double t2, double t_gate) {
    // 1/T_phi = 1/T2 - 1/(2 T1). Infinite T1/T2 contribute 0 to their term.
    const double inv_t2  = (std::isfinite(t2) && t2 > 0.0) ? 1.0 / t2          : 0.0;
    const double inv_2t1 = (std::isfinite(t1) && t1 > 0.0) ? 1.0 / (2.0 * t1)  : 0.0;
    const double inv_tphi = inv_t2 - inv_2t1;
    // Non-positive rate (T2 >= 2 T1, the physical limit) -> no pure dephasing.
    if (inv_tphi <= 0.0) return 0.0;
    // lambda = 1 - exp(-2 t / T_phi): phase_damping_ops(lambda) shrinks
    // coherence by sqrt(1-lambda) = exp(-t/T_phi), so together with amplitude
    // damping's exp(-t/2T1) the total coherence decays as exp(-t/T2). The factor 2
    // compensates the square root; a bare 1-exp(-t/T_phi) halves the dephasing rate.
    return 1.0 - std::exp(-2.0 * t_gate * inv_tphi);
}

std::vector<KrausChannel> T1T2Calculator::compute(double t1, double t2,
                                                  double t_gate, QubitIndex q) {
    std::vector<KrausChannel> out;

    const double gamma = amplitude_damping_gamma(t1, t_gate);
    if (gamma > 0.0) {
        KrausChannel ch;
        ch.operators    = detail::amplitude_damping_ops(gamma);
        ch.type         = ChannelType::AmplitudeDamping;
        ch.target_qubit = q;
        out.push_back(std::move(ch));
    }

    const double lambda = dephasing_lambda(t1, t2, t_gate);
    if (lambda > 0.0) {
        KrausChannel ch;
        ch.operators    = detail::phase_damping_ops(lambda);
        ch.type         = ChannelType::PhaseDamping;
        ch.target_qubit = q;
        out.push_back(std::move(ch));
    }

    return out;
}

}  // namespace qplayer
