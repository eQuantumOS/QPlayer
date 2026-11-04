// test_initial_layout.cpp — Phase 8 unit tests for InitialLayoutPass
// Covers the Trivial identity default, custom layout
// hints, coupling-map propagation into CircuitMetadata, and the rejection
// paths (wrong size, non-permutation, out-of-device-range hints).

#include "../../../framework/test_case.hpp"
#include "../../../framework/assertions.hpp"

#include <vector>

#include "../../../../include/circuit/circuit.hpp"
#include "../../../../include/error.hpp"
#include "../../../../include/noise/device_model.hpp"
#include "../../../../include/transpiler/passes/layout/initial_layout_pass.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::DeviceModel;
using qplayer::InitialLayoutPass;
using qplayer::QubitIndex;

namespace {

// A 3-qubit linear device 0-1-2 with coherence + a coupling map.
DeviceModel linear_3q() {
    return DeviceModel::from_json_string(R"({
      "qubits": [{"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}, {"T1": 1e5, "T2": 6e4}],
      "coupling_map": [[0, 1], [1, 2]]
    })");
}

}  // namespace

// ── Trivial identity default ──────────────────────────────────────────────────

TEST(InitialLayout, Default_ProducesIdentityLayout) {
    // Arrange
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1).cx(1, 2);

    // Act
    InitialLayoutPass pass(dev);
    Circuit out = pass.run(circ);

    // Assert — layout present, identity in both directions.
    EXPECT_TRUE(pass.changed());
    EXPECT_TRUE(out.metadata().layout.has_value());
    const auto& l2p = out.metadata().layout->logical_to_physical;
    EXPECT_EQ(l2p.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(l2p.at(0), static_cast<QubitIndex>(0));
    EXPECT_EQ(l2p.at(1), static_cast<QubitIndex>(1));
    EXPECT_EQ(l2p.at(2), static_cast<QubitIndex>(2));
}

TEST(InitialLayout, Default_PropagatesCouplingMap) {
    // Arrange
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);

    // Act
    InitialLayoutPass pass(dev);
    Circuit out = pass.run(circ);

    // Assert — coupling map copied into metadata for downstream SwapRouter.
    EXPECT_TRUE(out.metadata().coupling_map.has_value());
    EXPECT_EQ(out.metadata().coupling_map->size(), static_cast<std::size_t>(2));
}

TEST(InitialLayout, Default_OpsLeftUnchanged) {
    // InitialLayoutPass records the mapping but must not rewrite op indices.
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1).cx(1, 2);

    InitialLayoutPass pass(dev);
    Circuit out = pass.run(circ);

    EXPECT_EQ(out.num_ops(), circ.num_ops());
}

// ── Custom layout hints ───────────────────────────────────────────────────────

TEST(InitialLayout, CustomHints_AppliedAsPermutation) {
    // Arrange — reverse mapping logical 0->2, 1->1, 2->0.
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);
    circ.h(0).cx(0, 1);

    // Act
    InitialLayoutPass pass(dev, std::vector<QubitIndex>{2, 1, 0});
    Circuit out = pass.run(circ);

    // Assert — both directions reflect the permutation.
    EXPECT_TRUE(out.metadata().layout.has_value());
    const auto& l2p = out.metadata().layout->logical_to_physical;
    const auto& p2l = out.metadata().layout->physical_to_logical;
    EXPECT_EQ(l2p.at(0), static_cast<QubitIndex>(2));
    EXPECT_EQ(l2p.at(2), static_cast<QubitIndex>(0));
    EXPECT_EQ(p2l.at(2), static_cast<QubitIndex>(0));
    EXPECT_EQ(p2l.at(0), static_cast<QubitIndex>(2));
}

// ── Boundary: empty circuit ───────────────────────────────────────────────────

TEST(InitialLayout, ZeroQubitCircuit_EmptyLayout) {
    DeviceModel dev = linear_3q();
    Circuit circ(0, 0);

    InitialLayoutPass pass(dev);
    Circuit out = pass.run(circ);

    EXPECT_TRUE(out.metadata().layout.has_value());
    EXPECT_EQ(out.metadata().layout->logical_to_physical.size(),
              static_cast<std::size_t>(0));
}

// ── Rejection paths ───────────────────────────────────────────────────────────

TEST(InitialLayout, HintsWrongSize_ThrowsCircuitError) {
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);

    // Only 2 entries for a 3-qubit circuit.
    InitialLayoutPass pass(dev, std::vector<QubitIndex>{0, 1});
    EXPECT_THROW(pass.run(circ), CircuitError);
}

TEST(InitialLayout, HintsDuplicatePhysical_ThrowsCircuitError) {
    DeviceModel dev = linear_3q();
    Circuit circ(3, 0);

    // Physical qubit 1 used twice — not a permutation.
    InitialLayoutPass pass(dev, std::vector<QubitIndex>{0, 1, 1});
    EXPECT_THROW(pass.run(circ), CircuitError);
}

TEST(InitialLayout, HintsBeyondDeviceRange_ThrowsCircuitError) {
    DeviceModel dev = linear_3q();   // device describes physical qubits 0..2
    Circuit circ(3, 0);

    // Physical qubit 5 does not exist on the device.
    InitialLayoutPass pass(dev, std::vector<QubitIndex>{0, 1, 5});
    EXPECT_THROW(pass.run(circ), CircuitError);
}

TEST(InitialLayout, CircuitWiderThanDevice_ThrowsCircuitError) {
    DeviceModel dev = linear_3q();   // 3 physical qubits
    Circuit circ(4, 0);              // needs 4

    InitialLayoutPass pass(dev);
    EXPECT_THROW(pass.run(circ), CircuitError);
}
