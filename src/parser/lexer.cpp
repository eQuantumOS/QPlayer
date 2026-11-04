// lexer.cpp — OpenQASM v2 Lexer implementation.
//
// Strategy: hand-written single-pass scanner over std::string_view. Each call
// to next_token() classifies the next non-whitespace byte and dispatches to a
// dedicated lex_* helper. Position counters (line_, col_) advance on every
// byte read so QASMParseError messages always pinpoint the offending column.
//
// SSOT:. Keyword list mirrors the design
// document exactly — adding a new reserved word requires updating both ends.
// `pi` and other math constants stay as IDENT so the expression
// evaluator can fold them at parameter resolution time.

#include "parser/detail/lexer.hpp"

#include "error.hpp"

#include <array>
#include <cctype>
#include <string_view>
#include <utility>

namespace qplayer::detail {

namespace {

// Reserved word table — + the `OPENQASM` version header
// that introduces every QASM v2 program. Kept sorted alphabetically (case-
// sensitive); is_qasm_keyword() does a linear scan because the set is small.
constexpr std::array<std::string_view, 10> kKeywords{{
    "OPENQASM",
    "barrier",
    "creg",
    "gate",
    "if",
    "include",
    "measure",
    "opaque",
    "qreg",
    "reset",
}};

bool starts_identifier(char c) {
    // OpenQASM v2 identifiers begin with a letter (any case) or underscore.
    // Digits are forbidden in the first position to keep number literals
    // unambiguous (e.g. `2pi` would otherwise blur the boundary).
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool continues_identifier(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

}  // namespace

bool is_qasm_keyword(std::string_view word) {
    for (const auto& kw : kKeywords) {
        if (kw == word) {
            return true;
        }
    }
    return false;
}

Lexer::Lexer(std::string_view source, std::string file_name)
    : source_(source), file_(std::move(file_name)) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        Token t = next_token();
        bool is_eof = (t.kind == TokenKind::EndOfFile);
        tokens.push_back(std::move(t));
        if (is_eof) {
            break;
        }
    }
    return tokens;
}

Token Lexer::next_token() {
    skip_whitespace_and_comments();
    if (at_end()) {
        Token eof;
        eof.kind = TokenKind::EndOfFile;
        eof.line = line_;
        eof.col  = col_;
        return eof;
    }

    char c = peek();
    if (starts_identifier(c)) {
        return lex_identifier_or_keyword();
    }
    if (is_digit(c)) {
        return lex_number();
    }
    if (c == '"') {
        return lex_string();
    }
    return lex_symbol();
}

void Lexer::skip_whitespace_and_comments() {
    while (!at_end()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            // Line comment: discard until newline or EOF. Do not consume the
            // newline itself — the outer loop counts it via advance() so
            // line_/col_ stay accurate for the next token.
            while (!at_end() && peek() != '\n') {
                advance();
            }
            continue;
        }
        return;
    }
}

Token Lexer::lex_identifier_or_keyword() {
    Token t;
    t.line = line_;
    t.col  = col_;
    while (!at_end() && continues_identifier(peek())) {
        t.text.push_back(advance());
    }
    t.kind = is_qasm_keyword(t.text) ? TokenKind::Keyword : TokenKind::Ident;
    return t;
}

Token Lexer::lex_number() {
    Token t;
    t.kind = TokenKind::Number;
    t.line = line_;
    t.col  = col_;

    // Integer part: at least one digit (guaranteed by caller dispatch on
    // is_digit). Consume the full run of leading digits first.
    while (!at_end() && is_digit(peek())) {
        t.text.push_back(advance());
    }

    // Optional fractional part: `.` followed by zero or more digits. Accepts
    // `2.0`, `2.`, `2.5` — matches OpenQASM v2 spec numeric grammar.
    if (peek() == '.') {
        t.text.push_back(advance());
        while (!at_end() && is_digit(peek())) {
            t.text.push_back(advance());
        }
    }

    // Optional exponent: `[eE][+-]?[0-9]+`. The mantissa is already consumed,
    // so we only need to validate that at least one digit follows the marker.
    if (peek() == 'e' || peek() == 'E') {
        t.text.push_back(advance());
        if (peek() == '+' || peek() == '-') {
            t.text.push_back(advance());
        }
        if (!is_digit(peek())) {
            error("malformed exponent in numeric literal");
        }
        while (!at_end() && is_digit(peek())) {
            t.text.push_back(advance());
        }
    }

    return t;
}

Token Lexer::lex_string() {
    Token t;
    t.kind = TokenKind::String;
    t.line = line_;
    t.col  = col_;
    advance();  // consume opening quote

    while (!at_end() && peek() != '"') {
        char c = peek();
        if (c == '\n') {
            // QASM v2 strings appear only in include directives and must not
            // span lines. Unterminated string → QASMParseError.
            error("unterminated string literal");
        }
        t.text.push_back(advance());
    }
    if (at_end()) {
        error("unterminated string literal");
    }
    advance();  // consume closing quote
    return t;
}

Token Lexer::lex_symbol() {
    Token t;
    t.kind = TokenKind::Symbol;
    t.line = line_;
    t.col  = col_;
    char c = peek();

    // Two-character symbols first — `->` (measure arrow) and `==` (if
    // condition). Single-char dispatch otherwise. The set below is the union
    // of symbols actually used by OpenQASM v2 grammar; anything outside is an
    // illegal character (error path below).
    if (c == '-' && peek(1) == '>') {
        t.text.push_back(advance());
        t.text.push_back(advance());
        return t;
    }
    if (c == '=' && peek(1) == '=') {
        t.text.push_back(advance());
        t.text.push_back(advance());
        return t;
    }

    switch (c) {
        case '[': case ']':
        case '(': case ')':
        case '{': case '}':
        case ';': case ',':
        case '+': case '-':
        case '*': case '/':
        case '^':
        case '=':                              // bare `=` reserved for future
            t.text.push_back(advance());
            return t;
        default:
            break;
    }

    // Anything else is illegal at this position. Report column of the offender,
    // not of the next byte we would have read.
    error(std::string{"illegal character: '"} + c + "'");
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

char Lexer::peek(std::size_t n) const {
    std::size_t idx = pos_ + n;
    if (idx >= source_.size()) {
        return '\0';
    }
    return source_[idx];
}

void Lexer::error(const std::string& msg) const {
    throw QASMParseError{file_, line_, col_, msg};
}

}  // namespace qplayer::detail
