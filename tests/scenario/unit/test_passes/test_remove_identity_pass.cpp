// test_remove_identity_pass.cpp — Unit tests for RemoveIdentityPass.
// Verifies that GateType::I gates are removed and all other ops pass through.

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/circuit/gate_op.hpp"
#include "../../../../include/transpiler/passes/optimization/remove_identity_pass.hpp"

using qplayer::Circuit;
using qplayer::RemoveIdentityPass;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::Op;
using qplayer::detail_circuit_push_op;

// ── Case 1: two I gates removed ──────────────────────────────────────────────

TEST(RemoveIdentityPass, TwoIdentityGates_BothRemoved) {
    // Arrange
    Circuit c(1);
    c.id(0).id(0);

    // Act
    RemoveIdentityPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert
    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_TRUE(pass.changed());
}

// ── Case 2: non-identity gates pass through unchanged ─────────────────────────

TEST(RemoveIdentityPass, NonIdentityGates_NotRemoved) {
    // Arrange
    Circuit c(2);
    c.h(0).cx(0, 1);

    // Act
    RemoveIdentityPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert — 2 ops remain untouched
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_FALSE(pass.changed());
}

// ── Case 3: I between non-I gates — only I removed ───────────────────────────

TEST(RemoveIdentityPass, IdentityBetweenGates_OnlyIdentityRemoved) {
    // Arrange: H·I·H  →  H·H
    Circuit c(1);
    c.h(0).id(0).h(0);

    // Act
    RemoveIdentityPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::H);
    EXPECT_TRUE(std::get<GateOp>(out.ops()[1]).type == GateType::H);
}

// ── Case 4: Barrier and Measure pass through, only I removed ─────────────────

TEST(RemoveIdentityPass, Boundary_BarrierAndMeasurePassThrough) {
    // Arrange: I(0) · barrier · measure(0,0)
    Circuit c(1, 1);
    c.id(0).barrier({0}).measure(0, 0);

    // Act
    RemoveIdentityPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert — I removed, barrier + measure remain (2 ops)
    EXPECT_EQ(out.num_ops(), std::size_t{2});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::holds_alternative<qplayer::BarrierOp>(out.ops()[0]));
    EXPECT_TRUE(std::holds_alternative<qplayer::MeasureOp>(out.ops()[1]));
}

// ── Case 5: changed() true when I removed, false when no I gate ──────────────

TEST(RemoveIdentityPass, ChangedFlag_TrueOnlyWhenIdentityRemoved) {
    // No identity
    {
        Circuit c(1);
        c.x(0).y(0);
        RemoveIdentityPass pass;
        pass.run(std::move(c));
        EXPECT_FALSE(pass.changed());
    }
    // With identity
    {
        Circuit c(1);
        c.id(0);
        RemoveIdentityPass pass;
        pass.run(std::move(c));
        EXPECT_TRUE(pass.changed());
    }
}

// ── Case 6: name() returns expected string ───────────────────────────────────

TEST(RemoveIdentityPass, Name_ReturnsExpectedString) {
    RemoveIdentityPass pass;
    EXPECT_EQ(pass.name(), std::string{"RemoveIdentityPass"});
}

// ── Case 7: multi-qubit circuit — I on each qubit removed ────────────────────

TEST(RemoveIdentityPass, MultiQubit_AllIdentitiesRemoved) {
    // Arrange: id(0) · id(1) · cx(0,1)
    Circuit c(2);
    c.id(0).id(1).cx(0, 1);

    // Act
    RemoveIdentityPass pass;
    Circuit out = pass.run(std::move(c));

    // Assert — 2 identities removed, CX remains
    EXPECT_EQ(out.num_ops(), std::size_t{1});
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(std::get<GateOp>(out.ops()[0]).type == GateType::CX);
}

// ── Case 8: empty circuit — no crash, changed false ──────────────────────────

TEST(RemoveIdentityPass, EmptyCircuit_NoCrashChangedFalse) {
    Circuit c(2);
    RemoveIdentityPass pass;
    Circuit out = pass.run(std::move(c));
    EXPECT_EQ(out.num_ops(), std::size_t{0});
    EXPECT_FALSE(pass.changed());
}
