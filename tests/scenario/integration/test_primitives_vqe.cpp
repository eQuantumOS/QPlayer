// test_primitives_vqe.cpp — Estimator on a fixed-parameter VQE-style ansatz
// A product Ry ansatz has closed-form expectations
// (<Z>=cos t, <X>=sin t per qubit), so the weighted Hamiltonian energy is exact
// and checked at 1e-10. Confirms the facade composes run() + Sum(c_i*<P_i>)
// correctly across an energy sweep. Exact path -> FullState CPU (SparseState
// expectation is deferred in Phase 7).,.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/observable.hpp"
#include "../../../include/primitives.hpp"

using qplayer::Circuit;
using qplayer::Estimator;
using qplayer::Observable;
using qplayer::QRegister;
using qplayer::SimType;

namespace {
// Product ansatz Ry(t) on each of 2 qubits. Per qubit Ry(t)|0> gives
// <Z>=cos(t), <X>=sin(t); the two qubits are unentangled.
Circuit ry_ansatz(double t) {
    Circuit c(2, 2);
    c.ry(t, 0).ry(t, 1);
    return c;
}
}  // namespace

// H = ZI + IZ + 0.5*XX at t=pi/3:
//   <ZI>=<IZ>=cos(pi/3)=0.5 ; <XX>=sin^2(pi/3)=0.75 ; E = 0.5+0.5+0.5*0.75 = 1.375.
TEST(PrimitivesVQE, WeightedHamiltonian_MatchesAnalytic) {
    QRegister qr(SimType::FullState);
    Estimator est(qr);

    const double t = M_PI / 3.0;
    Observable h = Observable::from_list({{"ZI", 1.0}, {"IZ", 1.0}, {"XX", 0.5}});

    double energy = est.run(ry_ansatz(t), h);
    EXPECT_NEAR(energy, 1.375, 1e-10);
}

// Energy of H = ZI + IZ over a sweep equals 2*cos(t) at every point.
TEST(PrimitivesVQE, EnergySweep_MatchesCosCurve) {
    QRegister qr(SimType::FullState);
    Estimator est(qr);
    Observable h = Observable::from_list({{"ZI", 1.0}, {"IZ", 1.0}});

    const double samples[] = {0.0, M_PI / 6.0, M_PI / 2.0, 2.0 * M_PI / 3.0, M_PI};
    for (double t : samples) {
        double energy = est.run(ry_ansatz(t), h);
        EXPECT_NEAR(energy, 2.0 * std::cos(t), 1e-10);  // analytic <ZI>+<IZ>
    }
}

// The facade must equal the manual Sum(c_i * expectation_value(P_i)): run once,
// then evaluate each term by hand on the same final state.
TEST(PrimitivesVQE, Facade_EqualsManualWeightedSum) {
    const double t = 0.7;  // arbitrary angle

    QRegister qr(SimType::FullState);
    qr.run(ry_ansatz(t), 1);
    double manual = 1.0 * qr.expectation_value("ZI") +
                    1.0 * qr.expectation_value("IZ") +
                    0.5 * qr.expectation_value("XX");

    QRegister qr2(SimType::FullState);
    Observable h = Observable::from_list({{"ZI", 1.0}, {"IZ", 1.0}, {"XX", 0.5}});
    double via_facade = Estimator(qr2).run(ry_ansatz(t), h);

    EXPECT_NEAR(via_facade, manual, 1e-12);
}
