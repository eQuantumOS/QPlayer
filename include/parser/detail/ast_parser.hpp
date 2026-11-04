/// @file ast_parser.hpp
/// @brief Recursive-descent parser for OpenQASM v2.
/// @ingroup frontend
///
/// Consumes a Token stream produced by Lexer and emits a Program AST.
/// Hidden behind the Parser facade (parser.hpp); not part of the public ABI.
///
/// Error model: every parse failure throws QASMParseError stamped with
/// (file_name, line, col, message) so callers can report precise source
/// locations.
///
/// The expression sub-parser uses precedence climbing:
/// additive (+/-) > multiplicative (*/) > power (^, right-assoc)
/// > unary neg > primary (number, ident, call, parens).

#pragma once

#include "ast.hpp"
#include "lexer.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qplayer::detail {

/// Recursive-descent parser turning a token stream into the AST.
class AstParser {
public:
    // Constructs the parser from a pre-built token stream and a source label
    // used in all QASMParseError messages ("file_name" may be "<text>" for
    // in-memory strings, matching the Lexer convention).
    AstParser(std::vector<Token> tokens, std::string file_name);

    // Main entry point. Returns the fully-built Program AST or throws
    // QASMParseError on the first syntax error encountered.
    Program parse();

private:
    // ── Token stream helpers ─────────────────────────────────────────────────

    // Returns the current token without consuming it.
    const Token& peek() const;

    // Returns token at position pos_ + offset without consuming.
    const Token& peek_ahead(std::size_t offset) const;

    // Advances past the current token and returns it.
    Token advance();

    // Returns true (and advances) if the current token matches kind + text.
    bool match(TokenKind kind, const std::string& text);

    // Like match but throws QASMParseError if the token does not match.
    // Returns the consumed token so callers can extract its text/position.
    Token expect(TokenKind kind, const std::string& text);

    // Expects the current token to have kind `kind` (ignoring text).
    // Returns the consumed token.
    Token expect_kind(TokenKind kind, const std::string& what_label);

    // Unconditionally throw QASMParseError at the current token position.
    [[noreturn]] void error_at(const Token& tok, const std::string& msg) const;

    // Throw with the current peek() position.
    [[noreturn]] void error_current(const std::string& msg) const;

    // ── Top-level statement dispatch ─────────────────────────────────────────

    // Parses and returns the next statement (or throws on unexpected token).
    AstStmt parse_statement();

    // ── Statement parsers ─────────────────────────────────────────────────────

    // "OPENQASM" NUMBER ";"
    VersionDecl parse_version_decl();

    // "include" STRING ";"
    IncludeStmt parse_include();

    // "qreg" IDENT "[" NUMBER "]" ";"
    QRegDecl parse_qreg();

    // "creg" IDENT "[" NUMBER "]" ";"
    CRegDecl parse_creg();

    // "gate" IDENT [param_list] qubit_list "{" gate_body "}"
    GateDef parse_gate_def();

    // "opaque" IDENT [param_list] qubit_list ";"
    OpaqueDecl parse_opaque_decl();

    // IDENT [param_args] qubit_args ";"
    // Called for top-level gate invocations (gate statements) where the
    // leading IDENT has already been identified as a gate name.
    GateCall parse_gate_call();

    // "measure" qubit_ref "->" cbit_ref ";"
    MeasureStmt parse_measure();

    // "reset" qubit_ref ";"
    ResetStmt parse_reset();

    // "barrier" [qubit_ref_list] ";"
    BarrierStmt parse_barrier();

    // "if" "(" IDENT "==" NUMBER ")" (gate_call | measure | reset)
    IfStmt parse_if();

    // ── Sub-parsers ───────────────────────────────────────────────────────────

    // Parses a register reference: IDENT or IDENT "[" NUMBER "]".
    RegRef parse_reg_ref();

    // Parses a comma-separated list of register references.
    std::vector<RegRef> parse_reg_ref_list();

    // Parses a formal parameter name list: "(" IDENT ("," IDENT)* ")".
    // Returns an empty vector if no leading "(" is found.
    std::vector<std::string> parse_formal_param_list();

    // Parses a formal qubit name list: IDENT ("," IDENT)*.
    std::vector<std::string> parse_formal_qubit_list(
        const std::string& gate_name);

    // Parses a parameter expression list: "(" expr ("," expr)* ")".
    // Returns an empty vector if no leading "(" is found.
    std::vector<ExprPtr> parse_param_args();

    // Parses a gate body: zero or more gate calls until "}" is reached.
    std::vector<GateCall> parse_gate_body();

    // ── Expression parser (precedence climbing) ───────────────────────────────

    // Entry point: parses the lowest-precedence (additive) level.
    ExprPtr parse_expr();

    // Precedence level 1 (additive: +/-).
    ExprPtr parse_additive();

    // Precedence level 2 (multiplicative: */).
    ExprPtr parse_multiplicative();

    // Precedence level 3 (power: ^, right-associative).
    ExprPtr parse_power();

    // Precedence level 4 (unary negation).
    ExprPtr parse_unary();

    // Precedence level 5 (primary: literal, ident, call, parenthesised expr).
    ExprPtr parse_primary();

    // ── State ─────────────────────────────────────────────────────────────────

    std::vector<Token> tokens_;
    std::size_t        pos_   = 0;
    std::string        file_;
};

}  // namespace qplayer::detail
