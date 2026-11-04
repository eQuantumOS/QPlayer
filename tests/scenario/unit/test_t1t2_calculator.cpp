// test_t1t2_calculator.cpp — Phase 8 unit tests for T1T2Calculator
// Verifies the gamma / lambda formulas, the
// infinity + T2 >= 2 T1 clamps, and the assembled relaxation Kraus channels.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "../../../include/noise/t1t2_calculator.hpp"

using qplayer::ChannelType;
using qplayer::KrausChannel;
using qplayer::QubitIndex;
using qplayer::T1T2Calculator;
using qplayer::UnitaryMatrix;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

bool is_trace_preserving(const std::vector<UnitaryMatrix>& ops, double tol) {
    if (ops.empty()) return false;
    const std::size_t dim = ops.front().size();
    for (std::size_t i = 0; i < dim; ++i) {
        for (std::size_t j = 0; j < dim; ++j) {
            std::complex<double> acc{0.0, 0.0};
            for (const auto& K : ops)
                for (std::size_t k = 0; k < dim; ++k)
                    acc += std::conj(K[k][i]) * K[k][j];
            const std::complex<double> target =
                (i == j) ? std::complex<double>{1.0, 0.0} : std::complex<double>{0.0, 0.0};
            if (std::abs(acc - target) > tol) return false;
        }
    }
    return true;
}

}  // namespace

// ── gamma / lambda formulas ──────────────────────────────────────────────────

TEST(T1T2Calculator, AmplitudeGamma_MatchesFormula) {
    // gamma = 1 - exp(-t/T1).
    const double t1 = 50000.0, t = 300.0;
    EXPECT_NEAR(T1T2Calculator::amplitude_damping_gamma(t1, t),
                1.0 - std::exp(-t / t1), 1e-12);
}

TEST(T1T2Calculator, AmplitudeGamma_InfiniteT1_IsZero) {
    EXPECT_NEAR(T1T2Calculator::amplitude_damping_gamma(kInf, 300.0), 0.0, 1e-15);
}

TEST(T1T2Calculator, DephasingLambda_MatchesFormula) {
    // 1/T_phi = 1/T2 - 1/(2 T1); lambda = 1 - exp(-2 t/T_phi) (factor 2).
    const double t1 = 50000.0, t2 = 30000.0, t = 300.0;
    const double inv_tphi = 1.0 / t2 - 1.0 / (2.0 * t1);
    EXPECT_NEAR(T1T2Calculator::dephasing_lambda(t1, t2, t),
                1.0 - std::exp(-2.0 * t * inv_tphi), 1e-12);
}

TEST(T1T2Calculator, Compute_CoherenceDecaysAsExpMinusTOverT2) {
    // : the assembled amplitude+phase damping must shrink coherence by
    // exp(-t/T2). Amplitude damping contributes coherence factor sqrt(1-gamma) =
    // K0[1][1]; phase damping contributes sqrt(1-lambda) = K0[1][1]. Their product
    // is the total coherence multiplier applied to rho_01.
    const double t1 = 131528.6, t2 = 102203.9, t = 20000.0;  // manila q0, 20 us
    auto chans = T1T2Calculator::compute(t1, t2, t, /*q=*/0);
    EXPECT_EQ(chans.size(), static_cast<std::size_t>(2));

    const double amp_coh   = chans[0].operators[0][1][1].real();  // sqrt(1-gamma)
    const double phase_coh = chans[1].operators[0][1][1].real();  // sqrt(1-lambda)
    const double total_coh = amp_coh * phase_coh;

    // Tight tolerance: this is an exact analytic identity, not statistics.
    EXPECT_NEAR(total_coh, std::exp(-t / t2), 1e-9);
}

TEST(T1T2Calculator, DephasingLambda_T2AtTwiceT1_IsZero) {
    // T2 == 2 T1 -> pure-dephasing rate is exactly 0 (all decoherence from T1).
    EXPECT_NEAR(T1T2Calculator::dephasing_lambda(1000.0, 2000.0, 300.0), 0.0, 1e-15);
}

TEST(T1T2Calculator, DephasingLambda_InfiniteT2_ClampsToZero) {
    // T2 = inf > 2 T1 -> unphysical pure-dephasing rate clamps to 0.
    EXPECT_NEAR(T1T2Calculator::dephasing_lambda(1000.0, kInf, 300.0), 0.0, 1e-15);
}

// ── compute() assembled channels ─────────────────────────────────────────────

TEST(T1T2Calculator, Compute_FiniteCoherence_ReturnsTwoChannels) {
    // Arrange — T2 < 2 T1 so both amplitude damping and dephasing are non-zero.
    auto chans = T1T2Calculator::compute(50000.0, 30000.0, 300.0, /*q=*/2);

    // Assert
    EXPECT_EQ(chans.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<int>(chans[0].type), static_cast<int>(ChannelType::AmplitudeDamping));
    EXPECT_EQ(static_cast<int>(chans[1].type), static_cast<int>(ChannelType::PhaseDamping));
    EXPECT_EQ(chans[0].target_qubit, static_cast<QubitIndex>(2));
    EXPECT_TRUE(is_trace_preserving(chans[0].operators, 1e-10));
    EXPECT_TRUE(is_trace_preserving(chans[1].operators, 1e-10));
}

TEST(T1T2Calculator, Compute_InfiniteCoherence_ReturnsEmpty) {
    // Unknown qubit (infinite T1/T2) -> no relaxation channels.
    EXPECT_TRUE(T1T2Calculator::compute(kInf, kInf, 300.0, 0).empty());
}

TEST(T1T2Calculator, Compute_OnlyAmplitudeWhenT2Infinite) {
    // T1 finite, T2 infinite -> amplitude damping only (lambda clamps to 0).
    auto chans = T1T2Calculator::compute(50000.0, kInf, 300.0, 0);
    EXPECT_EQ(chans.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(static_cast<int>(chans[0].type), static_cast<int>(ChannelType::AmplitudeDamping));
}
