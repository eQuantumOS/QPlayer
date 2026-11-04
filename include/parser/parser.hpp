// parser.hpp — OpenQASM v2 parser facade.
// Public surface keeps two methods only. The internal Lexer / Parser /
// SemanticValidator / CircuitBuilder pipeline lives in src/parser/ and stays
// hidden behind this facade so users never link against the AST type set
// (— self-implemented parser, no external library).

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "circuit/circuit.hpp"

namespace qplayer {

class Parser {
public:
    Parser();
    ~Parser();

    // Parses an in-memory QASM v2 source string. Errors carry file ("<text>"),
    // line, column, and a human message via QASMParseError.
    Circuit parse(std::string_view qasm_text);

    // File-backed variant. Resolves `include "qelib1.inc"` against the
    // built-in symbol table (no filesystem read) and external includes
    // against the file's parent directory.
    Circuit parse_file(const std::string& path);

private:
    // Internal pipeline state lives behind a pimpl so this header stays free
    // of AST headers (built in Phase 2). The struct is intentionally
    // incomplete here.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qplayer
