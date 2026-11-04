// qelib1_signatures.hpp — Compile-time table of OpenQASM v2 standard gate
// signatures. This header is the SSOT shared by
// SemanticValidator and qelib1_builtin.cpp. Keeps (name,
// num_params, num_qubits) for all 25 qelib1 gates plus the 2 built-in aliases
// U/CX. The GateType enum mapping added by extends this file; the
// signature table itself is immutable after ships.
//
// Design:. The 27-entry array covers every gate accepted by
// a conforming OpenQASM v2 parser. Non-standard extensions (u0, rxx, ...) are
// rejected with QASMParseError per spec — see SemanticValidator::validate_gate_call.

#pragma once

#include <array>
#include <string_view>

namespace qplayer::detail {

// Carries just the three fields required for argument-count validation.
// adds a GateType enum field to a separate mapping structure; this struct
// stays stable so the SemanticValidator never needs rebuilding when lands.
struct GateSignature {
    std::string_view name;
    int              num_params = 0;
    int              num_qubits = 0;
};

// All 27 standard entries: 2 built-in (U, CX) + 25 qelib1 gates.
// Order mirrors the table in for review convenience.
// Linear scan is sufficient — 27 entries, called at parse time only.
inline constexpr std::array<GateSignature, 29> kQelib1Signatures{{
    // Built-in universal gates (OpenQASM v2 spec).
    {"U",     3, 1},   // U(theta, phi, lambda) q
    {"CX",    0, 2},   // CX ctrl, tgt

    // qelib1.inc identity.
    {"id",    0, 1},

    // Single-qubit Pauli + Hadamard.
    {"x",     0, 1},
    {"y",     0, 1},
    {"z",     0, 1},
    {"h",     0, 1},

    // Single-qubit phase gates.
    {"s",     0, 1},
    {"sdg",   0, 1},
    {"t",     0, 1},
    {"tdg",   0, 1},
    // Single-qubit sqrt(X) gates.
    {"sx",    0, 1},
    {"sxdg",  0, 1},

    // Single-qubit rotation gates.
    {"rx",    1, 1},
    {"ry",    1, 1},
    {"rz",    1, 1},

    // Single-qubit phase / universal variants.
    {"u1",    1, 1},
    {"u2",    2, 1},
    {"u3",    3, 1},

    // Two-qubit controlled-Pauli.
    {"cx",    0, 2},
    {"cy",    0, 2},
    {"cz",    0, 2},

    // Two-qubit controlled-H.
    {"ch",    0, 2},

    // Two-qubit SWAP.
    {"swap",  0, 2},

    // Two-qubit controlled-rotation / phase.
    {"crz",   1, 2},
    {"cu1",   1, 2},
    {"cu3",   3, 2},

    // Three-qubit gates.
    {"ccx",   0, 3},   // Toffoli
    {"cswap", 0, 3},   // Fredkin
}};

// Linear search by name; returns nullptr when name is not in the table.
// Callers: SemanticValidator, qelib1_builtin.cpp.
inline const GateSignature* find_qelib1_signature(std::string_view name) noexcept {
    for (const auto& sig : kQelib1Signatures) {
        if (sig.name == name) return &sig;
    }
    return nullptr;
}

}  // namespace qplayer::detail
