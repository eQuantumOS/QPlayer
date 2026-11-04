/// @file parser.cpp
/// @brief OpenQASM v2 parser facade.
///
/// Connects the four-stage pipeline:
/// Lexer → AstParser → SemanticValidator → CircuitBuilder
/// and exposes just two public entry points (parse / parse_file) so callers
/// never depend on the internal AST / token types.
///
/// parse_file reads the source into a std::string and forwards to the same
/// pipeline with the file path as the error-message label.
/// External includes (anything other than "qelib1.inc") are rejected by
/// CircuitBuilder with QASMParseError — handled transparently in CircuitBuilder.
///
/// The Impl struct is intentionally left empty; no persistent pipeline state is
/// needed between calls. The pimpl exists solely to give the header a stable ABI
/// across partial recompiles.

#include "parser/parser.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "circuit/circuit.hpp"
#include "error.hpp"
#include "parser/detail/ast_parser.hpp"
#include "parser/detail/circuit_builder.hpp"
#include "parser/detail/lexer.hpp"
#include "parser/detail/semantic_validator.hpp"

namespace qplayer {

// Empty pimpl — no persistent state required (-3 note).
struct Parser::Impl {};

Parser::Parser()  = default;
Parser::~Parser() = default;

// ── Internal pipeline helper ──────────────────────────────────────────────────

namespace {

// Runs all four pipeline stages on `source` text with `label` as the error
// source identifier. Used by both parse() and parse_file().
Circuit run_pipeline(std::string_view source, const std::string& label) {
    // Stage 1: Lex.
    detail::Lexer lexer{source, label};
    auto tokens = lexer.tokenize();

    // Stage 2: Parse AST.
    detail::AstParser ap{std::move(tokens), label};
    auto program = ap.parse();

    // Stage 3: Semantic validation.
    detail::SemanticValidator sv{label};
    sv.validate(program);

    // Stage 4: Build Circuit IR.
    detail::CircuitBuilder cb{label};
    return cb.build(program);
}

}  // namespace

// ── Public facade methods ─────────────────────────────────────────────────────

Circuit Parser::parse(std::string_view qasm_text) {
    return run_pipeline(qasm_text, "<text>");
}

Circuit Parser::parse_file(const std::string& path) {
    std::ifstream ifs{path};
    if (!ifs.is_open()) {
        throw QASMParseError{path, 0, 0,
                             "cannot open file: " + path};
    }
    std::ostringstream buf;
    buf << ifs.rdbuf();
    if (ifs.fail() && !ifs.eof()) {
        throw QASMParseError{path, 0, 0,
                             "I/O error reading file: " + path};
    }
    return run_pipeline(buf.str(), path);
}

}  // namespace qplayer
