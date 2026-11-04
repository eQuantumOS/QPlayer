/// @file lexer.hpp
/// @brief Internal Lexer for the OpenQASM v2 parser pipeline.
/// @ingroup frontend
///
/// Hidden behind the Parser facade (parser.hpp); not part of the public ABI
/// because callers never link against AST/token types (
/// self-implemented parser, no external library).

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qplayer::detail {

/// Token categories emitted by the Lexer. The Parser dispatches on `kind` and
/// re-reads `text` (the lexeme) when the value is required (numeric literals,
/// identifier names, etc.). Position fields are 1-based to match human-facing
/// error messages (QASMParseError reports line/col verbatim).
///
/// KEYWORD is the reserved word set listed in plus
/// `OPENQASM` for the version header. `pi` and other math symbols stay as
/// IDENT — the expression evaluator treats them as named constants,
/// not lexical keywords, per OpenQASM v2 spec.
enum class TokenKind {
    Keyword,
    Ident,
    Number,
    String,        // "qelib1.inc" — only used by `include` statements
    Symbol,        // [ ] ( ) { } ; , -> == = + - * / ^
    EndOfFile,
};

/// One lexed token: its kind, source text and (line, col) position.
struct Token {
    TokenKind   kind = TokenKind::EndOfFile;
    std::string text;          // exact lexeme as it appeared in source
    int         line = 0;      // 1-based; 0 means uninitialised
    int         col  = 0;      // 1-based column of the first character
};

/// Lexer tokenises an entire QASM source string in one pass. `file_name`
/// labels the source for QASMParseError messages — pass "<text>" for in-memory
/// strings (matches Parser::parse) or the file path for parse_file callers.
///
/// Error model: tokenize() throws QASMParseError on the first illegal character
/// or unterminated literal. The Lexer never silently skips characters — every
/// byte either becomes a token or raises an error (location tracking).
class Lexer {
public:
    Lexer(std::string_view source, std::string file_name);

    // Consume the full source and return the token stream. The last token is
    // always EndOfFile so the Parser can drive a uniform loop.
    // throws: QASMParseError — illegal character, unterminated string,
    //   malformed number literal (file/line/col captured).
    std::vector<Token> tokenize();

private:
    // Single-token entry point. Returns EndOfFile token when input exhausted.
    Token next_token();

    // Skips spaces, tabs, newlines, and `// ...` line comments. After the call
    // pos_ points at the first byte of a real token or at end of source.
    void skip_whitespace_and_comments();

    // Lex helpers — each one assumes pos_ already points at the first byte of
    // the matching token shape (decided by next_token's character class
    // dispatch). They advance pos_ past the token and stamp line/col on the
    // returned Token.
    Token lex_identifier_or_keyword();
    Token lex_number();
    Token lex_string();
    Token lex_symbol();

    // Advances pos_ by one byte and updates (line_, col_). Use this for every
    // forward step so the position counters stay in sync with the source.
    char advance();

    // Peeks at offset `n` bytes ahead without moving pos_. Returns '\0' past
    // end-of-source (callers must treat '\0' as EOF, not as a literal NUL —
    // OpenQASM source must be plain ASCII per spec).
    char peek(std::size_t n = 0) const;

    bool at_end() const noexcept { return pos_ >= source_.size(); }

    // Throws QASMParseError stamped with (file_, line_, col_) and `msg`.
    [[noreturn]] void error(const std::string& msg) const;

    std::string_view source_;
    std::string      file_;
    std::size_t      pos_  = 0;
    int              line_ = 1;
    int              col_  = 1;
};

// Tests use this set directly to assert keyword classification; SSOT for the
// reserved word list.
bool is_qasm_keyword(std::string_view word);

}  // namespace qplayer::detail
