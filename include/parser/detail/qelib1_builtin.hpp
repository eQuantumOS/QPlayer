// qelib1_builtin.hpp — GateType mapping for every qelib1.inc gate.
// Cross-links qelib1_signatures.hpp (num_params/num_qubits) with the GateType
// enum from gate_op.hpp. Used by CircuitBuilder to dispatch a GateCall AST node
// to the correct Circuit builder method.
//
// Design SSOT: table — 29 entries: U/CX built-in aliases
// plus 27 qelib1 gates (incl. sx/sxdg). kQelib1Signatures and
// kQelib1Mappings must stay in 1:1
// correspondence (same index order). A static_assert in qelib1_builtin.cpp
// enforces the array size match.
//
// qelib1.inc include handling: CircuitBuilder skips IncludeStmt whose path is
// "qelib1.inc" (or ends with "/qelib1.inc") as a no-op. Other paths throw
// QASMParseError (external includes are out of scope for Phase 3). The helper
// is_qelib1_inc() does the matching.

#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "circuit/gate_op.hpp"          // GateType
#include "qelib1_signatures.hpp"        // kQelib1Signatures size constant

namespace qplayer::detail {

// Pairs a QASM gate name with its Circuit IR GateType. Null-terminated
// name field matches the corresponding kQelib1Signatures entry at the same
// index position.
struct Qelib1Mapping {
    std::string_view name;
    GateType         type;
};

// 29-entry compile-time table: same order as kQelib1Signatures so index lookup
// is O(1) when the caller already has a GateSignature pointer offset.
// Built-in U maps to GateType::U3; built-in CX maps to GateType::CX — both
// are aliased at parse time per OpenQASM v2 spec.
inline constexpr std::array<Qelib1Mapping, 29> kQelib1Mappings{{
    // Built-in universal gates (must mirror kQelib1Signatures[0..1]).
    {"U",     GateType::U3},     // U(theta,phi,lambda) aliases u3
    {"CX",    GateType::CX},     // CX ctrl,tgt aliases cx

    // qelib1.inc identity.
    {"id",    GateType::I},

    // Single-qubit Pauli + Hadamard.
    {"x",     GateType::X},
    {"y",     GateType::Y},
    {"z",     GateType::Z},
    {"h",     GateType::H},

    // Single-qubit phase gates.
    {"s",     GateType::S},
    {"sdg",   GateType::Sdg},
    {"t",     GateType::T},
    {"tdg",   GateType::Tdg},
    // Single-qubit sqrt(X) gates.
    {"sx",    GateType::SX},
    {"sxdg",  GateType::SXdg},

    // Single-qubit rotation gates.
    {"rx",    GateType::RX},
    {"ry",    GateType::RY},
    {"rz",    GateType::RZ},

    // Single-qubit phase / universal variants.
    {"u1",    GateType::U1},
    {"u2",    GateType::U2},
    {"u3",    GateType::U3},

    // Two-qubit controlled-Pauli.
    {"cx",    GateType::CX},
    {"cy",    GateType::CY},
    {"cz",    GateType::CZ},

    // Two-qubit controlled-H.
    {"ch",    GateType::CH},

    // Two-qubit SWAP.
    {"swap",  GateType::SWAP},

    // Two-qubit controlled-rotation / phase.
    {"crz",   GateType::CRZ},
    {"cu1",   GateType::CU1},
    {"cu3",   GateType::CU3},

    // Three-qubit gates.
    {"ccx",   GateType::CCX},    // Toffoli
    {"cswap", GateType::CSwap},  // Fredkin
}};

// Linear search by gate name. Returns the GateType when name is in the
// standard table, or std::nullopt when the name is unknown (user-defined gate
// or opaque). O(27) — called only during QASM parsing, not simulation hot path.
inline std::optional<GateType> find_qelib1_gate_type(std::string_view name) noexcept {
    for (const auto& m : kQelib1Mappings) {
        if (m.name == name) return m.type;
    }
    return std::nullopt;
}

// Returns true when path identifies the built-in qelib1.inc include: exact
// match "qelib1.inc" or any path ending with "/qelib1.inc". Used by
// CircuitBuilder to skip the include as a no-op.
inline bool is_qelib1_inc(std::string_view path) noexcept {
    if (path == "qelib1.inc") return true;
    if (path.size() > 10 &&
        path.substr(path.size() - 10) == "qelib1.inc" &&
        path[path.size() - 11] == '/') {
        return true;
    }
    return false;
}

}  // namespace qplayer::detail
