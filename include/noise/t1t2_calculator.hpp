// t1t2_calculator.hpp — T1/T2 coherence -> relaxation Kraus conversion
// Used by
// NoiseModel::load_device (gate relaxation) and the IdleNoiseInserter /
// backend idle path wherever a duration is known:
//   gamma  = 1 - exp(-t / T1)                Amplitude Damping
//   T_phi  = 1 / (1/T2 - 1/(2 T1))           pure dephasing time
// lambda = 1 - exp(-2 t / T_phi) Dephasing (: the factor 2
//            compensates phase_damping_ops' sqrt(1-lambda) coherence factor so
//            the total coherence decays as exp(-t/T2), not exp(-t/2 T_phi extra)).
// All times share units (ns). Infinite T1/T2 (unknown qubits) and the
// unphysical T2 > 2 T1 case clamp to a zero rate (no-op) rather than throwing,
// so callers can pass device data through unvalidated.

#pragma once

#include <vector>

#include "noise/noise_model.hpp"  // KrausChannel, ChannelType
#include "types.hpp"

namespace qplayer {

struct T1T2Calculator {
    // gamma = 1 - exp(-t/T1); 0 when T1 is infinite/non-positive (no damping).
    static double amplitude_damping_gamma(double t1, double t_gate);

    // lambda = 1 - exp(-2 t/T_phi); 0 when the pure-dephasing rate
    // 1/T2 - 1/(2 T1) is non-positive (T2 >= 2 T1, including infinite T2).
    static double dephasing_lambda(double t1, double t2, double t_gate);

    // {AmplitudeDamping(gamma), Dephasing(lambda)} on qubit q. Zero-rate
    // (identity) channels are omitted, so an all-infinite (T1, T2) returns {}.
    static std::vector<KrausChannel> compute(double t1, double t2,
                                             double t_gate, QubitIndex q);
};

}  // namespace qplayer
