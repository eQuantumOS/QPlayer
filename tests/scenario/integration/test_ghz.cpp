// test_ghz.cpp — QRegister Batch-mode GHZ state. A 5-qubit GHZ circuit
// (H then a CX chain) measured into a 5-bit register yields only |00000> and
// |11111>, each ~0.5 (fast path). Migrated to TEST_BACKENDS
// so distribution and expectation value are verified on both CPU and GPU.
// .

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

// GHZ-5 distribution: all-zeros and all-ones each ~0.5; nothing else appears.
// Runs on both CPU and GPU (TEST_BACKENDS).
TEST_BACKENDS(GHZState, Batch_FiveQubit_Distribution) {
    // Arrange: H q0; CX(0,1); CX(1,2); CX(2,3); CX(3,4); measure_all.
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    qr.set_seed(99);
    Circuit circ(5, 5);
    circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4).measure_all();

    // Act
    Result r = qr.run(circ, 10000);
    const auto& counts = r.counts();

    // Assert: exactly two outcomes, correlated all-0 / all-1, each near 0.5.
    auto frac = [&](const char* k) {
        auto it = counts.find(k);
        return it == counts.end() ? 0.0
                                  : static_cast<double>(it->second) / 10000.0;
    };
    EXPECT_NEAR(frac("00000"), 0.5, 0.02);  // statistical tolerance @ 10000 shots
    EXPECT_NEAR(frac("11111"), 0.5, 0.02);
    // No partial bitstring should appear (GHZ has no other support).
    EXPECT_EQ(counts.size(), std::size_t{2});
}

// GHZ pairwise Z correlation: <Z_a Z_b> = +1 for any two qubits (even # of Z).
// "ZZIII" = Z on qubits 4 and 3 (Qiskit order). The full <ZZZZZ> would be 0
// instead (odd parity: (1 + (-1)^5)/2 = 0), so we test the even-parity case.
// Algebraic result is backend-independent; runs on both CPU and GPU.
TEST_BACKENDS(GHZState, ExpectationPairwiseZ) {
    QRegister qr(SimType::FullState, proc);  // proc is CPU or GPU
    Circuit circ(5);
    circ.h(0).cx(0, 1).cx(1, 2).cx(2, 3).cx(3, 4);
    qr.run(circ, 1);

    EXPECT_NEAR(qr.expectation_value("ZZIII"), 1.0, 1e-10);
}
