// test_lexer.cpp — Unit tests for the OpenQASM v2 Lexer.
//
// Coverage: keyword classification, identifier vs keyword disambiguation,
// number literals (int / decimal / scientific), string literals, multi-char
// symbols (->, ==), comment skipping, position tracking, and QASMParseError
// emission on illegal input. The Lexer is exercised via std::string_view; no
// filesystem I/O.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include "error.hpp"
#include "parser/detail/lexer.hpp"

#include <string>
#include <string_view>
#include <vector>

using qplayer::QASMParseError;
using qplayer::detail::is_qasm_keyword;
using qplayer::detail::Lexer;
using qplayer::detail::Token;
using qplayer::detail::TokenKind;

namespace {

std::vector<Token> tokenize(std::string_view src) {
    Lexer lex{src, "<text>"};
    return lex.tokenize();
}

}  // namespace

// ── Keyword classification ───────────────────────────────────────────────────

TEST(Lexer, IsQasmKeyword_AcceptsReservedWords_True) {
    EXPECT_TRUE(is_qasm_keyword("OPENQASM"));
    EXPECT_TRUE(is_qasm_keyword("qreg"));
    EXPECT_TRUE(is_qasm_keyword("creg"));
    EXPECT_TRUE(is_qasm_keyword("gate"));
    EXPECT_TRUE(is_qasm_keyword("opaque"));
    EXPECT_TRUE(is_qasm_keyword("measure"));
    EXPECT_TRUE(is_qasm_keyword("if"));
    EXPECT_TRUE(is_qasm_keyword("barrier"));
    EXPECT_TRUE(is_qasm_keyword("reset"));
    EXPECT_TRUE(is_qasm_keyword("include"));
}

TEST(Lexer, IsQasmKeyword_RejectsIdentifiers_False) {
    EXPECT_FALSE(is_qasm_keyword("pi"));      // pi stays as IDENT (evaluator)
    EXPECT_FALSE(is_qasm_keyword("h"));       // standard gate names are IDENT
    EXPECT_FALSE(is_qasm_keyword("cx"));
    EXPECT_FALSE(is_qasm_keyword("Q"));
    EXPECT_FALSE(is_qasm_keyword(""));
}

// ── Empty / EOF handling ─────────────────────────────────────────────────────

TEST(Lexer, Tokenize_EmptySource_OnlyEof) {
    auto toks = tokenize("");
    EXPECT_EQ(toks.size(), std::size_t{1});
    EXPECT_TRUE(toks[0].kind == TokenKind::EndOfFile);
}

TEST(Lexer, Tokenize_WhitespaceOnly_OnlyEof) {
    auto toks = tokenize("   \t\n  \r\n");
    EXPECT_EQ(toks.size(), std::size_t{1});
    EXPECT_TRUE(toks[0].kind == TokenKind::EndOfFile);
}

// ── Comment skipping ─────────────────────────────────────────────────────────

TEST(Lexer, Tokenize_LineComment_Skipped) {
    auto toks = tokenize("// this is a comment\nqreg q[2];");
    // Expect: qreg IDENT( q ) [ 2 ] ; EOF — 7 tokens.
    EXPECT_EQ(toks.size(), std::size_t{7});
    EXPECT_TRUE(toks[0].kind == TokenKind::Keyword);
    EXPECT_EQ(toks[0].text, std::string{"qreg"});
    EXPECT_EQ(toks[0].line, 2);   // comment consumed line 1
    EXPECT_EQ(toks[0].col, 1);
}

TEST(Lexer, Tokenize_CommentToEndOfFile_NoTrailingNewlineNeeded) {
    auto toks = tokenize("qreg q[1];  // trailing");
    // Same 7-token shape as above; comment after `;` is consumed without error.
    EXPECT_EQ(toks.size(), std::size_t{7});
    EXPECT_TRUE(toks[6].kind == TokenKind::EndOfFile);
}

// ── Keyword vs identifier disambiguation ─────────────────────────────────────

TEST(Lexer, Tokenize_GateKeyword_TaggedAsKeyword) {
    auto toks = tokenize("gate h q { U(0,0,0) q; }");
    EXPECT_TRUE(toks[0].kind == TokenKind::Keyword);
    EXPECT_EQ(toks[0].text, std::string{"gate"});
    EXPECT_TRUE(toks[1].kind == TokenKind::Ident);
    EXPECT_EQ(toks[1].text, std::string{"h"});
}

TEST(Lexer, Tokenize_PiAsIdentifier_NotKeyword) {
    // `pi` is recognized by the expression evaluator, not by the Lexer.
    auto toks = tokenize("rx(pi)");
    EXPECT_TRUE(toks[0].kind == TokenKind::Ident);
    EXPECT_EQ(toks[0].text, std::string{"rx"});
    EXPECT_TRUE(toks[2].kind == TokenKind::Ident);
    EXPECT_EQ(toks[2].text, std::string{"pi"});
}

// ── Number literals ──────────────────────────────────────────────────────────

TEST(Lexer, Tokenize_IntegerLiteral_PreservesText) {
    auto toks = tokenize("q[42]");
    EXPECT_TRUE(toks[2].kind == TokenKind::Number);
    EXPECT_EQ(toks[2].text, std::string{"42"});
}

TEST(Lexer, Tokenize_DecimalLiteral_PreservesText) {
    auto toks = tokenize("OPENQASM 2.0;");
    EXPECT_TRUE(toks[1].kind == TokenKind::Number);
    EXPECT_EQ(toks[1].text, std::string{"2.0"});
}

TEST(Lexer, Tokenize_ScientificLiteral_PreservesText) {
    auto toks = tokenize("rx(1.5e-3)");
    EXPECT_TRUE(toks[2].kind == TokenKind::Number);
    EXPECT_EQ(toks[2].text, std::string{"1.5e-3"});
}

TEST(Lexer, Tokenize_MalformedExponent_Throws) {
    // "1e" with no digits after the exponent marker is illegal.
    EXPECT_THROW(tokenize("rx(1e)"), QASMParseError);
}

// ── String literals ──────────────────────────────────────────────────────────

TEST(Lexer, Tokenize_IncludeString_StripsQuotes) {
    auto toks = tokenize("include \"qelib1.inc\";");
    EXPECT_TRUE(toks[0].kind == TokenKind::Keyword);
    EXPECT_TRUE(toks[1].kind == TokenKind::String);
    // Lexer reports the inner content without the surrounding quotes.
    EXPECT_EQ(toks[1].text, std::string{"qelib1.inc"});
}

TEST(Lexer, Tokenize_UnterminatedString_Throws) {
    EXPECT_THROW(tokenize("include \"missing-close\n"), QASMParseError);
}

// ── Symbols (single + multi-char) ────────────────────────────────────────────

TEST(Lexer, Tokenize_MultiCharSymbols_MeasureArrow_RecognizedAsArrow) {
    auto toks = tokenize("measure q[0] -> c[0];");
    // measure q [ 0 ] -> c [ 0 ] ; EOF — 12 tokens.
    EXPECT_EQ(toks.size(), std::size_t{12});
    EXPECT_TRUE(toks[5].kind == TokenKind::Symbol);
    EXPECT_EQ(toks[5].text, std::string{"->"});
}

TEST(Lexer, Tokenize_IfCondition_EqualsEqualsRecognized) {
    auto toks = tokenize("if (c==0) x q[0];");
    bool found_eqeq = false;
    for (const auto& t : toks) {
        if (t.kind == TokenKind::Symbol && t.text == "==") {
            found_eqeq = true;
            break;
        }
    }
    EXPECT_TRUE(found_eqeq);
}

TEST(Lexer, Tokenize_BellPairProgram_TokenSequenceMatchesDesign) {
    // Smoke test using a stripped-down Bell pair, mirroring tests/data/qasm/bell.qasm
    // but without the preamble. Verifies the Lexer produces a clean stream the
    // Parser can drive.
    auto toks = tokenize(
        "OPENQASM 2.0;\n"
        "qreg q[2];\n"
        "h q[0];\n"
        "cx q[0], q[1];\n");

    // OPENQASM 2.0 ; qreg q [ 2 ] ; h q [ 0 ] ; cx q [ 0 ] , q [ 1 ] ; EOF
    // 3 + 6 + 6 + 11 + EOF = 27 tokens.
    EXPECT_EQ(toks.size(), std::size_t{27});
    EXPECT_TRUE(toks[0].kind == TokenKind::Keyword);
    EXPECT_EQ(toks[0].text, std::string{"OPENQASM"});
    EXPECT_TRUE(toks[26].kind == TokenKind::EndOfFile);
}

// ── Position tracking ────────────────────────────────────────────────────────

TEST(Lexer, Tokenize_PositionTracking_LineAndColumnAreOneBased) {
    auto toks = tokenize("qreg q[2];\ncreg c[2];\n");
    // `qreg` on line 1, col 1
    EXPECT_EQ(toks[0].line, 1);
    EXPECT_EQ(toks[0].col, 1);
    // `creg` on line 2, col 1 (first token after the newline)
    bool checked = false;
    for (const auto& t : toks) {
        if (t.kind == TokenKind::Keyword && t.text == "creg") {
            EXPECT_EQ(t.line, 2);
            EXPECT_EQ(t.col, 1);
            checked = true;
        }
    }
    EXPECT_TRUE(checked);
}

// ── Error propagation ────────────────────────────────────────────────────────

TEST(Lexer, Tokenize_IllegalCharacter_ThrowsWithLineAndCol) {
    try {
        tokenize("qreg q[2];\n@bad");
        FAIL("expected QASMParseError");
    } catch (const QASMParseError& e) {
        // `@` lives on line 2 column 1.
        EXPECT_EQ(e.line(), 2);
        EXPECT_EQ(e.col(), 1);
    }
}
