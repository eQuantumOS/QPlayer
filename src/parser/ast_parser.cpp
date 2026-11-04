// ast_parser.cpp — Recursive-descent parser for OpenQASM v2.
//
// Grammar reference:.
// Error model: every error throws QASMParseError with (file, line, col, msg).
// No external parsing libraries are used.
//
// Expression parsing uses precedence climbing (highest-to-lowest):
//   primary → unary → power (right-assoc) → multiplicative → additive
// `pi`, `sin`, `cos`, etc. are preserved as IdentExpr / CallExpr so the
// expression evaluator in can fold them at parameter resolution time.

#include "parser/detail/ast_parser.hpp"
#include "error.hpp"

#include <cassert>
#include <cstdlib>   // std::stoi
#include <sstream>

namespace qplayer::detail {

// ── Constructor ──────────────────────────────────────────────────────────────

AstParser::AstParser(std::vector<Token> tokens, std::string file_name)
    : tokens_(std::move(tokens)), file_(std::move(file_name)) {}

// ── Public entry point ────────────────────────────────────────────────────────

Program AstParser::parse() {
    Program prog;
    // Drive the top-level loop until EndOfFile is the current token.
    while (peek().kind != TokenKind::EndOfFile) {
        prog.statements.push_back(parse_statement());
    }
    return prog;
}

// ── Token stream helpers ──────────────────────────────────────────────────────

const Token& AstParser::peek() const {
    // The last token in tokens_ is always EndOfFile (guaranteed by Lexer).
    if (pos_ >= tokens_.size()) {
        return tokens_.back();  // EndOfFile sentinel
    }
    return tokens_[pos_];
}

const Token& AstParser::peek_ahead(std::size_t offset) const {
    std::size_t idx = pos_ + offset;
    if (idx >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[idx];
}

Token AstParser::advance() {
    Token t = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) {
        ++pos_;
    }
    return t;
}

bool AstParser::match(TokenKind kind, const std::string& text) {
    const Token& cur = peek();
    if (cur.kind == kind && cur.text == text) {
        advance();
        return true;
    }
    return false;
}

Token AstParser::expect(TokenKind kind, const std::string& text) {
    const Token& cur = peek();
    if (cur.kind != kind || cur.text != text) {
        std::ostringstream msg;
        msg << "expected '" << text << "' but got '"
            << (cur.kind == TokenKind::EndOfFile ? "<EOF>" : cur.text) << "'";
        error_at(cur, msg.str());
    }
    return advance();
}

Token AstParser::expect_kind(TokenKind kind, const std::string& what_label) {
    const Token& cur = peek();
    if (cur.kind != kind) {
        std::ostringstream msg;
        msg << "expected " << what_label << " but got '"
            << (cur.kind == TokenKind::EndOfFile ? "<EOF>" : cur.text) << "'";
        error_at(cur, msg.str());
    }
    return advance();
}

void AstParser::error_at(const Token& tok, const std::string& msg) const {
    throw QASMParseError{file_, tok.line, tok.col, msg};
}

void AstParser::error_current(const std::string& msg) const {
    error_at(peek(), msg);
}

// ── Top-level statement dispatch ─────────────────────────────────────────────

AstStmt AstParser::parse_statement() {
    const Token& cur = peek();

    if (cur.kind == TokenKind::Keyword) {
        if (cur.text == "OPENQASM") {
            return AstStmt{parse_version_decl()};
        }
        if (cur.text == "include") {
            return AstStmt{parse_include()};
        }
        if (cur.text == "qreg") {
            return AstStmt{parse_qreg()};
        }
        if (cur.text == "creg") {
            return AstStmt{parse_creg()};
        }
        if (cur.text == "gate") {
            return AstStmt{parse_gate_def()};
        }
        if (cur.text == "opaque") {
            return AstStmt{parse_opaque_decl()};
        }
        if (cur.text == "measure") {
            return AstStmt{parse_measure()};
        }
        if (cur.text == "reset") {
            return AstStmt{parse_reset()};
        }
        if (cur.text == "barrier") {
            return AstStmt{parse_barrier()};
        }
        if (cur.text == "if") {
            return AstStmt{parse_if()};
        }
        // Unknown keyword at statement level.
        std::ostringstream msg;
        msg << "unexpected keyword '" << cur.text << "' at statement level";
        error_at(cur, msg.str());
    }

    if (cur.kind == TokenKind::Ident) {
        // An identifier at statement level is a gate call.
        return AstStmt{parse_gate_call()};
    }

    // Anything else (number, string, symbol, EOF) is a parse error.
    std::ostringstream msg;
    if (cur.kind == TokenKind::EndOfFile) {
        msg << "unexpected end of file";
    } else {
        msg << "unexpected token '" << cur.text << "' at statement level";
    }
    error_at(cur, msg.str());
}

// ── Statement parsers ─────────────────────────────────────────────────────────

VersionDecl AstParser::parse_version_decl() {
    Token kw = expect(TokenKind::Keyword, "OPENQASM");
    Token ver = expect_kind(TokenKind::Number, "version number");
    expect(TokenKind::Symbol, ";");
    return VersionDecl{ver.text, kw.line, kw.col};
}

IncludeStmt AstParser::parse_include() {
    Token kw = expect(TokenKind::Keyword, "include");
    Token path = expect_kind(TokenKind::String, "include path string");
    expect(TokenKind::Symbol, ";");
    return IncludeStmt{path.text, kw.line, kw.col};
}

QRegDecl AstParser::parse_qreg() {
    Token kw = expect(TokenKind::Keyword, "qreg");
    Token name = expect_kind(TokenKind::Ident, "register name");
    expect(TokenKind::Symbol, "[");
    Token size_tok = expect_kind(TokenKind::Number, "register size");
    expect(TokenKind::Symbol, "]");
    expect(TokenKind::Symbol, ";");

    int sz = std::stoi(size_tok.text);
    if (sz <= 0) {
        error_at(size_tok, "qreg size must be a positive integer");
    }
    return QRegDecl{name.text, sz, kw.line, kw.col};
}

CRegDecl AstParser::parse_creg() {
    Token kw = expect(TokenKind::Keyword, "creg");
    Token name = expect_kind(TokenKind::Ident, "register name");
    expect(TokenKind::Symbol, "[");
    Token size_tok = expect_kind(TokenKind::Number, "register size");
    expect(TokenKind::Symbol, "]");
    expect(TokenKind::Symbol, ";");

    int sz = std::stoi(size_tok.text);
    if (sz <= 0) {
        error_at(size_tok, "creg size must be a positive integer");
    }
    return CRegDecl{name.text, sz, kw.line, kw.col};
}

GateDef AstParser::parse_gate_def() {
    Token kw   = expect(TokenKind::Keyword, "gate");
    Token name = expect_kind(TokenKind::Ident, "gate name");

    auto params = parse_formal_param_list();
    auto qargs  = parse_formal_qubit_list(name.text);

    expect(TokenKind::Symbol, "{");
    auto body = parse_gate_body();
    expect(TokenKind::Symbol, "}");

    return GateDef{name.text, std::move(params), std::move(qargs),
                   std::move(body), kw.line, kw.col};
}

OpaqueDecl AstParser::parse_opaque_decl() {
    Token kw   = expect(TokenKind::Keyword, "opaque");
    Token name = expect_kind(TokenKind::Ident, "opaque gate name");

    auto params = parse_formal_param_list();
    auto qargs  = parse_formal_qubit_list(name.text);

    expect(TokenKind::Symbol, ";");

    return OpaqueDecl{name.text, std::move(params), std::move(qargs),
                      kw.line, kw.col};
}

GateCall AstParser::parse_gate_call() {
    // Caller guarantees current token is an IDENT (gate name).
    Token name = expect_kind(TokenKind::Ident, "gate name");
    auto params = parse_param_args();
    auto qargs  = parse_reg_ref_list();

    if (qargs.empty()) {
        error_at(name, "gate call '" + name.text + "' has no qubit arguments");
    }

    expect(TokenKind::Symbol, ";");
    return GateCall{name.text, std::move(params), std::move(qargs),
                    name.line, name.col};
}

MeasureStmt AstParser::parse_measure() {
    Token kw = expect(TokenKind::Keyword, "measure");
    RegRef qubit = parse_reg_ref();
    expect(TokenKind::Symbol, "->");
    RegRef cbit  = parse_reg_ref();
    expect(TokenKind::Symbol, ";");
    return MeasureStmt{qubit, cbit, kw.line, kw.col};
}

ResetStmt AstParser::parse_reset() {
    Token kw = expect(TokenKind::Keyword, "reset");
    RegRef qubit = parse_reg_ref();
    expect(TokenKind::Symbol, ";");
    return ResetStmt{qubit, kw.line, kw.col};
}

BarrierStmt AstParser::parse_barrier() {
    Token kw = expect(TokenKind::Keyword, "barrier");

    // Empty barrier (barrier;) means "apply to all qubits" in QASM v2.
    // We accept it by checking if the next token is already ";".
    std::vector<RegRef> qargs;
    if (!match(TokenKind::Symbol, ";")) {
        qargs = parse_reg_ref_list();
        expect(TokenKind::Symbol, ";");
    }
    return BarrierStmt{std::move(qargs), kw.line, kw.col};
}

IfStmt AstParser::parse_if() {
    Token kw = expect(TokenKind::Keyword, "if");
    expect(TokenKind::Symbol, "(");
    Token creg_tok = expect_kind(TokenKind::Ident, "classical register name");
    expect(TokenKind::Symbol, "==");
    Token val_tok = expect_kind(TokenKind::Number, "comparison value");
    expect(TokenKind::Symbol, ")");

    int val = std::stoi(val_tok.text);

    // Body must be a gate_call, measure, or reset — not a gate_def or if.
    // Parser enforces this restriction (OpenQASM v2 spec).
    const Token& body_tok = peek();
    std::unique_ptr<AstStmt> body;

    if (body_tok.kind == TokenKind::Keyword && body_tok.text == "measure") {
        body = std::make_unique<AstStmt>(AstStmt{parse_measure()});
    } else if (body_tok.kind == TokenKind::Keyword && body_tok.text == "reset") {
        body = std::make_unique<AstStmt>(AstStmt{parse_reset()});
    } else if (body_tok.kind == TokenKind::Ident) {
        body = std::make_unique<AstStmt>(AstStmt{parse_gate_call()});
    } else {
        // gate_def / opaque / if / qreg / creg / barrier as if-body are
        // not legal in OpenQASM v2. Emit a clear diagnostic.
        std::ostringstream msg;
        msg << "if body must be a gate call, measure, or reset; got '"
            << body_tok.text << "'";
        error_at(body_tok, msg.str());
    }

    return IfStmt{creg_tok.text, val, std::move(body), kw.line, kw.col};
}

// ── Sub-parsers ───────────────────────────────────────────────────────────────

RegRef AstParser::parse_reg_ref() {
    Token name = expect_kind(TokenKind::Ident, "register name");
    std::optional<int> index;
    if (match(TokenKind::Symbol, "[")) {
        Token idx_tok = expect_kind(TokenKind::Number, "index");
        expect(TokenKind::Symbol, "]");
        index = std::stoi(idx_tok.text);
    }
    return RegRef{name.text, index, name.line, name.col};
}

std::vector<RegRef> AstParser::parse_reg_ref_list() {
    std::vector<RegRef> list;
    // A ref list starts only if the current token is an IDENT.
    if (peek().kind != TokenKind::Ident) {
        return list;
    }
    list.push_back(parse_reg_ref());
    while (match(TokenKind::Symbol, ",")) {
        list.push_back(parse_reg_ref());
    }
    return list;
}

std::vector<std::string> AstParser::parse_formal_param_list() {
    // Formal parameter list is optional; only present when next token is "(".
    std::vector<std::string> params;
    if (!match(TokenKind::Symbol, "(")) {
        return params;
    }
    // An empty parameter list "()" is syntactically valid.
    if (!match(TokenKind::Symbol, ")")) {
        Token p = expect_kind(TokenKind::Ident, "parameter name");
        params.push_back(p.text);
        while (match(TokenKind::Symbol, ",")) {
            Token pn = expect_kind(TokenKind::Ident, "parameter name");
            params.push_back(pn.text);
        }
        expect(TokenKind::Symbol, ")");
    }
    return params;
}

std::vector<std::string> AstParser::parse_formal_qubit_list(
        const std::string& gate_name) {
    // At least one qubit name is required for a gate definition (OpenQASM).
    std::vector<std::string> qargs;
    Token first = expect_kind(TokenKind::Ident, "qubit name in gate '" + gate_name + "'");
    qargs.push_back(first.text);
    while (match(TokenKind::Symbol, ",")) {
        Token qn = expect_kind(TokenKind::Ident, "qubit name");
        qargs.push_back(qn.text);
    }
    return qargs;
}

std::vector<ExprPtr> AstParser::parse_param_args() {
    // Parameter argument list is optional; only present when next token is "(".
    std::vector<ExprPtr> args;
    if (!match(TokenKind::Symbol, "(")) {
        return args;
    }
    // Allow an empty "()" parameter list.
    if (!match(TokenKind::Symbol, ")")) {
        args.push_back(parse_expr());
        while (match(TokenKind::Symbol, ",")) {
            args.push_back(parse_expr());
        }
        expect(TokenKind::Symbol, ")");
    }
    return args;
}

std::vector<GateCall> AstParser::parse_gate_body() {
    // Parse zero or more gate calls until we see "}".
    // Only IDENT tokens start valid gate calls inside a gate body.
    std::vector<GateCall> body;
    while (peek().kind != TokenKind::Symbol || peek().text != "}") {
        if (peek().kind == TokenKind::EndOfFile) {
            error_current("unexpected end of file inside gate body (missing '}')");
        }
        if (peek().kind != TokenKind::Ident) {
            std::ostringstream msg;
            msg << "expected gate call inside gate body, got '"
                << peek().text << "'";
            error_current(msg.str());
        }
        // GateCall inside a body uses the same syntax as a top-level call.
        body.push_back(parse_gate_call());
    }
    return body;
}

// ── Expression parser (precedence climbing) ───────────────────────────────────

// Entry: lowest precedence (additive).
ExprPtr AstParser::parse_expr() {
    return parse_additive();
}

ExprPtr AstParser::parse_additive() {
    ExprPtr left = parse_multiplicative();
    while (peek().kind == TokenKind::Symbol &&
           (peek().text == "+" || peek().text == "-")) {
        Token op_tok = advance();
        BinaryOp op = (op_tok.text == "+") ? BinaryOp::Add : BinaryOp::Sub;
        ExprPtr right = parse_multiplicative();
        auto node = std::make_shared<ExprNode>();
        node->v = BinaryExpr{op, std::move(left), std::move(right),
                              op_tok.line, op_tok.col};
        left = std::move(node);
    }
    return left;
}

ExprPtr AstParser::parse_multiplicative() {
    ExprPtr left = parse_power();
    while (peek().kind == TokenKind::Symbol &&
           (peek().text == "*" || peek().text == "/")) {
        Token op_tok = advance();
        BinaryOp op = (op_tok.text == "*") ? BinaryOp::Mul : BinaryOp::Div;
        ExprPtr right = parse_power();
        auto node = std::make_shared<ExprNode>();
        node->v = BinaryExpr{op, std::move(left), std::move(right),
                              op_tok.line, op_tok.col};
        left = std::move(node);
    }
    return left;
}

ExprPtr AstParser::parse_power() {
    // Right-associative: parse_unary ^ parse_power (recursive right descent).
    ExprPtr base = parse_unary();
    if (peek().kind == TokenKind::Symbol && peek().text == "^") {
        Token op_tok = advance();
        ExprPtr exp  = parse_power();  // right-recursive for right-associativity
        auto node = std::make_shared<ExprNode>();
        node->v = BinaryExpr{BinaryOp::Pow, std::move(base), std::move(exp),
                              op_tok.line, op_tok.col};
        return node;
    }
    return base;
}

ExprPtr AstParser::parse_unary() {
    if (peek().kind == TokenKind::Symbol && peek().text == "-") {
        Token minus = advance();
        ExprPtr operand = parse_unary();  // allow double negation (`--x`)
        auto node = std::make_shared<ExprNode>();
        node->v = UnaryNegExpr{std::move(operand), minus.line, minus.col};
        return node;
    }
    return parse_primary();
}

ExprPtr AstParser::parse_primary() {
    const Token& cur = peek();

    // Number literal.
    if (cur.kind == TokenKind::Number) {
        Token t = advance();
        auto node = std::make_shared<ExprNode>();
        node->v = NumberLit{t.text, t.line, t.col};
        return node;
    }

    // Identifier or function call.
    if (cur.kind == TokenKind::Ident) {
        Token t = advance();
        // Check if this is a function call: IDENT "(" args ")".
        if (peek().kind == TokenKind::Symbol && peek().text == "(") {
            advance();  // consume "("
            std::vector<ExprPtr> args;
            if (!(peek().kind == TokenKind::Symbol && peek().text == ")")) {
                args.push_back(parse_expr());
                while (match(TokenKind::Symbol, ",")) {
                    args.push_back(parse_expr());
                }
            }
            expect(TokenKind::Symbol, ")");
            auto node = std::make_shared<ExprNode>();
            node->v = CallExpr{t.text, std::move(args), t.line, t.col};
            return node;
        }
        // Plain identifier.
        auto node = std::make_shared<ExprNode>();
        node->v = IdentExpr{t.text, t.line, t.col};
        return node;
    }

    // Parenthesised expression.
    if (cur.kind == TokenKind::Symbol && cur.text == "(") {
        advance();  // consume "("
        ExprPtr inner = parse_expr();
        expect(TokenKind::Symbol, ")");
        return inner;
    }

    // Anything else is a syntax error inside an expression.
    std::ostringstream msg;
    if (cur.kind == TokenKind::EndOfFile) {
        msg << "unexpected end of file in expression";
    } else {
        msg << "unexpected token '" << cur.text << "' in expression";
    }
    error_at(cur, msg.str());
}

}  // namespace qplayer::detail
