/// @file ast.hpp
/// @brief Abstract Syntax Tree node types for the OpenQASM v2 parser.
/// @ingroup frontend
///
/// Hidden behind the Parser facade (parser.hpp); never part of the public ABI.
/// All parser pipeline components that need AST types include this header.
///
/// Design:. Each node carries (line, col) position fields
/// so downstream components (SemanticValidator, CircuitBuilder) can emit
/// QASMParseError with accurate source locations.
///
/// ExprNode uses a recursive variant structure. Evaluation of symbolic constants
/// (pi, sin, cos,...) is deferred to the expression evaluator; the
/// parser preserves them as IdentExpr / CallExpr unchanged.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace qplayer::detail {

// ── Expression nodes ─────────────────────────────────────────────────────────

// Forward declaration required because ExprNode variants reference ExprPtr.
struct ExprNode;
/// Shared handle to an expression node, letting ExprNode nest recursively.
using ExprPtr = std::shared_ptr<ExprNode>;

/// Binary operators: +  -  *  /  ^ (right-associative power).
enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Pow,
};

/// Number literal: the raw text from the lexer is preserved for lossless
/// conversion. Callers can std::stod(text) or leave it symbolic for.
struct NumberLit {
    std::string text;   // lexeme as scanned (e.g. "2.0", "1.5e-3")
    int line = 0;
    int col  = 0;
};

/// Identifier expression: a plain name, e.g. "pi", "theta", "x".
/// resolves known constants (pi, euler) and gate parameters.
struct IdentExpr {
    std::string name;
    int line = 0;
    int col  = 0;
};

/// Binary arithmetic node: left OP right.
struct BinaryExpr {
    BinaryOp op;
    ExprPtr  left;
    ExprPtr  right;
    int line = 0;
    int col  = 0;
};

/// Unary negation: -expr.
struct UnaryNegExpr {
    ExprPtr operand;
    int line = 0;
    int col  = 0;
};

/// Function call: name(arg0, arg1, ...). Covers sin/cos/exp/ln/sqrt and any
/// future user-defined expression functions. evaluates known functions.
struct CallExpr {
    std::string          name;
    std::vector<ExprPtr> args;
    int line = 0;
    int col  = 0;
};

/// ExprNode is the sum type of all five expression variants.
struct ExprNode {
    std::variant<NumberLit, IdentExpr, BinaryExpr, UnaryNegExpr, CallExpr> v;
};

// ── Register reference ────────────────────────────────────────────────────────

/// Represents a qubit or classical-bit reference: `name` or `name[index]`.
/// index is absent for whole-register references (e.g. barrier or gate body
/// formal parameter lists that use register names without subscripts).
struct RegRef {
    std::string      name;
    std::optional<int> index;   // present only when `[n]` appears in source
    int line = 0;
    int col  = 0;
};

// ── Statement nodes ───────────────────────────────────────────────────────────

/// OPENQASM \<version\> ;
struct VersionDecl {
    std::string version;   // text of the version token, e.g. "2.0"
    int line = 0;
    int col  = 0;
};

/// include "\<filename\>" ;
/// File resolution is deferred to (qelib1.inc handled via built-in table,
/// other files via filesystem). The parser just records the include path.
struct IncludeStmt {
    std::string path;      // filename text without quotes
    int line = 0;
    int col  = 0;
};

/// qreg \<name\>[\<size\>] ;
struct QRegDecl {
    std::string name;
    int         size = 0;
    int line = 0;
    int col  = 0;
};

/// creg \<name\>[\<size\>] ;
struct CRegDecl {
    std::string name;
    int         size = 0;
    int line = 0;
    int col  = 0;
};

/// A single gate invocation inside a gate body or at the top level.
/// params: parameter expression list (may be empty).
/// qargs:  qubit argument list (always non-empty in valid QASM).
struct GateCall {
    std::string           name;
    std::vector<ExprPtr>  params;
    std::vector<RegRef>   qargs;
    int line = 0;
    int col  = 0;
};

/// measure \<qubit_ref\> -> \<cbit_ref\> ;
struct MeasureStmt {
    RegRef qubit;
    RegRef cbit;
    int line = 0;
    int col  = 0;
};

/// reset \<qubit_ref\> ;
struct ResetStmt {
    RegRef qubit;
    int line = 0;
    int col  = 0;
};

/// barrier \<qubit_ref_list\> ;
/// qargs may be empty, which means "barrier on all qubits" in QASM v2 semantics.
struct BarrierStmt {
    std::vector<RegRef> qargs;
    int line = 0;
    int col  = 0;
};

// Forward declaration so IfStmt can hold a unique_ptr to an AstStmt.
struct AstStmt;

/// if ( \<creg_name\> == \<value\> ) \<body\>
/// body must be a gate_stmt, measure, or reset (parser enforces this).
struct IfStmt {
    std::string                  creg_name;
    int                          value = 0;   // RHS integer literal
    std::unique_ptr<AstStmt>     body;
    int line = 0;
    int col  = 0;
};

/// opaque \<name\> [(\<param_names\>)] \<qubit_names\> ;
/// The parser records formal parameter names and qubit names; CircuitBuilder
/// maps this to Circuit::declare_opaque(name, num_qubits).
struct OpaqueDecl {
    std::string              name;
    std::vector<std::string> params;   // formal parameter names (may be empty)
    std::vector<std::string> qargs;    // formal qubit names
    int line = 0;
    int col  = 0;
};

/// gate \<name\> [(\<param_names\>)] \<qubit_names\> { \<body\> }
/// body: zero or more GateCall nodes (the gate body defines a unitary).
struct GateDef {
    std::string              name;
    std::vector<std::string> params;   // formal parameter names (may be empty)
    std::vector<std::string> qargs;    // formal qubit names
    std::vector<GateCall>    body;
    int line = 0;
    int col  = 0;
};

// ── AstStmt sum type ──────────────────────────────────────────────────────────

/// All eleven top-level statement kinds that can appear in an OpenQASM v2 program.
struct AstStmt {
    std::variant<
        VersionDecl,
        IncludeStmt,
        QRegDecl,
        CRegDecl,
        GateDef,
        GateCall,
        MeasureStmt,
        ResetStmt,
        BarrierStmt,
        IfStmt,
        OpaqueDecl
    > v;
};

// ── Top-level program ─────────────────────────────────────────────────────────

/// Root node: one parsed OpenQASM v2 translation unit.
struct Program {
    std::vector<AstStmt> statements;
};

}  // namespace qplayer::detail
