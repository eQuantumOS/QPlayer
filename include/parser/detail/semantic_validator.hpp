// semantic_validator.hpp — AST-level semantic validation for OpenQASM v2
// Sits between the AstParser and CircuitBuilder
// in the pipeline: Lexer → AstParser → SemanticValidator → CircuitBuilder.
//
// Responsibilities (SSOT):
//   • Symbol table: duplicate qreg/creg/gate/opaque names (gate + opaque share
//     one namespace — collision forbidden).
//   • Register-index range: q[i] requires i < qreg.size, c[j] requires j < creg.size.
//   • Argument counts: gate calls must match the declared (#params, #qubits).
//   • if-condition register type: only creg is legal as condition operand.
//   • if-condition value range: 0 ≤ N < 2^creg.width.
//   • gate_def body: formal qubit / param names only; direct qreg indexing forbidden.
//
// NOT in scope (deferred to other pipeline stages):
// • opaque definition-existence check → IR-level SemanticValidatorPass.
// • Expression evaluation (pi, sin, cos,...) → evaluator.
// • `include` path resolution → qelib1_builtin.cpp.
//
// Error model: the first violation throws qplayer::QASMParseError stamped with
// (file, line, col, message) — same as AstParser.

#pragma once

#include "ast.hpp"
#include "qelib1_signatures.hpp"

#include <set>
#include <string>
#include <unordered_map>

namespace qplayer::detail {

class SemanticValidator {
public:
    // Constructs the validator with the source label used in error messages.
    // Pass "<text>" for in-memory strings, file path for file-backed sources —
    // must match the label used by Lexer / AstParser for consistent messages.
    explicit SemanticValidator(std::string file_name);

    // Single entry point. Walks every statement in `program` in source order
    // (one-pass, no forward references per OpenQASM v2 spec).
    // Throws qplayer::QASMParseError on the first semantic violation.
    void validate(const Program& program);

private:
    // ── Registration helpers (throw on duplicate) ────────────────────────────

    void register_qreg(const QRegDecl& decl);
    void register_creg(const CRegDecl& decl);
    // Both GateDef and OpaqueDecl share the gate namespace; these helpers
    // insert into user_gates_ / opaques_ respectively after collision check.
    void register_gate_def(const GateDef& def);
    void register_opaque(const OpaqueDecl& decl);

    // ── Statement dispatchers ─────────────────────────────────────────────────

    void validate_statement(const AstStmt& stmt);

    // Top-level gate call (not inside a gate_def body).
    void validate_gate_call_toplevel(const GateCall& call);

    // Gate call inside a gate_def body. `formal_qubits` holds the set of
    // formal qubit parameter names bound in the enclosing gate_def; RegRef
    // names must appear in this set and must NOT carry an index subscript.
    // `formal_params` holds formal parameter names for expression context.
    void validate_gate_call_in_body(const GateCall& call,
                                    const std::set<std::string>& formal_qubits,
                                    const std::set<std::string>& formal_params);

    void validate_measure(const MeasureStmt& stmt);
    void validate_reset(const ResetStmt& stmt);
    void validate_barrier(const BarrierStmt& stmt);
    void validate_if(const IfStmt& stmt);
    void validate_gate_def_body(const GateDef& def);

    // ── Register-reference helpers ────────────────────────────────────────────

    // Validates a register reference at top level (non-body context).
    //   is_qubit=true  → must be declared in qregs_, index < qreg.size.
    //   is_qubit=false → must be declared in cregs_, index < creg.size.
    // Absent index (whole-register reference) is allowed only for barrier.
    // `allow_whole` controls whether index-less references are accepted.
    void validate_reg_ref_toplevel(const RegRef& ref, bool is_qubit,
                                   bool allow_whole = false);

    // Validates a register reference inside a gate_def body.
    // The name must appear in `formal_qubits`; index must be absent (RegRef
    // inside a gate body never carries a concrete subscript per QASM v2 spec).
    void validate_reg_ref_in_body(const RegRef& ref,
                                  const std::set<std::string>& formal_qubits);

    // ── Gate-signature lookup ─────────────────────────────────────────────────

    // Looks up (num_params, num_qubits) for `name` in precedence order:
    //   1. user_gates_  (user-defined gate or redefinition shadows builtins)
    //   2. opaques_     (opaque declarations)
    //   3. kQelib1Signatures (standard library)
    // Returns nullptr when the gate is completely unknown.
    const GateSignature* lookup_gate(const std::string& name) const;

    // ── State ─────────────────────────────────────────────────────────────────

    std::string file_;   // source label for error messages

    // Register symbol tables: name → size (number of bits/qubits).
    std::unordered_map<std::string, int> qregs_;
    std::unordered_map<std::string, int> cregs_;

    // User-defined gates and opaques — both stored as GateSignature for uniform
    // argument-count lookup. The two maps are kept separate so CircuitBuilder
    // can distinguish gate-def inline expansion from opaque placeholders.
    std::unordered_map<std::string, GateSignature> user_gates_;
    std::unordered_map<std::string, GateSignature> opaques_;
};

}  // namespace qplayer::detail
