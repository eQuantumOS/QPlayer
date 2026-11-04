// test_semantic_validator.cpp — Unit tests for the OpenQASM v2 SemanticValidator
//
//
// Each test runs the full Lexer → AstParser → SemanticValidator pipeline so
// that source-position stamps flow naturally from real token streams.
//
// Coverage:
//   A. Well-formed programs (Bell, QFT-style, gate_def, opaque, if, barrier).
//   B. Boundary cases (size=1 registers, 0-param gate, empty body, N=2^w-1).
//   C. Error cases — each verifies QASMParseError is thrown.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include "error.hpp"
#include "parser/detail/ast.hpp"
#include "parser/detail/ast_parser.hpp"
#include "parser/detail/lexer.hpp"
#include "parser/detail/semantic_validator.hpp"

#include <string>
#include <string_view>

using qplayer::QASMParseError;
using namespace qplayer::detail;

namespace {

// Full pipeline: lex → parse → semantic validate.
// Returns normally on success; propagates QASMParseError on any error.
void validate(std::string_view src) {
    Lexer lex{src, "<test>"};
    auto tokens = lex.tokenize();
    AstParser ap{std::move(tokens), "<test>"};
    Program prog = ap.parse();
    SemanticValidator sv{"<test>"};
    sv.validate(prog);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// A. Well-formed programs
// ─────────────────────────────────────────────────────────────────────────────

TEST(SemanticValidator, BellPair_ValidProgram_NoThrow) {
    // Arrange
    const char* src = R"(
OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
creg c[2];
h q[0];
cx q[0], q[1];
measure q[0] -> c[0];
measure q[1] -> c[1];
)";
    // Act + Assert
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, SingleQubitRotation_WithParam_NoThrow) {
    // Arrange: rx(pi/2) requires 1 param + 1 qubit.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
rx(1.5707963) q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, UserGateDef_InlinedCall_NoThrow) {
    // User-defined gate with 1 qubit, 0 params; called at top level.
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
gate mybell a, b {
  h a;
  cx a, b;
}
mybell q[0], q[1];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, OpaqueDecl_CalledAtTopLevel_NoThrow) {
    // opaque declaration: num_params=1, num_qubits=1.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
opaque mygate(theta) q;
mygate(1.0) q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, IfStmt_CregCondition_NoThrow) {
    // if(c==3) h q[0] with creg c[2] — value 3 = 2^2-1 (max).
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
creg c[2];
h q[0];
if (c==3) h q[1];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, BarrierWithArgs_NoThrow) {
    const char* src = R"(
OPENQASM 2.0;
qreg q[3];
h q[0];
barrier q[0], q[1], q[2];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, BarrierNoArgs_NoThrow) {
    // barrier with no args is legal in QASM v2.
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
barrier;
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, ResetGate_NoThrow) {
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
reset q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, GateWithParams_cu1_NoThrow) {
    // cu1(lambda) ctrl, tgt — 1 param + 2 qubits.
    const char* src = R"(
OPENQASM 2.0;
qreg q[3];
cu1(0.785398) q[0], q[1];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, GateDefWithParamInBody_NoThrow) {
    // gate body uses a formal parameter in an inner gate call.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
gate myrx(theta) a {
  rx(theta) a;
}
myrx(1.0) q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, MultipleGateDefs_NoCrossConflict_NoThrow) {
    // Two different user gate names — no collision.
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
gate gateA a { h a; }
gate gateB a { x a; }
gateA q[0];
gateB q[1];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, U3BuiltIn_ThreeParams_NoThrow) {
    // Built-in U(theta,phi,lambda) — 3 params, 1 qubit.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
U(1.0, 2.0, 3.0) q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, CXBuiltIn_TwoQubits_NoThrow) {
    // Built-in CX — 0 params, 2 qubits.
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
CX q[0], q[1];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, ToffoliCCX_NoThrow) {
    // ccx — 0 params, 3 qubits.
    const char* src = R"(
OPENQASM 2.0;
qreg q[3];
ccx q[0], q[1], q[2];
)";
    EXPECT_NO_THROW(validate(src));
}

// ─────────────────────────────────────────────────────────────────────────────
// B. Boundary cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(SemanticValidator, Boundary_QRegSize1_IndexZero_Valid) {
    // qreg q[1]; h q[0]; — index 0 is the only valid index.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
h q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, Boundary_CRegSize1_IndexZero_Valid) {
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
creg c[1];
measure q[0] -> c[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, Boundary_GateDef_ZeroParams_NoThrow) {
    // gate with 0 formal params is legal.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
gate noparam a { h a; }
noparam q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, Boundary_GateDef_EmptyBody_NoThrow) {
    // gate with an empty body is valid syntax.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
gate empty a { }
empty q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, Boundary_OpaqueZeroParams_NoThrow) {
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
opaque noparam_opaque a;
noparam_opaque q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, Boundary_IfValueAtMax_Width2_NoThrow) {
    // creg c[2]: max value = 3 = 2^2-1.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
creg c[2];
if (c==3) h q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

TEST(SemanticValidator, Boundary_IfValueZero_NoThrow) {
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
creg c[2];
if (c==0) h q[0];
)";
    EXPECT_NO_THROW(validate(src));
}

// ─────────────────────────────────────────────────────────────────────────────
// C. Error cases — each must throw QASMParseError
// ─────────────────────────────────────────────────────────────────────────────

TEST(SemanticValidator, Error_QubitIndexOutOfRange_Throws) {
    // qreg q[2]; h q[5]; — index 5 >= size 2.
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
h q[5];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_CbitIndexOutOfRange_Throws) {
    // creg c[2]; measure q[0] -> c[5]; — index 5 >= size 2.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
creg c[2];
measure q[0] -> c[5];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_DuplicateQreg_Throws) {
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
qreg q[3];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_DuplicateCreg_Throws) {
    const char* src = R"(
OPENQASM 2.0;
creg c[2];
creg c[3];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_DuplicateGateDef_Throws) {
    // Two gate defs with the same name.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
gate mygate a { h a; }
gate mygate a { x a; }
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_GateVsOpaqueNameConflict_Throws) {
    // gate and opaque share one namespace — same name is a collision.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
gate mything a { h a; }
opaque mything a;
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_OpaqueVsGateNameConflict_Throws) {
    // opaque declared first, then gate with same name.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
opaque mything a;
gate mything a { h a; }
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_GateQubitArgCountMismatch_Throws) {
    // h expects 1 qubit but gets 2.
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
h q[0], q[1];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_GateParamCountMismatch_Throws) {
    // rx expects 1 param but gets 0.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
rx q[0];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_UnknownGate_Throws) {
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
totally_unknown_gate q[0];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_IfConditionIsQreg_Throws) {
    // if (q==0) — q is a qreg, not a creg.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
if (q==0) h q[0];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_IfConditionValueOutOfRange_Throws) {
    // creg c[2]: valid range [0,3]; value 4 is out of range.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
creg c[2];
if (c==4) h q[0];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_IfConditionUndeclaredRegister_Throws) {
    // if (xyz==0) — xyz is not declared at all.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
if (xyz==0) h q[0];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_GateBodyDirectQregIndexing_Throws) {
    // gate foo a { h q[0]; } — q[0] is a direct qreg reference inside a body.
    const char* src = R"(
OPENQASM 2.0;
qreg q[2];
gate foo a {
  h q[0];
}
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_GateBodyFormalNotInScope_Throws) {
    // gate foo a { h b; } — b is not a formal qubit of foo.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
gate foo a {
  h b;
}
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_UndeclaredQreg_Throws) {
    // h q[0] without any qreg declaration.
    const char* src = R"(
OPENQASM 2.0;
h q[0];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

TEST(SemanticValidator, Error_UndeclaredCreg_InMeasure_Throws) {
    // measure q[0] -> c[0] without creg declaration.
    const char* src = R"(
OPENQASM 2.0;
qreg q[1];
measure q[0] -> c[0];
)";
    EXPECT_THROW(validate(src), qplayer::QASMParseError);
}

// Verify that QASMParseError carries a non-zero line number (position stamp).
TEST(SemanticValidator, Error_PositionStamp_LineNonZero) {
    // Arrange: the bad qubit reference is on line 4 of the source.
    const char* src = R"(OPENQASM 2.0;
qreg q[2];
creg c[1];
h q[9];
)";
    // Act
    bool caught = false;
    try {
        validate(src);
    } catch (const qplayer::QASMParseError& e) {
        caught = true;
        // Assert: line must be > 0 (stamped at source location of the violation).
        EXPECT_TRUE(e.line() > 0);
    }
    EXPECT_TRUE(caught);
}
