// test_primitives.cpp — Estimator / Sampler facades + expectation_value(Observable)
// Analytic Bell-state checks: <ZZ>=1, <XX>=1,
// 0.5*ZZ+0.5*XX = 1.0 (exact, 1e-10). Sampler equals run().counts() under a
// fixed seed. Backend unchanged — values come from the existing exact path.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstdint>
#include <map>
#include <string>

#include "../../../include/qregister.hpp"
#include "../../../include/observable.hpp"
#include "../../../include/primitives.hpp"

using qplayer::Circuit;
using qplayer::Estimator;
using qplayer::Observable;
using qplayer::QRegister;
using qplayer::Sampler;
using qplayer::SimType;

namespace {
// Bell |Phi+> = (|00> + |11>)/sqrt(2): h(0).cx(0,1), no measurement.
Circuit bell_prep() {
    Circuit c(2, 2);
    c.h(0).cx(0, 1);
    return c;
}
}  // namespace

// ── expectation_value(Observable) overload ─────────────────────

// Single-term weighted observable reduces to coeff * <P>.
TEST(Primitives, ExpectationObservable_SingleTerm_ZZ) {
    QRegister qr(SimType::FullState);
    qr.run(bell_prep(), 1);

    Observable obs = Observable::from_list({{"ZZ", 1.0}});
    EXPECT_NEAR(qr.expectation_value(obs), 1.0, 1e-10);  // Bell <ZZ> = 1
}

// Weighted sum 0.5*<ZZ> + 0.5*<XX> = 0.5 + 0.5 = 1.0 on the Bell state.
TEST(Primitives, ExpectationObservable_WeightedSum) {
    QRegister qr(SimType::FullState);
    qr.run(bell_prep(), 1);

    Observable obs = Observable::from_list({{"ZZ", 0.5}, {"XX", 0.5}});
    EXPECT_NEAR(qr.expectation_value(obs), 1.0, 1e-10);
}

// Empty observable evaluates to 0.0 (boundary).
TEST(Primitives, ExpectationObservable_Empty_IsZero) {
    QRegister qr(SimType::FullState);
    qr.run(bell_prep(), 1);

    Observable empty;
    EXPECT_NEAR(qr.expectation_value(empty), 0.0, 1e-15);
}

// ── Estimator facade ───────────────────────────────────────────

// Estimator wraps run() + expectation_value(obs): one call returns <H>.
TEST(Primitives, Estimator_Bell_WeightedSum_Exact) {
    QRegister qr(SimType::FullState);
    Estimator est(qr);

    Observable obs = Observable::from_list({{"ZZ", 0.5}, {"XX", 0.5}});
    EXPECT_NEAR(est.run(bell_prep(), obs), 1.0, 1e-10);
}

// <YY> on |Phi+> is -1 — sign is carried correctly through the facade.
TEST(Primitives, Estimator_Bell_YY_IsMinusOne) {
    QRegister qr(SimType::FullState);
    Estimator est(qr);

    Observable obs = Observable::from_list({{"YY", 1.0}});
    EXPECT_NEAR(est.run(bell_prep(), obs), -1.0, 1e-10);
}

// ── Sampler facade ─────────────────────────────────────────────

// Sampler returns the measurement distribution; Bell yields only 00 / 11.
TEST(Primitives, Sampler_Bell_OnlyCorrelatedOutcomes) {
    QRegister qr(SimType::FullState);
    qr.set_seed(42);
    Sampler sampler(qr);

    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    auto counts = sampler.run(c, 10000);

    std::uint64_t total = 0;
    for (const auto& [bits, n] : counts) total += n;
    EXPECT_EQ(total, std::uint64_t{10000});                 // all shots accounted
    EXPECT_TRUE(counts.find("01") == counts.end());         // no anti-correlated
    EXPECT_TRUE(counts.find("10") == counts.end());
    EXPECT_NEAR(static_cast<double>(counts["00"]) / 10000.0, 0.5, 0.03);  // ~50%
    EXPECT_NEAR(static_cast<double>(counts["11"]) / 10000.0, 0.5, 0.03);
}

// Sampler.run() equals QRegister::run().counts() under the same fixed seed.
TEST(Primitives, Sampler_MatchesDirectRunCounts) {
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    QRegister qr(SimType::FullState);
    qr.set_seed(7);
    auto via_sampler = Sampler(qr).run(c, 4096);

    qr.set_seed(7);
    auto via_direct = qr.run(c, 4096).counts();

    EXPECT_TRUE(via_sampler == via_direct);  // bit-identical under same seed
}
