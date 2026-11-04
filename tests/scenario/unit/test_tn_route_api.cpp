// test_tn_route_api.cpp — QRegister::set_tn_noise_path() / set_mps_truncation() /
// set_mpo_truncation() Public API tests.
// Tests follow the Arrange / Act / Assert layout.
//
// The facade owns three things here and each is a separate claim: that the route
// string actually reaches the backend and changes which physics runs, that a bad
// string fails at the call instead of silently running the other route, and that
// the knobs a non-TN backend cannot honour do not pretend to have worked.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <map>
#include <string>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/noise/noise_model.hpp"
#include "../../../include/error.hpp"
// Ceilings live with the backend policy constants;
// the test names them rather than repeating the numbers.
#include "../../../include/backend/tensor_network/tn_backend.hpp"
#include "../../../include/backend/tensor_network/tn_mpo.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::SimType;

namespace {

// A brickwork wide enough that a low bond cap has to truncate, so the knobs have
// an observable consequence rather than only a stored value.
Circuit entangling_circuit(QubitIndex n, int depth) {
    Circuit c(n, n);
    for (int layer = 0; layer < depth; ++layer) {
        for (QubitIndex q = static_cast<QubitIndex>(layer % 2); q + 1 < n; q += 2) {
            c.ry(0.63, q).ry(0.41, q + 1).cx(q, q + 1);
        }
    }
    c.measure_all();
    return c;
}

double tvd(const std::map<std::string, std::uint64_t>& a,
           const std::map<std::string, std::uint64_t>& b, std::uint64_t shots) {
    double d = 0.0;
    for (const auto& kv : a) {
        const auto it = b.find(kv.first);
        const double pb = (it == b.end()) ? 0.0 : static_cast<double>(it->second) / shots;
        d += std::abs(static_cast<double>(kv.second) / shots - pb);
    }
    for (const auto& kv : b) {
        if (a.find(kv.first) == a.end()) d += static_cast<double>(kv.second) / shots;
    }
    return 0.5 * d;
}

}  // namespace

TEST(TnRouteApi, NoisePathRejectsUnknownRoute) {
    // Arrange / Act / Assert: an env typo keeps MC and warns, but a
    // bad argument here is a caller bug — running the other route silently is the
    // worst failure this switch has.
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    EXPECT_THROW(qr.set_tn_noise_path("mpa"), CircuitError);
    EXPECT_THROW(qr.set_tn_noise_path(""), CircuitError);
    EXPECT_THROW(qr.set_tn_noise_path("MPO"), CircuitError);   // case is not folded
    EXPECT_NO_THROW(qr.set_tn_noise_path("mc"));
    EXPECT_NO_THROW(qr.set_tn_noise_path("mpo"));
}

TEST(TnRouteApi, MpoRouteReachesTheBackendAndAgreesWithMonteCarlo) {
    // Arrange: the same NoiseModel and circuit down both routes. This is the one
    // test that proves the string is wired through rather than stored — the MPO
    // route carries rho exactly, so agreement is a claim about the physics.
    Circuit circ(5, 5);
    circ.ry(0.9, 0).ry(1.3, 1).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4);
    circ.measure_all();
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.02);
    const std::uint32_t shots = 20000;

    QRegister mc(SimType::TensorNetwork, ProcType::CPU);
    mc.set_seed(4242);
    mc.set_noise_model(nm);

    QRegister mpo(SimType::TensorNetwork, ProcType::CPU);
    mpo.set_seed(4242);
    mpo.set_noise_model(nm);
    mpo.set_tn_noise_path("mpo");

    // Act
    const auto mc_counts  = mc.run(circ, shots).counts();
    const auto mpo_counts = mpo.run(circ, shots).counts();

    // Assert
    EXPECT_NEAR(tvd(mc_counts, mpo_counts, shots), 0.0, 0.02);
}

TEST(TnRouteApi, DefaultRouteIsMonteCarlo) {
    // Arrange: the two routes are hard to tell apart from the outside — both
    // refuse state access afterwards and both return counts. Asymmetric caps make
    // the difference unambiguous: rho is capped at 4 while the state has 4096 to
    // spare, so a lossless run can only have been the sampled route and a lossy
    // one can only have been the density matrix.
    const Circuit circ = entangling_circuit(8, 6);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.01);

    QRegister def(SimType::TensorNetwork, ProcType::CPU);
    def.set_seed(7);
    def.set_noise_model(nm);
    def.set_mps_truncation(4096, 1e-12);
    def.set_mpo_truncation(4, 1e-12);

    QRegister forced(SimType::TensorNetwork, ProcType::CPU);
    forced.set_seed(7);
    forced.set_noise_model(nm);
    forced.set_mps_truncation(4096, 1e-12);
    forced.set_mpo_truncation(4, 1e-12);
    forced.set_tn_noise_path("mpo");

    // Act
    const auto plain = def.run(circ, 8);
    const auto rho   = forced.run(circ, 8);

    // Assert
    double plain_worst = 0.0, rho_worst = 0.0;
    for (double e : plain.truncation_errors()) plain_worst = std::max(plain_worst, e);
    for (double e : rho.truncation_errors())   rho_worst   = std::max(rho_worst, e);
    EXPECT_NEAR(plain_worst, 0.0, 1e-15);
    EXPECT_TRUE(rho_worst > 0.0);
}

TEST(TnRouteApi, MpsTruncationKnobChangesTheReportedError) {
    // Arrange: the facade knob must reach the same place the env var does. A tight
    // cap has to lose weight where a generous one does not — that difference is
    // the observable, since the stored setting itself is not visible.
    const Circuit circ = entangling_circuit(12, 8);
    NoiseModel nm;
    nm.add_depolarizing("cx", 0.01);

    QRegister tight(SimType::TensorNetwork, ProcType::CPU);
    tight.set_seed(11);
    tight.set_noise_model(nm);
    tight.set_mps_truncation(4, 1e-12);

    QRegister roomy(SimType::TensorNetwork, ProcType::CPU);
    roomy.set_seed(11);
    roomy.set_noise_model(nm);
    roomy.set_mps_truncation(4096, 1e-12);

    // Act
    const auto capped   = tight.run(circ, 4);
    const auto uncapped = roomy.run(circ, 4);

    // Assert
    double capped_sum = 0.0, uncapped_sum = 0.0;
    for (double e : capped.truncation_errors())   capped_sum   += e * e;
    for (double e : uncapped.truncation_errors()) uncapped_sum += e * e;
    EXPECT_TRUE(std::sqrt(capped_sum) > 0.1);
    EXPECT_NEAR(std::sqrt(uncapped_sum), 0.0, 1e-15);
}

TEST(TnRouteApi, TruncationSettersRejectUnhonourableValues) {
    // Arrange / Act / Assert: validation belongs to the backend config types, so
    // this checks the facade forwards rather than swallowing. A zero cap cannot
    // hold a single singular value and a negative cutoff has no meaning.
    QRegister qr(SimType::TensorNetwork, ProcType::CPU);
    EXPECT_THROW(qr.set_mps_truncation(0, 1e-12), CircuitError);
    EXPECT_THROW(qr.set_mps_truncation(8, -1.0), CircuitError);
    EXPECT_THROW(qr.set_mpo_truncation(0, 1e-12), CircuitError);
    EXPECT_THROW(qr.set_mpo_truncation(8, -1.0), CircuitError);
    EXPECT_THROW(qr.set_mps_truncation(qplayer::kMpsMaxBondCeiling + 1u, 1e-12), CircuitError);
    EXPECT_THROW(qr.set_mpo_truncation(qplayer::kMpoMaxBondCeiling + 1u, 1e-12), CircuitError);
}

TEST(TnRouteApi, KnobsOnANonTnBackendAreIgnoredNotFatal) {
    // Arrange: a script that sweeps backends reuses one setup function, so these
    // must not throw on FullState — the warning is what tells the
    // user the knob had no effect. And the run must be unaffected: a forwarded
    // bond cap must not, for instance, be mistaken for a fusion width.
    Circuit circ(3, 3);
    circ.h(0).cx(0, 1).cx(1, 2).measure_all();

    QRegister fs(SimType::FullState, ProcType::CPU);
    fs.set_seed(5);
    EXPECT_NO_THROW(fs.set_tn_noise_path("mpo"));
    EXPECT_NO_THROW(fs.set_mps_truncation(4, 1e-12));
    EXPECT_NO_THROW(fs.set_mpo_truncation(4, 1e-12));

    // Act
    const auto r = fs.run(circ, 4000);

    // Assert: GHZ halves, and no truncation channel appears on a dense backend.
    EXPECT_NEAR(static_cast<double>(r.counts().at("000")) / 4000.0, 0.5, 0.03);
    EXPECT_NEAR(static_cast<double>(r.counts().at("111")) / 4000.0, 0.5, 0.03);
    EXPECT_FALSE(r.has_truncation_errors());
}
