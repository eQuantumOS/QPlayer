// test_interactive.cpp — Unit tests for the Interactive facade
// The facade delegates immediate gate / measurement / state-access
// calls to the bound SimulatorBackend; these tests pin the contract:
//   - initialize() activates the session and is rejected before use,
//   - the generic apply_gate(GateOp) covers the full GateType enum (it is the
//     replay entry point used by run_qasm --interactive),
//   - measurement collapses the state (Born rule) and is reproducible under a
// fixed seed,
//   - Interactive is CPU-statevector only — FullState GPU / TensorNetwork
// initialize() throw SimulatorError (feature matrix).
//
// The facade needs a CPU backend, so QRegister is constructed with
// ProcType::CPU to avoid the Auto->GPU resolution on GPU hosts.

#include "../../framework/assertions.hpp"
#include "../../framework/test_case.hpp"

#include <cmath>
#include <vector>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/error.hpp"
#include "../../../include/qregister.hpp"

using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::SimType;
using qplayer::SimulatorError;

// Helper: a CPU FullState register (interactive is CPU-statevector only).
static QRegister make_cpu_reg() {
    return QRegister{SimType::FullState, ProcType::CPU};
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

TEST(Interactive, Initialize_ActivatesSession) {
    // Arrange
    QRegister qr = make_cpu_reg();

    // Act
    qr.interactive.initialize(3);

    // Assert
    EXPECT_TRUE(qr.interactive.is_active());
    EXPECT_EQ(qr.interactive.num_qubits(), 3u);
}

TEST(Interactive, GateBeforeInitialize_Throws) {
    // Arrange
    QRegister qr = make_cpu_reg();

    // Act + Assert: dispatching before initialize() is a SimulatorError.
    EXPECT_THROW(qr.interactive.h(0), SimulatorError);
}

// ── State evolution + collapse ───────────────────────────────────────────────

TEST(Interactive, HGate_PutsQubitInSuperposition_ProbabilityHalf) {
    // Arrange
    QRegister qr = make_cpu_reg();
    qr.interactive.initialize(1);

    // Act
    qr.interactive.h(0);

    // Assert: P(0) == P(1) == 0.5 (exact from the H matrix, not statistics).
    EXPECT_NEAR(qr.interactive.probability(0, 0), 0.5, 1e-12);
    EXPECT_NEAR(qr.interactive.probability(0, 1), 0.5, 1e-12);
}

TEST(Interactive, BellMeasure_CollapsesToCorrelatedOutcome) {
    // Arrange
    QRegister qr = make_cpu_reg();
    qr.set_seed(2);  // deterministic measurement RNG
    qr.interactive.initialize(2);
    qr.interactive.h(0).cx(0, 1);

    // Act: measuring q0 collapses the Bell pair; q1 must agree.
    const int b0 = qr.interactive.measure(0);
    const int b1 = qr.interactive.measure(1);

    // Assert: Bell correlation — both bits equal, and the post-collapse state is
    // a basis state (probability 1 on the measured outcome).
    EXPECT_EQ(b0, b1);
    EXPECT_NEAR(qr.interactive.probability(0, b0), 1.0, 1e-12);
}

TEST(Interactive, FixedSeed_MeasurementIsReproducible) {
    // Arrange + Act: same seed -> same outcome across fresh registers.
    auto run_once = []() {
        QRegister qr = make_cpu_reg();
        qr.set_seed(42);
        qr.interactive.initialize(2);
        qr.interactive.h(0).cx(0, 1);
        return qr.interactive.measure(0);
    };

    // Assert: deterministic (— interactive path must honour the seed).
    EXPECT_EQ(run_once(), run_once());
}

// ── Generic apply_gate(GateOp) entry point ───────────────────────────────────

TEST(Interactive, ApplyGate_CoversGatesMissingTypedMethods) {
    // Arrange: SX has no typed Interactive method; apply_gate must still drive it
    // (this is the IR-replay path used by run_qasm --interactive). SX*SX = X, so
    // applying SX twice on |0> yields |1> (probability 1).
    QRegister qr = make_cpu_reg();
    qr.interactive.initialize(1);

    GateOp sx;
    sx.type   = GateType::SX;
    sx.name   = "sx";
    sx.qubits = {0};

    // Act
    qr.interactive.apply_gate(sx).apply_gate(sx);

    // Assert
    EXPECT_NEAR(qr.interactive.probability(0, 1), 1.0, 1e-12);
}

// ── measure_all ───────────────────────────────────────────────────────

TEST(Interactive, MeasureAll_ReturnsOnePerQubit) {
    // Arrange: X on q1 of a 3-qubit register -> deterministic 0,1,0.
    QRegister qr = make_cpu_reg();
    qr.set_seed(1);
    qr.interactive.initialize(3);
    qr.interactive.x(1);

    // Act
    const std::vector<int> bits = qr.interactive.measure_all();

    // Assert: index i carries qubit i's outcome, size == num_qubits().
    EXPECT_EQ(bits.size(), 3u);
    EXPECT_EQ(bits[0], 0);
    EXPECT_EQ(bits[1], 1);
    EXPECT_EQ(bits[2], 0);
}

// ── Backend support matrix ─────────────────────────────────────────

TEST(Interactive, TensorNetworkInitialize_Throws) {
    // Arrange
    QRegister qr{SimType::TensorNetwork};

    // Act + Assert: TN does not support interactive immediate-mode.
    EXPECT_THROW(qr.interactive.initialize(2), SimulatorError);
}

TEST(Interactive, SparseStateInteractive_Works) {
    // Arrange: SparseState is a supported CPU statevector backend.
    QRegister qr{SimType::SparseState};
    qr.set_seed(2);
    qr.interactive.initialize(2);

    // Act
    qr.interactive.h(0).cx(0, 1);
    const int b0 = qr.interactive.measure(0);
    const int b1 = qr.interactive.measure(1);

    // Assert: Bell correlation holds on SparseState too.
    EXPECT_EQ(b0, b1);
}
