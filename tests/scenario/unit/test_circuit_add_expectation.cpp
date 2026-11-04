// test_circuit_add_expectation.cpp — Circuit::add_expectation + expectation_targets
// Covers validation, idempotency, the
// accessor, and JSON round-trip persistence (expectation_targets survives
// to_json/from_json). Pauli qubit order is Qiskit-standard.
//

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;

// Valid Pauli string is registered and retrievable.
TEST(AddExpectation, ValidPauli_Registered) {
    Circuit c(2);
    c.add_expectation("ZZ");
    EXPECT_EQ(c.expectation_targets().size(), std::size_t{1});
    EXPECT_TRUE(c.expectation_targets()[0] == std::string{"ZZ"});
}

// Chainable builder + multiple distinct targets.
TEST(AddExpectation, MultipleDistinct_AllRegistered) {
    Circuit c(2);
    c.add_expectation("ZZ").add_expectation("XX");
    EXPECT_EQ(c.expectation_targets().size(), std::size_t{2});
}

// Duplicate registration is idempotent (no second copy).
TEST(AddExpectation, Duplicate_Idempotent) {
    Circuit c(2);
    c.add_expectation("ZZ").add_expectation("ZZ");
    EXPECT_EQ(c.expectation_targets().size(), std::size_t{1});
}

// Empty Pauli string -> CircuitError.
TEST(AddExpectation, Empty_Throws) {
    Circuit c(2);
    EXPECT_THROW(c.add_expectation(""), CircuitError);
}

// Length mismatch with num_qubits -> CircuitError.
TEST(AddExpectation, WrongLength_Throws) {
    Circuit c(3);
    EXPECT_THROW(c.add_expectation("ZZ"), CircuitError);  // len 2 != 3
}

// Invalid Pauli character -> CircuitError.
TEST(AddExpectation, InvalidChar_Throws) {
    Circuit c(2);
    EXPECT_THROW(c.add_expectation("ZA"), CircuitError);
}

// JSON round-trip preserves expectation_targets.
TEST(AddExpectation, JsonRoundTrip_PreservesTargets) {
    // Arrange
    Circuit c(2);
    c.h(0).cx(0, 1).add_expectation("ZZ").add_expectation("XX");

    // Act
    const std::string json = c.to_json();
    Circuit restored = Circuit::from_json(json);

    // Assert: both targets survive the round-trip, in order.
    EXPECT_EQ(restored.expectation_targets().size(), std::size_t{2});
    EXPECT_TRUE(restored.expectation_targets()[0] == std::string{"ZZ"});
    EXPECT_TRUE(restored.expectation_targets()[1] == std::string{"XX"});
    // The serialized text must mention the field.
    EXPECT_TRUE(json.find("expectation_targets") != std::string::npos);
}

// A circuit with no targets round-trips to an empty target list.
TEST(AddExpectation, JsonRoundTrip_NoTargets_Empty) {
    Circuit c(1);
    c.h(0);
    Circuit restored = Circuit::from_json(c.to_json());
    EXPECT_TRUE(restored.expectation_targets().empty());
}
