// test_bell_state.cpp — QRegister Batch-mode Bell pair + state-access
// API. Verifies the distribution matches theory, and that
// QRegister's get_statevector / get_statevector_raw / expectation_value
// delegate correctly to the backend.
// All tests migrated to TEST_BACKENDS: distribution, AoS/SoA
// consistency, and expectation values hold on both CPU and GPU.
//

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"
#include "../../framework/backend_params.hpp"  // TEST_BACKENDS,

#include <cstdint>

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"

using qplayer::QRegister;
using qplayer::Circuit;
using qplayer::ProcType;
using qplayer::SimType;
using qplayer::Result;
using qplayer::StateVector;
using qplayer::RawStateView;

// Bell distribution: |00> and |11> each ~0.5, |01>/|10> absent.
// Runs on both CPU and GPU (TEST_BACKENDS).
TEST_BACKENDS(BellState, Batch_Distribution_MatchesTheory) {
    // Arrange
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    qr.set_seed(42);
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act
    Result r = qr.run(circ, 10000);
    const auto& counts = r.counts();

    // Assert: only correlated outcomes appear, each near 0.5 (nshots=10000 stat tol).
    auto frac = [&](const char* k) {
        auto it = counts.find(k);
        return it == counts.end() ? 0.0
                                  : static_cast<double>(it->second) / 10000.0;
    };
    EXPECT_NEAR(frac("00"), 0.5, 0.02);  // statistical tolerance @ 10000 shots
    EXPECT_NEAR(frac("11"), 0.5, 0.02);
    EXPECT_NEAR(frac("01"), 0.0, 1e-9);  // impossible outcomes
    EXPECT_NEAR(frac("10"), 0.0, 1e-9);
}

// QRegister::get_statevector (AoS) agrees with get_statevector_raw (SoA).
// Both backends download from the same quantum state, so the two views must be
// bit-identical (1e-15 tolerance). Runs on both CPU and GPU.
TEST_BACKENDS(BellState, StateAccess_AoSMatchesRaw) {
    // Arrange: no measurement -> leaves |ψ_final⟩ available.
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(2);
    circ.h(0).cx(0, 1);
    qr.run(circ, 1);

    // Act
    const StateVector  aos = qr.get_statevector();
    const RawStateView raw = qr.get_statevector_raw();

    // Assert: amplitudes agree to 1e-15. Bell: |00> and |11| are 1/sqrt(2).
    for (std::size_t i = 0; i < aos.size(); ++i) {
        EXPECT_NEAR(raw.re[i], aos[i].real(), 1e-15);
        EXPECT_NEAR(raw.im[i], aos[i].imag(), 1e-15);
    }
    EXPECT_NEAR(aos[0].real(), 0.70710678, 1e-6);
    EXPECT_NEAR(aos[3].real(), 0.70710678, 1e-6);
}

// QRegister::expectation_value delegates to the backend (<ZZ> Bell = 1).
// Runs on both CPU and GPU; algebraic result is backend-independent.
TEST_BACKENDS(BellState, StateAccess_ExpectationZZ) {
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(2);
    circ.h(0).cx(0, 1);
    qr.run(circ, 1);

    EXPECT_NEAR(qr.expectation_value("ZZ"), 1.0, 1e-10);
}

// No-arg expectation_value() defaults to all-Z over every qubit, so on a Bell
// pair it equals expectation_value("ZZ") == +1 (algebraic, tol 1e-10).
// Runs on both CPU and GPU.
TEST_BACKENDS(BellState, StateAccess_ExpectationDefaultAllZ) {
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(2);
    circ.h(0).cx(0, 1);
    qr.run(circ, 1);

    EXPECT_NEAR(qr.expectation_value(), qr.expectation_value("ZZ"), 1e-10);
    EXPECT_NEAR(qr.expectation_value(), 1.0, 1e-10);
}

// On |+++> (all Hadamard) every qubit is an eigenstate of X, so <Z...Z> = 0.
// Guards the all-Z default on a 3-qubit register (length built from num_qubits).
// Runs on both CPU and GPU.
TEST_BACKENDS(BellState, StateAccess_ExpectationDefaultAllZ_PlusState) {
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(3);
    circ.h(0).h(1).h(2);
    qr.run(circ, 1);

    EXPECT_NEAR(qr.expectation_value(), qr.expectation_value("ZZZ"), 1e-10);
    EXPECT_NEAR(qr.expectation_value(), 0.0, 1e-10);  // <Z> = 0 per qubit
}
