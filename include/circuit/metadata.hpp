// metadata.hpp — Circuit-level annotations carried alongside the Op stream
// Materialised by InitialLayoutPass + SwapRouterPass and
// inspected by IdleNoiseInserterPass when T1/T2 data is present.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "types.hpp"
#include "noise/device_model.hpp"   // CouplingMap

namespace qplayer {

// Logical-to-physical qubit mapping after compilation.
// Identity layout is the default — Phase 4 InitialLayoutPass fills the maps
// once SwapRouter runs against a device. requires that even the
// device-independent transpile() path (full-connectivity) produces an
// identity mapping so measurement results stay logical-indexed.
struct QubitLayout {
    std::unordered_map<QubitIndex, QubitIndex> logical_to_physical;
    std::unordered_map<QubitIndex, QubitIndex> physical_to_logical;
};

// Sidecar metadata bag for Circuit. layout / coupling_map populate when
// transpile() runs against a device_json; device_model_ref is a free-form
// breadcrumb so JSON round-trip can keep the original path.
struct CircuitMetadata {
    std::string                 name;
    std::optional<QubitLayout>  layout;
    std::optional<CouplingMap>  coupling_map;
    std::optional<std::string>  device_model_ref;
    // After QubitTruncationPass: dense qubit index in the truncated
    // circuit -> original physical qubit. Lets noise synthesis fetch
    // device T1/T2/error by physical qubit even though the circuit now runs in
    // dense [0, k) space. Absent when no truncation occurred (dense == physical).
    // Transient transpile artifact — intentionally not JSON-persisted.
    std::optional<std::unordered_map<QubitIndex, QubitIndex>> qubit_truncation;
};

// Build an identity QubitLayout for `num_qubits` qubits — logical i maps to
// physical i in both directions. Used by InitialLayoutPass when no device
// JSON is supplied so downstream passes (SwapRouter, measurement)
// always read from a populated mapping. Header-inline because every
// caller already includes metadata.hpp and the body is a single short loop.
inline QubitLayout make_identity_layout(QubitIndex num_qubits) {
    QubitLayout layout;
    layout.logical_to_physical.reserve(num_qubits);
    layout.physical_to_logical.reserve(num_qubits);
    for (QubitIndex i = 0; i < num_qubits; ++i) {
        layout.logical_to_physical.emplace(i, i);
        layout.physical_to_logical.emplace(i, i);
    }
    return layout;
}

// Returns true when logical_to_physical and physical_to_logical are mutual
// inverses (forall l: phys_to_log[log_to_phys[l]] == l) and both maps have
// the same size. Used by SwapRouterPass after each SWAP insertion to assert
// that the bookkeeping stayed consistent — a violation indicates a SWAP
// applied to only one direction of the mapping.
inline bool is_layout_consistent(const QubitLayout& layout) {
    if (layout.logical_to_physical.size() != layout.physical_to_logical.size()) {
        return false;
    }
    for (const auto& [logical, physical] : layout.logical_to_physical) {
        const auto it = layout.physical_to_logical.find(physical);
        if (it == layout.physical_to_logical.end() || it->second != logical) {
            return false;
        }
    }
    return true;
}

}  // namespace qplayer
