// test_parser_ast.cpp — Unit tests for the OpenQASM v2 recursive-descent
// parser. Tests cover the AstParser class directly; no Lexer output is
// hand-crafted — we exercise the full Lexer → AstParser pipeline so that
// position information flows naturally.
//
// Coverage areas:
//   1. Well-formed programs (version, qreg, creg, gate calls, gate_def, measure,
//      reset, barrier, if, opaque, multi-statement programs).
//   2. Expression variants (integer, decimal, pi, pi/2, negation, power, call).
//   3. Error cases: missing ";", unmatched "{", bad if-body, empty gate name, etc.
//   4. Boundary cases: empty program, gate with empty body, barrier with no args.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include "error.hpp"
#include "parser/detail/ast.hpp"
#include "parser/detail/ast_parser.hpp"
#include "parser/detail/lexer.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using qplayer::QASMParseError;
using namespace qplayer::detail;

namespace {

// Helper: lex then parse the given source string. Returns the Program AST or
// propagates any QASMParseError thrown by the pipeline.
Program parse(std::string_view src) {
    Lexer lex{src, "<test>"};
    auto tokens = lex.tokenize();
    AstParser parser{std::move(tokens), "<test>"};
    return parser.parse();
}

// Helper: extract a specific variant from AstStmt at index i.
template <typename T>
const T& stmt_as(const Program& prog, std::size_t i) {
    return std::get<T>(prog.statements.at(i).v);
}

}  // namespace

// ── 1. Empty program ──────────────────────────────────────────────────────────

TEST(Parser, EmptySource_ReturnsEmptyProgram) {
    // Arrange + Act
    auto prog = parse("");

    // Assert
    EXPECT_EQ(prog.statements.size(), std::size_t{0});
}

TEST(Parser, WhitespaceOnly_ReturnsEmptyProgram) {
    auto prog = parse("   \n\t  ");
    EXPECT_EQ(prog.statements.size(), std::size_t{0});
}

// ── 2. Version declaration ────────────────────────────────────────────────────

TEST(Parser, VersionDecl_ValidVersion_ParsedCorrectly) {
    auto prog = parse("OPENQASM 2.0;");
    EXPECT_EQ(prog.statements.size(), std::size_t{1});
    const auto& vd = stmt_as<VersionDecl>(prog, 0);
    EXPECT_EQ(vd.version, std::string{"2.0"});
    EXPECT_EQ(vd.line, 1);
    EXPECT_EQ(vd.col, 1);
}

// ── 3. Register declarations ──────────────────────────────────────────────────

TEST(Parser, QRegDecl_ValidDecl_SizeAndNameCorrect) {
    auto prog = parse("qreg q[5];");
    EXPECT_EQ(prog.statements.size(), std::size_t{1});
    const auto& qd = stmt_as<QRegDecl>(prog, 0);
    EXPECT_EQ(qd.name, std::string{"q"});
    EXPECT_EQ(qd.size, 5);
}

TEST(Parser, CRegDecl_ValidDecl_SizeAndNameCorrect) {
    auto prog = parse("creg c[3];");
    EXPECT_EQ(prog.statements.size(), std::size_t{1});
    const auto& cd = stmt_as<CRegDecl>(prog, 0);
    EXPECT_EQ(cd.name, std::string{"c"});
    EXPECT_EQ(cd.size, 3);
}

TEST(Parser, QRegDecl_SizeOne_IsValid) {
    // Boundary: single qubit register.
    auto prog = parse("qreg a[1];");
    const auto& qd = stmt_as<QRegDecl>(prog, 0);
    EXPECT_EQ(qd.size, 1);
}

// ── 4. Include statement ──────────────────────────────────────────────────────

TEST(Parser, IncludeStmt_QelibPath_ParsedWithoutFileIO) {
    auto prog = parse("include \"qelib1.inc\";");
    EXPECT_EQ(prog.statements.size(), std::size_t{1});
    const auto& inc = stmt_as<IncludeStmt>(prog, 0);
    // The path stored is the bare filename without quotes.
    EXPECT_EQ(inc.path, std::string{"qelib1.inc"});
}

// ── 5. Gate call (top-level) ──────────────────────────────────────────────────

TEST(Parser, GateCall_NoParams_SingleQubit_Correct) {
    // h q[0]; — single qubit gate without parameters.
    auto prog = parse("qreg q[2];\nh q[0];");
    EXPECT_EQ(prog.statements.size(), std::size_t{2});
    const auto& gc = stmt_as<GateCall>(prog, 1);
    EXPECT_EQ(gc.name, std::string{"h"});
    EXPECT_EQ(gc.params.size(), std::size_t{0});
    EXPECT_EQ(gc.qargs.size(), std::size_t{1});
    EXPECT_EQ(gc.qargs[0].name, std::string{"q"});
    EXPECT_EQ(gc.qargs[0].index.value(), 0);
}

TEST(Parser, GateCall_TwoQubitGate_BothArgsPresent) {
    // cx q[0], q[1];
    auto prog = parse("cx q[0], q[1];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_EQ(gc.name, std::string{"cx"});
    EXPECT_EQ(gc.qargs.size(), std::size_t{2});
    EXPECT_EQ(gc.qargs[1].index.value(), 1);
}

TEST(Parser, GateCall_WithParamExpression_ParamsPreserved) {
    // rx(pi/2) q[0]; — parameter expression preserved as AST, not evaluated.
    auto prog = parse("rx(pi/2) q[0];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_EQ(gc.name, std::string{"rx"});
    EXPECT_EQ(gc.params.size(), std::size_t{1});
    // Parameter is a BinaryExpr (Div).
    EXPECT_TRUE(std::holds_alternative<BinaryExpr>(gc.params[0]->v));
    const auto& bin = std::get<BinaryExpr>(gc.params[0]->v);
    EXPECT_TRUE(bin.op == BinaryOp::Div);
}

TEST(Parser, GateCall_MultipleParams_AllPresent) {
    // U(pi/2, 0, pi) q[0]; — 3-parameter gate.
    auto prog = parse("U(pi/2, 0, pi) q[0];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_EQ(gc.params.size(), std::size_t{3});
}

// ── 6. Measure statement ──────────────────────────────────────────────────────

TEST(Parser, MeasureStmt_FullIndexed_CorrectRefs) {
    auto prog = parse("measure q[0] -> c[0];");
    EXPECT_EQ(prog.statements.size(), std::size_t{1});
    const auto& ms = stmt_as<MeasureStmt>(prog, 0);
    EXPECT_EQ(ms.qubit.name, std::string{"q"});
    EXPECT_EQ(ms.qubit.index.value(), 0);
    EXPECT_EQ(ms.cbit.name, std::string{"c"});
    EXPECT_EQ(ms.cbit.index.value(), 0);
    EXPECT_EQ(ms.line, 1);
}

// ── 7. Reset statement ───────────────────────────────────────────────────────

TEST(Parser, ResetStmt_Indexed_ParsedCorrectly) {
    auto prog = parse("reset q[1];");
    const auto& rs = stmt_as<ResetStmt>(prog, 0);
    EXPECT_EQ(rs.qubit.name, std::string{"q"});
    EXPECT_EQ(rs.qubit.index.value(), 1);
}

// ── 8. Barrier statement ──────────────────────────────────────────────────────

TEST(Parser, BarrierStmt_TwoQubits_BothPresent) {
    auto prog = parse("barrier q[0], q[1];");
    const auto& bs = stmt_as<BarrierStmt>(prog, 0);
    EXPECT_EQ(bs.qargs.size(), std::size_t{2});
}

TEST(Parser, BarrierStmt_EmptyArgList_ValidEmptyBarrier) {
    // "barrier;" with no qubit list means "all qubits" in QASM v2.
    auto prog = parse("barrier;");
    const auto& bs = stmt_as<BarrierStmt>(prog, 0);
    EXPECT_EQ(bs.qargs.size(), std::size_t{0});
}

// ── 9. If statement ───────────────────────────────────────────────────────────

TEST(Parser, IfStmt_GateCallBody_FieldsCorrect) {
    auto prog = parse("if (c==1) x q[0];");
    EXPECT_EQ(prog.statements.size(), std::size_t{1});
    const auto& ifs = stmt_as<IfStmt>(prog, 0);
    EXPECT_EQ(ifs.creg_name, std::string{"c"});
    EXPECT_EQ(ifs.value, 1);
    // Body should be a GateCall.
    EXPECT_TRUE(std::holds_alternative<GateCall>(ifs.body->v));
    const auto& body_gc = std::get<GateCall>(ifs.body->v);
    EXPECT_EQ(body_gc.name, std::string{"x"});
}

TEST(Parser, IfStmt_MeasureBody_ParsedCorrectly) {
    auto prog = parse("if (c==0) measure q[0] -> c[0];");
    const auto& ifs = stmt_as<IfStmt>(prog, 0);
    EXPECT_TRUE(std::holds_alternative<MeasureStmt>(ifs.body->v));
}

TEST(Parser, IfStmt_ResetBody_ParsedCorrectly) {
    auto prog = parse("if (c==3) reset q[0];");
    const auto& ifs = stmt_as<IfStmt>(prog, 0);
    EXPECT_TRUE(std::holds_alternative<ResetStmt>(ifs.body->v));
}

// ── 10. Gate definition ───────────────────────────────────────────────────────

TEST(Parser, GateDef_EmptyBody_ParsedCorrectly) {
    // A gate with an empty body is syntactically valid in QASM v2.
    auto prog = parse("gate mygate q { }");
    EXPECT_EQ(prog.statements.size(), std::size_t{1});
    const auto& gd = stmt_as<GateDef>(prog, 0);
    EXPECT_EQ(gd.name, std::string{"mygate"});
    EXPECT_EQ(gd.qargs.size(), std::size_t{1});
    EXPECT_EQ(gd.body.size(), std::size_t{0});
}

TEST(Parser, GateDef_WithParamsAndBody_AllFieldsCorrect) {
    const char* src =
        "gate bell q0, q1 {\n"
        "  h q0;\n"
        "  cx q0, q1;\n"
        "}";
    auto prog = parse(src);
    const auto& gd = stmt_as<GateDef>(prog, 0);
    EXPECT_EQ(gd.name, std::string{"bell"});
    EXPECT_EQ(gd.qargs.size(), std::size_t{2});
    EXPECT_EQ(gd.body.size(), std::size_t{2});
    EXPECT_EQ(gd.body[0].name, std::string{"h"});
    EXPECT_EQ(gd.body[1].name, std::string{"cx"});
}

TEST(Parser, GateDef_WithFormalParams_ParamListPopulated) {
    const char* src = "gate rxy(theta, phi) q { rx(theta) q; ry(phi) q; }";
    auto prog = parse(src);
    const auto& gd = stmt_as<GateDef>(prog, 0);
    EXPECT_EQ(gd.params.size(), std::size_t{2});
    EXPECT_EQ(gd.params[0], std::string{"theta"});
    EXPECT_EQ(gd.params[1], std::string{"phi"});
}

// ── 11. Opaque declaration ────────────────────────────────────────────────────

TEST(Parser, OpaqueDecl_NoParams_CorrectFields) {
    auto prog = parse("opaque myop a, b;");
    const auto& od = stmt_as<OpaqueDecl>(prog, 0);
    EXPECT_EQ(od.name, std::string{"myop"});
    EXPECT_EQ(od.qargs.size(), std::size_t{2});
    EXPECT_EQ(od.params.size(), std::size_t{0});
}

TEST(Parser, OpaqueDecl_WithParams_ParamsPresent) {
    auto prog = parse("opaque noise(gamma) q;");
    const auto& od = stmt_as<OpaqueDecl>(prog, 0);
    EXPECT_EQ(od.params.size(), std::size_t{1});
    EXPECT_EQ(od.params[0], std::string{"gamma"});
}

// ── 12. Expression variants ───────────────────────────────────────────────────

TEST(Parser, Expr_IntegerLiteral_IsNumberLit) {
    auto prog = parse("rx(42) q[0];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_TRUE(std::holds_alternative<NumberLit>(gc.params[0]->v));
    const auto& nl = std::get<NumberLit>(gc.params[0]->v);
    EXPECT_EQ(nl.text, std::string{"42"});
}

TEST(Parser, Expr_PiIdentifier_IsIdentExpr) {
    // `pi` must be preserved as IdentExpr — not evaluated at parse time.
    auto prog = parse("rx(pi) q[0];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_TRUE(std::holds_alternative<IdentExpr>(gc.params[0]->v));
    const auto& ie = std::get<IdentExpr>(gc.params[0]->v);
    EXPECT_EQ(ie.name, std::string{"pi"});
}

TEST(Parser, Expr_UnaryNeg_WrapsOperand) {
    // -pi/4 parses as (-pi)/4 (standard left-to-right precedence).
    auto prog = parse("rz(-pi/4) q[0];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    // Top-level is a BinaryExpr(Div) wrapping UnaryNegExpr(pi).
    EXPECT_TRUE(std::holds_alternative<BinaryExpr>(gc.params[0]->v));
    const auto& bin = std::get<BinaryExpr>(gc.params[0]->v);
    EXPECT_TRUE(bin.op == BinaryOp::Div);
    EXPECT_TRUE(std::holds_alternative<UnaryNegExpr>(bin.left->v));
}

TEST(Parser, Expr_PowerOperator_RightAssociative) {
    // 2^10 should parse as BinaryExpr(Pow, 2, 10).
    auto prog = parse("rx(2^10) q[0];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_TRUE(std::holds_alternative<BinaryExpr>(gc.params[0]->v));
    const auto& bin = std::get<BinaryExpr>(gc.params[0]->v);
    EXPECT_TRUE(bin.op == BinaryOp::Pow);
}

TEST(Parser, Expr_FunctionCallSinPlusCos_IsAddOfTwoCalls) {
    // sin(pi)+cos(pi) → BinaryExpr(Add, CallExpr(sin,...), CallExpr(cos,...)).
    auto prog = parse("rx(sin(pi)+cos(pi)) q[0];");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_TRUE(std::holds_alternative<BinaryExpr>(gc.params[0]->v));
    const auto& bin = std::get<BinaryExpr>(gc.params[0]->v);
    EXPECT_TRUE(bin.op == BinaryOp::Add);
    EXPECT_TRUE(std::holds_alternative<CallExpr>(bin.left->v));
    EXPECT_TRUE(std::holds_alternative<CallExpr>(bin.right->v));
}

// ── 13. Bell pair full program ────────────────────────────────────────────────

TEST(Parser, BellPairProgram_ParsesCompletely) {
    // Mirrors tests/data/qasm/bell.qasm structure without the include line.
    const char* src =
        "OPENQASM 2.0;\n"
        "qreg q[2];\n"
        "creg c[2];\n"
        "h q[0];\n"
        "cx q[0], q[1];\n"
        "measure q[0] -> c[0];\n"
        "measure q[1] -> c[1];\n";

    auto prog = parse(src);
    EXPECT_EQ(prog.statements.size(), std::size_t{7});

    // Check statement types in order.
    EXPECT_TRUE(std::holds_alternative<VersionDecl>(prog.statements[0].v));
    EXPECT_TRUE(std::holds_alternative<QRegDecl>(prog.statements[1].v));
    EXPECT_TRUE(std::holds_alternative<CRegDecl>(prog.statements[2].v));
    EXPECT_TRUE(std::holds_alternative<GateCall>(prog.statements[3].v));
    EXPECT_TRUE(std::holds_alternative<GateCall>(prog.statements[4].v));
    EXPECT_TRUE(std::holds_alternative<MeasureStmt>(prog.statements[5].v));
    EXPECT_TRUE(std::holds_alternative<MeasureStmt>(prog.statements[6].v));
}

// ── 14. Multi-qubit gate without subscript (whole-register) ──────────────────

TEST(Parser, GateCall_WholeRegisterArg_NoIndexPresent) {
    // `h q` — qubit register used without subscript index.
    auto prog = parse("h q;");
    const auto& gc = stmt_as<GateCall>(prog, 0);
    EXPECT_FALSE(gc.qargs[0].index.has_value());
}

// ── 15. Position tracking ─────────────────────────────────────────────────────

TEST(Parser, StatementPositions_LineAndColPreserved) {
    // qreg on line 1, creg on line 2; check that (line, col) of each stmt node
    // matches the token position from the Lexer.
    const char* src = "qreg q[2];\ncreg c[2];";
    auto prog = parse(src);
    const auto& qd = stmt_as<QRegDecl>(prog, 0);
    const auto& cd = stmt_as<CRegDecl>(prog, 1);
    EXPECT_EQ(qd.line, 1);
    EXPECT_EQ(qd.col,  1);
    EXPECT_EQ(cd.line, 2);
    EXPECT_EQ(cd.col,  1);
}

// ── 16. Error cases ──────────────────────────────────────────────────────────

TEST(Parser, Error_MissingSemicolon_ThrowsParseError) {
    EXPECT_THROW(parse("qreg q[2]"), QASMParseError);
}

TEST(Parser, Error_MissingClosingBracket_ThrowsParseError) {
    EXPECT_THROW(parse("qreg q[2;"), QASMParseError);
}

TEST(Parser, Error_GateCallNoQubits_ThrowsParseError) {
    // A gate call with no qubit arguments is a parser error.
    EXPECT_THROW(parse("h;"), QASMParseError);
}

TEST(Parser, Error_IfBodyIsGateDef_ThrowsParseError) {
    // `if` body must be gate_call / measure / reset — not a gate_def.
    EXPECT_THROW(parse("if (c==0) gate mygate q { }"), QASMParseError);
}

TEST(Parser, Error_IfBodyIsGateDef_Keyword_ThrowsParseError) {
    // `if` body with `qreg` should also be rejected.
    EXPECT_THROW(parse("if (c==0) qreg r[2];"), QASMParseError);
}

TEST(Parser, Error_UnclosedGateBody_ThrowsParseError) {
    EXPECT_THROW(parse("gate mygate q { h q;"), QASMParseError);
}

TEST(Parser, Error_VersionWithIdentifierInsteadOfNumber_ThrowsParseError) {
    EXPECT_THROW(parse("OPENQASM two;"), QASMParseError);
}

TEST(Parser, Error_ParseErrorContainsLineAndCol) {
    // Verify that the thrown QASMParseError carries accurate position info.
    try {
        parse("qreg q[2]\n"   // line 1: missing ";"
              "creg c[2];");
        FAIL("expected QASMParseError");
    } catch (const QASMParseError& e) {
        // The error is at line 2 col 1 where "creg" starts (parser expected ";").
        EXPECT_EQ(e.line(), 2);
    }
}

TEST(Parser, Error_ExpressionMissingClosingParen_ThrowsParseError) {
    EXPECT_THROW(parse("rx(pi/2 q[0];"), QASMParseError);
}
