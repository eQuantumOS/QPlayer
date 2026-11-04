// test_circuit_metadata.cpp -- Phase 2 unit tests for CircuitMetadata,
// QubitLayout, make_identity_layout, and is_layout_consistent
// These verify the default
// path: even without a device, transpile() must produce a populated identity
// mapping so measurements stay logical-indexed.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <unordered_map>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/metadata.hpp"

using qplayer::Circuit;
using qplayer::CircuitMetadata;
using qplayer::CouplingMap;
using qplayer::is_layout_consistent;
using qplayer::make_identity_layout;
using qplayer::QubitIndex;
using qplayer::QubitLayout;

// ── Default-constructed QubitLayout / CircuitMetadata ────────────────────────

TEST(CircuitMetadata, QubitLayout_DefaultIsEmpty) {
    QubitLayout layout{};
    EXPECT_EQ(layout.logical_to_physical.size(), static_cast<std::size_t>(0));
    EXPECT_EQ(layout.physical_to_logical.size(), static_cast<std::size_t>(0));
}

TEST(CircuitMetadata, CircuitMetadata_DefaultAllOptionalsEmpty) {
    CircuitMetadata meta{};
    EXPECT_TRUE(meta.name.empty());
    EXPECT_FALSE(meta.layout.has_value());
    EXPECT_FALSE(meta.coupling_map.has_value());
    EXPECT_FALSE(meta.device_model_ref.has_value());
}

TEST(CircuitMetadata, Circuit_FreshlyConstructedExposesEmptyMetadata) {
    // Circuit::metadata() should return a default CircuitMetadata when no
    // transpile() has run -- left metadata_ default-constructed.
    Circuit c(3, 0);
    const auto& meta = c.metadata();
    EXPECT_TRUE(meta.name.empty());
    EXPECT_FALSE(meta.layout.has_value());
    EXPECT_FALSE(meta.coupling_map.has_value());
    EXPECT_FALSE(meta.device_model_ref.has_value());
}

// ── make_identity_layout: identity mapping (default path) ───

TEST(CircuitMetadata, MakeIdentityLayout_ZeroQubits_ReturnsEmptyMaps) {
    QubitLayout layout = make_identity_layout(0);
    EXPECT_EQ(layout.logical_to_physical.size(), static_cast<std::size_t>(0));
    EXPECT_EQ(layout.physical_to_logical.size(), static_cast<std::size_t>(0));
}

TEST(CircuitMetadata, MakeIdentityLayout_NQubits_MapsIToI) {
    constexpr QubitIndex kN = 5;
    QubitLayout layout = make_identity_layout(kN);

    EXPECT_EQ(layout.logical_to_physical.size(), static_cast<std::size_t>(kN));
    EXPECT_EQ(layout.physical_to_logical.size(), static_cast<std::size_t>(kN));

    for (QubitIndex i = 0; i < kN; ++i) {
        const auto fwd = layout.logical_to_physical.find(i);
        const auto rev = layout.physical_to_logical.find(i);
        EXPECT_TRUE(fwd != layout.logical_to_physical.end());
        EXPECT_TRUE(rev != layout.physical_to_logical.end());
        if (fwd != layout.logical_to_physical.end()) EXPECT_EQ(fwd->second, i);
        if (rev != layout.physical_to_logical.end()) EXPECT_EQ(rev->second, i);
    }
}

// ── is_layout_consistent: mutual inverse invariant (SwapRouterPass guard) ───

TEST(CircuitMetadata, IsLayoutConsistent_IdentityIsConsistent) {
    QubitLayout layout = make_identity_layout(4);
    EXPECT_TRUE(is_layout_consistent(layout));
}

TEST(CircuitMetadata, IsLayoutConsistent_AfterSymmetricSwapStaysConsistent) {
    // Simulate a SWAP(0, 1) applied to BOTH directions of the mapping --
    // SwapRouterPass must always update logical_to_physical AND
    // physical_to_logical in lockstep.
    QubitLayout layout = make_identity_layout(3);

    // Before: logical 0->phys 0, logical 1->phys 1. After SWAP swap:
    layout.logical_to_physical[0] = 1;
    layout.logical_to_physical[1] = 0;
    layout.physical_to_logical[0] = 1;
    layout.physical_to_logical[1] = 0;

    EXPECT_TRUE(is_layout_consistent(layout));
}

TEST(CircuitMetadata, IsLayoutConsistent_AsymmetricUpdateBreaksInvariant) {
    // Only logical_to_physical updated -- the reverse map still says
    // physical 0 -> logical 0, but forward says logical 0 -> physical 1.
    QubitLayout layout = make_identity_layout(3);
    layout.logical_to_physical[0] = 1;
    // physical_to_logical untouched -- inconsistent.

    EXPECT_FALSE(is_layout_consistent(layout));
}

TEST(CircuitMetadata, IsLayoutConsistent_MapSizeMismatchIsRejected) {
    QubitLayout layout = make_identity_layout(2);
    layout.logical_to_physical.emplace(99u, 99u);  // forward only.
    EXPECT_FALSE(is_layout_consistent(layout));
}

// ── CircuitMetadata sidecar fields work as plain optionals ──────────────────

TEST(CircuitMetadata, OptionalFields_RoundTrip) {
    CircuitMetadata meta{};

    meta.name             = "bell_pair";
    meta.layout           = make_identity_layout(2);
    meta.coupling_map     = CouplingMap{{0, 1}};
    meta.device_model_ref = std::string{"devices/example.json"};

    EXPECT_EQ(meta.name, std::string{"bell_pair"});
    EXPECT_TRUE(meta.layout.has_value());
    EXPECT_TRUE(meta.coupling_map.has_value());
    EXPECT_TRUE(meta.device_model_ref.has_value());

    if (meta.layout.has_value()) {
        EXPECT_EQ(meta.layout->logical_to_physical.size(), static_cast<std::size_t>(2));
    }
    if (meta.coupling_map.has_value()) {
        EXPECT_EQ(meta.coupling_map->size(), static_cast<std::size_t>(1));
        EXPECT_EQ(meta.coupling_map->at(0).first,  static_cast<QubitIndex>(0));
        EXPECT_EQ(meta.coupling_map->at(0).second, static_cast<QubitIndex>(1));
    }
    if (meta.device_model_ref.has_value()) {
        EXPECT_EQ(*meta.device_model_ref, std::string{"devices/example.json"});
    }
}
