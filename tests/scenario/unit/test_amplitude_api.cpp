// test_amplitude_api.cpp — QRegister::amplitude() / reduced_density_matrix()
// Public API tests.
// 1.1, 1.3 (Arrange/Act/Assert).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::SimType;
using qplayer::SimulatorError;
using qplayer::StateIndex;

namespace {

Circuit asymmetric_circuit(QubitIndex n) {
    // Distinct 1Q gates per qubit: a symmetric state could not detect an index
    // convention bug, which is the whole risk in an index-taking API.
    Circuit c(n, n);
    c.h(0).t(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) c.cx(q, q + 1);
    if (n > 2) c.s(2);
    return c;
}

}  // namespace

// ── amplitude() agrees with get_statevector() on every backend ────────────────

TEST(AmplitudeApi, FullState_MatchesStatevectorEntryByEntry) {
    // Arrange
    QRegister qr(SimType::FullState, ProcType::CPU);
    const Circuit circ = asymmetric_circuit(4);

    // Act
    qr.run(circ, 1);
    const auto sv = qr.get_statevector();

    // Assert: the facade's index space is the statevector's.
    double max_diff = 0.0;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(qr.amplitude(StateIndex{i}) - sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-14);
}

TEST(AmplitudeApi, SparseState_MatchesStatevectorEntryByEntry) {
    // Arrange
    QRegister qr(SimType::SparseState);
    const Circuit circ = asymmetric_circuit(4);

    // Act
    qr.run(circ, 1);
    const auto sv = qr.get_statevector();

    // Assert: a sparse miss must read as 0, not as an error (policy).
    double max_diff = 0.0;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(qr.amplitude(StateIndex{i}) - sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-14);
}

TEST(AmplitudeApi, TensorNetwork_MatchesStatevectorEntryByEntry) {
    // Arrange
    QRegister qr(SimType::TensorNetwork);
    const Circuit circ = asymmetric_circuit(4);

    // Act
    qr.run(circ, 1);
    const auto sv = qr.get_statevector();

    // Assert
    double max_diff = 0.0;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(qr.amplitude(StateIndex{i}) - sv[i]));
    }
    EXPECT_NEAR(max_diff, 0.0, 1e-12);
}

TEST(AmplitudeApi, TensorNetwork_WorksBeyondDenseExportCap) {
    // Arrange: 30 qubits — get_statevector() refuses, amplitude() does not.
    const QubitIndex n = 30;
    QRegister qr(SimType::TensorNetwork);
    Circuit circ(n, n);
    circ.h(0);
    for (QubitIndex q = 0; q + 1 < n; ++q) circ.cx(q, q + 1);

    // Act
    qr.run(circ, 1);

    // Assert: GHZ over 30 qubits — 1/sqrt2 on the two extremes, 0 elsewhere.
    const StateIndex all_ones = (StateIndex{1} << n) - 1;
    EXPECT_NEAR(std::abs(qr.amplitude(StateIndex{0})), M_SQRT1_2, 1e-12);
    EXPECT_NEAR(std::abs(qr.amplitude(all_ones)),      M_SQRT1_2, 1e-12);
    EXPECT_NEAR(std::abs(qr.amplitude(StateIndex{1} << 9)), 0.0,  1e-12);
    EXPECT_THROW(qr.get_statevector(), SimulatorError);
}

TEST(AmplitudeApi, Amplitude_OutOfRangeBasis_Throws) {
    QRegister qr(SimType::TensorNetwork);
    qr.run(asymmetric_circuit(3), 1);
    EXPECT_THROW(qr.amplitude(StateIndex{8}), SimulatorError);  // 2^3 is out of range
}

// ── reduced_density_matrix() is TensorNetwork-only ──────────────

TEST(AmplitudeApi, Rdm_TensorNetwork_BellHalfIsMaximallyMixed) {
    // Arrange
    QRegister qr(SimType::TensorNetwork);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1);

    // Act
    qr.run(circ, 1);
    const auto rho = qr.reduced_density_matrix({0});

    // Assert
    EXPECT_EQ(rho.size(), static_cast<std::size_t>(4));
    EXPECT_NEAR(rho[0].real(), 0.5, 1e-12);
    EXPECT_NEAR(rho[3].real(), 0.5, 1e-12);
    EXPECT_NEAR(std::abs(rho[1]), 0.0, 1e-12);
}

TEST(AmplitudeApi, Rdm_DenseBackends_ThrowWithAnAlternative) {
    // Arrange + Act + Assert: a dense user can partial-trace get_statevector(),
    // so duplicating the same computation in two backends would only risk convention drift.
    QRegister fs(SimType::FullState, ProcType::CPU);
    fs.run(asymmetric_circuit(3), 1);
    EXPECT_THROW(fs.reduced_density_matrix({0}), SimulatorError);

    QRegister sp(SimType::SparseState);
    sp.run(asymmetric_circuit(3), 1);
    EXPECT_THROW(sp.reduced_density_matrix({0}), SimulatorError);
}
