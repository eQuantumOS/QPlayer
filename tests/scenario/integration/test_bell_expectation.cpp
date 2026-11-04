// test_bell_expectation.cpp — Result.expectation dict accumulation (
// model A). QRegister::run() fills Result.expectation for each Pauli
// target pre-registered via Circuit::add_expectation (fast path).
// Non-registered targets are absent (has_expectation == false, expectation()
// throws). Migrated to TEST_BACKENDS: statevector path and
// error handling are backend-independent.
// .

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"
#include "../../framework/backend_params.hpp"  // TEST_BACKENDS,

#include "../../../include/qregister.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::QRegister;
using qplayer::Circuit;
using qplayer::ProcType;
using qplayer::SimType;
using qplayer::Result;
using qplayer::CircuitError;

// Bell state with add_expectation("ZZ") -> Result.expectation("ZZ") == +1.
// fast path uses backend->expectation_value() (algebraic, tol 1e-10).
// Runs on both CPU and GPU (TEST_BACKENDS).
TEST_BACKENDS(BellExpectation, RegisteredZZ_AccumulatesToOne) {
    // Arrange: Bell pair, register <ZZ>. No measurement -> fast path.
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    qr.set_seed(1);
    Circuit circ(2);
    circ.h(0).cx(0, 1).add_expectation("ZZ");

    // Act
    Result r = qr.run(circ, 1000);

    // Assert: <ZZ> on the Bell state is exactly +1 (algebraic, tol 1e-10).
    EXPECT_TRUE(r.has_expectation("ZZ"));
    EXPECT_NEAR(r.expectation("ZZ"), 1.0, 1e-10);
}

// Multiple registered targets are all accumulated (<ZZ> = <XX> = +1 on Bell).
// Runs on both CPU and GPU (TEST_BACKENDS).
TEST_BACKENDS(BellExpectation, MultipleTargets_AllAccumulated) {
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(2);
    circ.h(0).cx(0, 1).add_expectation("ZZ").add_expectation("XX");

    Result r = qr.run(circ, 500);

    EXPECT_NEAR(r.expectation("ZZ"), 1.0, 1e-10);
    EXPECT_NEAR(r.expectation("XX"), 1.0, 1e-10);
    EXPECT_EQ(r.all_expectations().size(), std::size_t{2});
}

// No registration -> the dict stays empty; has_expectation false, expectation throws.
// Error handling is backend-independent; runs on both CPU and GPU.
TEST_BACKENDS(BellExpectation, NotRegistered_AbsentAndThrows) {
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(2);
    circ.h(0).cx(0, 1);  // no add_expectation

    Result r = qr.run(circ, 100);

    EXPECT_FALSE(r.has_expectation("ZZ"));
    EXPECT_THROW(r.expectation("ZZ"), CircuitError);
}
