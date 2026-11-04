// circuit_builder.hpp — AST → Circuit IR transformation.
// Consumes a validated Program AST (SemanticValidator output) and emits a
// fully-constructed Circuit object. Sits at the end of the parser pipeline:
//   Lexer → AstParser → SemanticValidator → CircuitBuilder.
//
// Design SSOT:.
//
// Error handling:
//   QASMParseError  — expression evaluation failures (unknown name, div/0,
//                     sqrt of negative, ln of non-positive) with (file,line,col).
//   CircuitError    — propagated unchanged from Circuit API violations.
//
// Inline depth limit: user-defined gate bodies are expanded recursively up to
// kMaxInlineDepth levels (5). Deeper nesting throws QASMParseError to avoid
// unbounded stack growth from mutually-recursive gate definitions that slip
// past the SemanticValidator.
//
// if-body restriction: only GateCall bodies are supported inside IfStmt at
// Phase 3. Measure or Reset as if-body throws CircuitError because the Circuit
// API's if_then() accepts only GateOp ( allows the full
// set — tracked as a post-Phase-3 open item in).

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "circuit/circuit.hpp"
#include "types.hpp"

namespace qplayer::detail {

class CircuitBuilder {
public:
    // Constructs the builder with the source label used in QASMParseError
    // messages. Pass "<text>" for in-memory strings, file path for file-backed
    // sources — must match the label used by all upstream pipeline stages.
    explicit CircuitBuilder(std::string file_name);

    // Single entry point. Transforms `program` into a Circuit IR.
    // Precondition: program passed SemanticValidator::validate() without error.
    // Throws QASMParseError or CircuitError on first failure encountered.
    qplayer::Circuit build(const Program& program);

private:
    // ── Register resolution ───────────────────────────────────────────────────

    // Tracks one quantum register: its name, global qubit offset, and size.
    // offset + index yields the global QubitIndex used by Circuit builders.
    struct QregInfo {
        std::string  name;
        QubitIndex   offset = 0;
        QubitIndex   size   = 0;
    };

    // Tracks one classical register offset for cubit global-index resolution.
    struct CregInfo {
        std::string       name;
        ClassicalBitIndex offset = 0;
        ClassicalBitIndex size   = 0;
    };

    // Resolves `ref` (a qubit RegRef with name + optional index) to a global
    // QubitIndex. Throws QASMParseError when the register or index is unknown.
    QubitIndex resolve_qubit(const RegRef& ref) const;

    // Resolves `ref` (a classical bit RegRef) to a global ClassicalBitIndex.
    ClassicalBitIndex resolve_cbit(const RegRef& ref) const;

    // Returns the ClassicalRegisterIndex for creg `name`. Throws QASMParseError
    // on name miss (should not happen after SemanticValidator, but defensive).
    ClassicalRegisterIndex resolve_creg(const std::string& name) const;

    // ── Expression evaluator ──────────────────────────────────────────────────

    // Evaluates an expression tree to a double. Parameter substitution map
    // `param_map` maps formal parameter names to their numeric values; it is
    // empty at top-level calls and populated during gate inline expansion.
    // Supported functions: sin, cos, tan, exp, ln (natural log), sqrt.
    // Supported constant: pi.
    // Operators: + - * / ^ (pow), unary -.
    // Throws QASMParseError for unknown identifiers / functions, division by
    // zero, sqrt of negative, ln of non-positive.
    double eval_expr(const ExprNode& node,
                     const std::unordered_map<std::string, double>& param_map,
                     int default_line, int default_col) const;

    // ── Statement emitters ────────────────────────────────────────────────────

    // Dispatches a single top-level statement to the appropriate emitter.
    // Skips VersionDecl, IncludeStmt, QRegDecl, CRegDecl, GateDef, OpaqueDecl
    // (handled in setup passes). Throws for IncludeStmt whose path is not
    // qelib1.inc (external includes unsupported in Phase 3).
    void emit_statement(qplayer::Circuit& c, const AstStmt& stmt,
                        int inline_depth = 0);

    // Emits a gate call. `qarg_map` translates formal qubit names to global
    // QubitIndex values when this call appears inside a user gate body during
    // inline expansion; empty map signals top-level context.
    void emit_gate_call(qplayer::Circuit& c,
                        const GateCall& call,
                        const std::unordered_map<std::string, QubitIndex>& qarg_map,
                        const std::unordered_map<std::string, double>& param_map,
                        int inline_depth);

    // Inline-expands a user-defined gate: binds actuals to formals and emits
    // each body GateCall via emit_gate_call. Respects kMaxInlineDepth.
    void inline_user_gate(qplayer::Circuit& c,
                          const GateCall& call,
                          const GateDef& def,
                          const std::unordered_map<std::string, QubitIndex>& outer_qarg_map,
                          const std::unordered_map<std::string, double>& outer_param_map,
                          int inline_depth);

    // Dispatches a built-in (qelib1) GateType to the matching Circuit method.
    // params and qubits are already evaluated / resolved before this call.
    void emit_builtin_gate(qplayer::Circuit& c,
                           GateType type,
                           const std::vector<double>& params,
                           const std::vector<QubitIndex>& qubits,
                           const std::string& name,
                           int line, int col);

    // Builds a GateOp for use as the body of an IfOp from a GateCall.
    // Requires call.name to be a built-in or opaque (user gate inline inside
    // if-body is not permitted at Phase 3 — SemanticValidator allows it but
    // CircuitBuilder rejects it with QASMParseError).
    qplayer::GateOp build_if_body_gate(
        const GateCall& call,
        const std::unordered_map<std::string, QubitIndex>& qarg_map,
        const std::unordered_map<std::string, double>& param_map);

    // ── State ─────────────────────────────────────────────────────────────────

    std::string file_;   // source label for error messages

    std::vector<QregInfo>                              qregs_;
    std::unordered_map<std::string, const QregInfo*>   qreg_by_name_;

    std::vector<CregInfo>                              cregs_;
    std::unordered_map<std::string, const CregInfo*>   creg_by_name_;

    // creg name → ClassicalRegisterIndex as assigned by Circuit::add_creg().
    std::unordered_map<std::string, ClassicalRegisterIndex> creg_idx_;

    // User-defined gates declared by GateDef nodes.
    std::unordered_map<std::string, const GateDef*>    user_gates_;

    // Opaque declarations (name → OpaqueDecl from AST).
    std::unordered_map<std::string, const OpaqueDecl*> opaques_;

    // Maximum recursion depth for gate inline expansion. Stack overflows from
    // deeply-recursive user gate definitions are prevented by this limit.
    static constexpr int kMaxInlineDepth = 5;
};

}  // namespace qplayer::detail
