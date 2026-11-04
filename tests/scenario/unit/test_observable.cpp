// test_observable.cpp — Observable weighted Pauli-sum value type
// Covers construction, chaining, accessors,
// boundary cases, and Pauli validation (empty / non-I/X/Y/Z -> CircuitError).
// Pauli qubit order is Qiskit big-endian.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <string>

#include "../../../include/observable.hpp"
#include "../../../include/error.hpp"

using qplayer::CircuitError;
using qplayer::Observable;

// ── Normal path ────────────────────────────────────────────────

// from_list builds the observable with coefficients and Pauli strings intact.
TEST(Observable, FromList_PreservesTermsAndCoeffs) {
    Observable obs = Observable::from_list({{"ZZ", 0.5}, {"XX", 0.5}});

    EXPECT_EQ(obs.size(), std::size_t{2});
    EXPECT_NEAR(obs.terms()[0].first, 0.5, 1e-15);   // exact literal round-trip
    EXPECT_TRUE(obs.terms()[0].second == std::string{"ZZ"});
    EXPECT_NEAR(obs.terms()[1].first, 0.5, 1e-15);
    EXPECT_TRUE(obs.terms()[1].second == std::string{"XX"});
}

// add_term is chainable and appends in insertion order.
TEST(Observable, AddTerm_ChainsInOrder) {
    Observable obs;
    obs.add_term(1.0, "ZI").add_term(-2.0, "IZ");

    EXPECT_EQ(obs.size(), std::size_t{2});
    EXPECT_TRUE(obs.terms()[0].second == std::string{"ZI"});
    EXPECT_NEAR(obs.terms()[1].first, -2.0, 1e-15);  // negative coeff preserved
    EXPECT_TRUE(obs.terms()[1].second == std::string{"IZ"});
}

// ── Boundary conditions ────────────────────────────────────────

// A default-constructed observable is empty.
TEST(Observable, Default_IsEmpty) {
    Observable obs;
    EXPECT_EQ(obs.size(), std::size_t{0});
    EXPECT_TRUE(obs.terms().empty());
}

// Zero coefficient and all-identity Pauli are valid (boundary, not rejected).
TEST(Observable, ZeroCoeffAndIdentity_Accepted) {
    Observable obs = Observable::from_list({{"III", 0.0}});
    EXPECT_EQ(obs.size(), std::size_t{1});
    EXPECT_NEAR(obs.terms()[0].first, 0.0, 1e-15);
}

// ── Exception paths ────────────────────────────────────────────

// Empty Pauli string is rejected (length==num_qubits is checked later, but
// emptiness is structurally invalid regardless of circuit).
TEST(Observable, AddTerm_EmptyPauli_Throws) {
    Observable obs;
    EXPECT_THROW(obs.add_term(1.0, ""), CircuitError);
}

// A char outside I/X/Y/Z is rejected.
TEST(Observable, AddTerm_InvalidChar_Throws) {
    Observable obs;
    EXPECT_THROW(obs.add_term(1.0, "ZA"), CircuitError);
}

// from_list propagates validation: an invalid term aborts construction.
TEST(Observable, FromList_InvalidTerm_Throws) {
    EXPECT_THROW(Observable::from_list({{"ZZ", 0.5}, {"Q", 0.5}}), CircuitError);
}
