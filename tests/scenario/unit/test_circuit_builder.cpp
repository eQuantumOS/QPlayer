// test_circuit_builder.cpp — Unit tests for CircuitBuilder.
// Covers: normal gate emission, qreg/creg multi-register, user-defined gate
// inline expansion, opaque declaration, barrier, reset, if-then, parameter
// expression evaluation, parse_file, round-trip to_qasm(), and error cases.
//
// Testing rules: -1.3. All floating-point comparisons use
// EXPECT_NEAR with documented tolerance. Arrange / Act / Assert 3-section format.

#include "test_case.hpp"
#include "assertions.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "error.hpp"
#include "parser/parser.hpp"
#include "types.hpp"

using namespace qplayer;

// ── Helper: count ops of a specific Op alternative ──────────────────────────

static std::size_t count_gate_ops(const Circuit& c) {
    std::size_t n = 0;
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<GateOp>(op)) ++n;
    }
    return n;
}

static std::size_t count_measure_ops(const Circuit& c) {
    std::size_t n = 0;
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<MeasureOp>(op)) ++n;
    }
    return n;
}

static std::size_t count_barrier_ops(const Circuit& c) {
    std::size_t n = 0;
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<BarrierOp>(op)) ++n;
    }
    return n;
}

static std::size_t count_if_ops(const Circuit& c) {
    std::size_t n = 0;
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<IfOp>(op)) ++n;
    }
    return n;
}

// ── Helper: find first GateOp at position idx in ops stream ─────────────────
static const GateOp& nth_gate(const Circuit& c, std::size_t idx) {
    std::size_t seen = 0;
    for (const auto& op : c.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            if (seen == idx) return *g;
            ++seen;
        }
    }
    throw std::out_of_range{"nth_gate: index out of range"};
}

// ═══════════════════════════════════════════════════════════════════
// 1. NORMAL CASES — basic gate emission
// ═══════════════════════════════════════════════════════════════════

TEST(CircuitBuilder, BellPair_ParsesCorrectly) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "creg c[2];\n"
        "h q[0];\n"
        "cx q[0],q[1];\n"
        "measure q[0] -> c[0];\n"
        "measure q[1] -> c[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — 2 gates (h, cx) + 2 measures
    EXPECT_EQ(count_gate_ops(c),    std::size_t{2});
    EXPECT_EQ(count_measure_ops(c), std::size_t{2});
    EXPECT_EQ(c.num_qubits(), QubitIndex{2});
}

TEST(CircuitBuilder, BellPair_GateTypes_AreCorrect) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "creg c[2];\n"
        "h q[0];\n"
        "cx q[0],q[1];\n"
        "measure q[0] -> c[0];\n"
        "measure q[1] -> c[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — gate 0 is H on qubit 0, gate 1 is CX on qubits 0,1
    const GateOp& g0 = nth_gate(c, 0);
    EXPECT_TRUE(g0.type == GateType::H);
    EXPECT_EQ(g0.qubits[0], QubitIndex{0});

    const GateOp& g1 = nth_gate(c, 1);
    EXPECT_TRUE(g1.type == GateType::CX);
    EXPECT_EQ(g1.qubits[0], QubitIndex{0});
    EXPECT_EQ(g1.qubits[1], QubitIndex{1});
}

// sx / sxdg are recognized as qelib1 built-ins and map to SX / SXdg.
TEST(CircuitBuilder, SxSxdg_ParsesToSXGateTypes) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "sx q[0];\n"
        "sxdg q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — gate 0 is SX on qubit 0, gate 1 is SXdg on qubit 1.
    const GateOp& g0 = nth_gate(c, 0);
    EXPECT_TRUE(g0.type == GateType::SX);
    EXPECT_EQ(g0.qubits[0], QubitIndex{0});

    const GateOp& g1s = nth_gate(c, 1);
    EXPECT_TRUE(g1s.type == GateType::SXdg);
    EXPECT_EQ(g1s.qubits[0], QubitIndex{1});
}

TEST(CircuitBuilder, Cu1Parameter_PiOver2_EvaluatesCorrectly) {
    // Arrange — minimal QFT fragment exercising parameter evaluation
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "h q[0];\n"
        "cu1(pi/2) q[1],q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — cu1 gate parameter is pi/2 within 1e-12 (exact fold)
    const GateOp& cu1_gate = nth_gate(c, 1);
    EXPECT_TRUE(cu1_gate.type == GateType::CU1);
    EXPECT_NEAR(cu1_gate.params[0], M_PI / 2.0, 1e-12);
    // (tolerance: exact constant fold, not statistical)
}

TEST(CircuitBuilder, MultiQreg_QubitOffset_IsCorrect) {
    // Arrange — r[2] should be global qubit 4 (q occupies 0..1, r occupies 2..4)
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "qreg r[3];\n"
        "h q[1];\n"
        "h r[2];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert
    EXPECT_EQ(c.num_qubits(), QubitIndex{5});
    const GateOp& g0 = nth_gate(c, 0);   // h q[1] → global qubit 1
    EXPECT_EQ(g0.qubits[0], QubitIndex{1});
    const GateOp& g1 = nth_gate(c, 1);   // h r[2] → global qubit 4
    EXPECT_EQ(g1.qubits[0], QubitIndex{4});
}

// Regression: 4 qregs force std::vector<QregInfo> reallocation. Operating on
// the FIRST register after later decls previously read a dangling pointer
// (size 0 -> "index out of range [0,0)"). Mirrors q17_qec9xz/q20_qram.
TEST(CircuitBuilder, ManyQregs_FirstRegisterResolvesAfterRealloc) {
    // Arrange — addr[3] q0..2, rout[8] q3..10, ram[8] q11..18, qout[1] q19.
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg addr[3];\n"
        "qreg rout[8];\n"
        "qreg ram[8];\n"
        "qreg qout[1];\n"
        "h addr[0];\n"
        "cx addr[2],qout[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — first register still resolves (addr[0]=0), last resolves (qout[0]=19).
    EXPECT_EQ(c.num_qubits(), QubitIndex{20});
    const GateOp& g0 = nth_gate(c, 0);   // h addr[0] → global 0
    EXPECT_EQ(g0.qubits[0], QubitIndex{0});
    const GateOp& g1 = nth_gate(c, 1);   // cx addr[2],qout[0] → 2, 19
    EXPECT_EQ(g1.qubits[0], QubitIndex{2});
    EXPECT_EQ(g1.qubits[1], QubitIndex{19});
}

TEST(CircuitBuilder, MultiCreg_SeparateMeasures) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "creg a[1];\n"
        "creg b[1];\n"
        "measure q[0] -> a[0];\n"
        "measure q[1] -> b[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — two classical registers, two measures, correct cubit global indices
    EXPECT_EQ(c.classical_registers().size(), std::size_t{2});
    EXPECT_EQ(count_measure_ops(c), std::size_t{2});

    // creg a starts at offset 0, b at offset 1
    const auto& regs = c.classical_registers();
    EXPECT_EQ(regs[0].name, std::string{"a"});
    EXPECT_EQ(regs[0].offset, ClassicalBitIndex{0});
    EXPECT_EQ(regs[1].name, std::string{"b"});
    EXPECT_EQ(regs[1].offset, ClassicalBitIndex{1});
}

TEST(CircuitBuilder, BarrierEmpty_EmitsAll) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[3];\n"
        "barrier;\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — one BarrierOp with empty qubit list (means all qubits)
    EXPECT_EQ(count_barrier_ops(c), std::size_t{1});
    for (const auto& op : c.ops()) {
        if (const auto* b = std::get_if<BarrierOp>(&op)) {
            EXPECT_EQ(b->qubits.size(), std::size_t{0});
        }
    }
}

TEST(CircuitBuilder, BarrierExplicit_EmitsCorrectQubits) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[3];\n"
        "barrier q[0],q[2];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — BarrierOp with qubits {0, 2}
    EXPECT_EQ(count_barrier_ops(c), std::size_t{1});
    for (const auto& op : c.ops()) {
        if (const auto* b = std::get_if<BarrierOp>(&op)) {
            EXPECT_EQ(b->qubits.size(), std::size_t{2});
            EXPECT_EQ(b->qubits[0], QubitIndex{0});
            EXPECT_EQ(b->qubits[1], QubitIndex{2});
        }
    }
}

TEST(CircuitBuilder, Reset_EmitsResetGateOp) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "reset q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — one GateOp{Reset}
    EXPECT_EQ(count_gate_ops(c), std::size_t{1});
    const GateOp& g = nth_gate(c, 0);
    EXPECT_TRUE(g.type == GateType::Reset);
}

TEST(CircuitBuilder, IfThen_GateCall_EmitsIfOp) {
    // Arrange — if(c==3) cx q[0],q[1] with creg width 2
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "creg c[2];\n"
        "if(c==3) cx q[0],q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — one IfOp with correct value and CX body
    EXPECT_EQ(count_if_ops(c), std::size_t{1});
    for (const auto& op : c.ops()) {
        if (const auto* ifop = std::get_if<IfOp>(&op)) {
            EXPECT_EQ(ifop->value, std::uint64_t{3});
            EXPECT_TRUE(ifop->body.type == GateType::CX);
        }
    }
}

TEST(CircuitBuilder, UserGate_BellInline_ExpandsToHCX) {
    // Arrange — user gate "bell" expands to h + cx inline
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "gate bell a, b { h a; cx a, b; }\n"
        "qreg q[2];\n"
        "bell q[0], q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — inline expansion yields H(0) + CX(0,1)
    EXPECT_EQ(count_gate_ops(c), std::size_t{2});
    const GateOp& g0 = nth_gate(c, 0);
    EXPECT_TRUE(g0.type == GateType::H);
    EXPECT_EQ(g0.qubits[0], QubitIndex{0});
    const GateOp& g1 = nth_gate(c, 1);
    EXPECT_TRUE(g1.type == GateType::CX);
    EXPECT_EQ(g1.qubits[0], QubitIndex{0});
    EXPECT_EQ(g1.qubits[1], QubitIndex{1});
}

TEST(CircuitBuilder, OpaqueDecl_RegisteredOnCircuit) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "opaque myop a;\n"
        "qreg q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — opaque is declared on the circuit (DeclarationOnly body)
    EXPECT_TRUE(c.has_opaque_definition("myop"));
    EXPECT_EQ(c.opaque_definition("myop").num_qubits, QubitIndex{1});
}

TEST(CircuitBuilder, OpaqueCall_EmitsOpaqueGateOp) {
    // Arrange — opaque call creates GateOp{Opaque}
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "opaque myop a;\n"
        "qreg q[1];\n"
        "myop q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — one GateOp{Opaque} for the call
    EXPECT_EQ(count_gate_ops(c), std::size_t{1});
    const GateOp& g = nth_gate(c, 0);
    EXPECT_TRUE(g.type == GateType::Opaque);
    EXPECT_EQ(g.opaque_name, std::string{"myop"});
    EXPECT_TRUE(g.is_opaque);
}

// ═══════════════════════════════════════════════════════════════════
// 2. BOUNDARY CASES
// ═══════════════════════════════════════════════════════════════════

TEST(CircuitBuilder, EmptyCircuit_QregOnly_ZeroOps) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — valid circuit with no ops
    EXPECT_EQ(c.num_qubits(), QubitIndex{1});
    EXPECT_EQ(c.num_ops(), std::size_t{0});
}

TEST(CircuitBuilder, SingleQubit_SingleMeasure) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "creg c[1];\n"
        "measure q[0] -> c[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert
    EXPECT_EQ(c.num_qubits(), QubitIndex{1});
    EXPECT_EQ(count_measure_ops(c), std::size_t{1});
}

TEST(CircuitBuilder, Param_Pi_EvaluatesExact) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(pi) q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — parameter is exactly M_PI (exact constant fold)
    const GateOp& g = nth_gate(c, 0);
    EXPECT_NEAR(g.params[0], M_PI, 1e-15);
    // (tolerance: constant fold, not statistical)
}

TEST(CircuitBuilder, Param_NegPiOver2_EvaluatesExact) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(-pi/2) q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert
    const GateOp& g = nth_gate(c, 0);
    EXPECT_NEAR(g.params[0], -M_PI / 2.0, 1e-15);
    // (tolerance: exact constant fold)
}

TEST(CircuitBuilder, Param_Pow2to10_Evaluates1024) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(2^10) q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — 2^10 = 1024.0
    const GateOp& g = nth_gate(c, 0);
    EXPECT_NEAR(g.params[0], 1024.0, 1e-12);
    // (tolerance: exact integer power)
}

TEST(CircuitBuilder, Param_SinPiOver2_Evaluates1) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(sin(pi/2)) q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — sin(pi/2) = 1.0 within floating-point precision
    const GateOp& g = nth_gate(c, 0);
    EXPECT_NEAR(g.params[0], 1.0, 1e-15);
    // (tolerance: sin(pi/2) has negligible rounding error)
}

TEST(CircuitBuilder, Param_ExpZero_Evaluates1) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(exp(0)) q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — exp(0) = 1.0
    const GateOp& g = nth_gate(c, 0);
    EXPECT_NEAR(g.params[0], 1.0, 1e-15);
    // (tolerance: exact)
}

TEST(CircuitBuilder, AllSingleQubitCliffordT_EmitCorrectly) {
    // Arrange — all 9 Clifford+T single-qubit gates
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "id q[0]; x q[0]; y q[0]; z q[0]; h q[0];\n"
        "s q[0]; sdg q[0]; t q[0]; tdg q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — 9 gate ops in order
    EXPECT_EQ(count_gate_ops(c), std::size_t{9});
    EXPECT_TRUE(nth_gate(c, 0).type == GateType::I);
    EXPECT_TRUE(nth_gate(c, 1).type == GateType::X);
    EXPECT_TRUE(nth_gate(c, 2).type == GateType::Y);
    EXPECT_TRUE(nth_gate(c, 3).type == GateType::Z);
    EXPECT_TRUE(nth_gate(c, 4).type == GateType::H);
    EXPECT_TRUE(nth_gate(c, 5).type == GateType::S);
    EXPECT_TRUE(nth_gate(c, 6).type == GateType::Sdg);
    EXPECT_TRUE(nth_gate(c, 7).type == GateType::T);
    EXPECT_TRUE(nth_gate(c, 8).type == GateType::Tdg);
}

TEST(CircuitBuilder, AllRotationGates_EmitWithParams) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(0.1) q[0]; ry(0.2) q[0]; rz(0.3) q[0];\n"
        "u1(0.4) q[0]; u2(0.5,0.6) q[0]; u3(0.7,0.8,0.9) q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — 6 gate ops with correct param counts
    EXPECT_EQ(count_gate_ops(c), std::size_t{6});
    EXPECT_EQ(nth_gate(c, 0).params.size(), std::size_t{1});  // rx
    EXPECT_EQ(nth_gate(c, 1).params.size(), std::size_t{1});  // ry
    EXPECT_EQ(nth_gate(c, 2).params.size(), std::size_t{1});  // rz
    EXPECT_EQ(nth_gate(c, 3).params.size(), std::size_t{1});  // u1
    EXPECT_EQ(nth_gate(c, 4).params.size(), std::size_t{2});  // u2
    EXPECT_EQ(nth_gate(c, 5).params.size(), std::size_t{3});  // u3
}

TEST(CircuitBuilder, TwoQubitGates_AllVariants) {
    // Arrange — all 2Q gates from qelib1
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "cx q[0],q[1]; cy q[0],q[1]; cz q[0],q[1]; ch q[0],q[1];\n"
        "swap q[0],q[1]; crz(0.1) q[0],q[1]; cu1(0.2) q[0],q[1];\n"
        "cu3(0.3,0.4,0.5) q[0],q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — 8 gates
    EXPECT_EQ(count_gate_ops(c), std::size_t{8});
    EXPECT_TRUE(nth_gate(c, 0).type == GateType::CX);
    EXPECT_TRUE(nth_gate(c, 1).type == GateType::CY);
    EXPECT_TRUE(nth_gate(c, 2).type == GateType::CZ);
    EXPECT_TRUE(nth_gate(c, 3).type == GateType::CH);
    EXPECT_TRUE(nth_gate(c, 4).type == GateType::SWAP);
    EXPECT_TRUE(nth_gate(c, 5).type == GateType::CRZ);
    EXPECT_TRUE(nth_gate(c, 6).type == GateType::CU1);
    EXPECT_TRUE(nth_gate(c, 7).type == GateType::CU3);
}

TEST(CircuitBuilder, ThreeQubitGates_CCX_CSwap) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[3];\n"
        "ccx q[0],q[1],q[2];\n"
        "cswap q[0],q[1],q[2];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert
    EXPECT_EQ(count_gate_ops(c), std::size_t{2});
    EXPECT_TRUE(nth_gate(c, 0).type == GateType::CCX);
    EXPECT_TRUE(nth_gate(c, 1).type == GateType::CSwap);
}

TEST(CircuitBuilder, BuiltinU_AliasesU3) {
    // Arrange — built-in U gate must alias to U3 at parse time
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "U(0.1,0.2,0.3) q[0];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert — emitted as GateType::U3
    EXPECT_EQ(count_gate_ops(c), std::size_t{1});
    EXPECT_TRUE(nth_gate(c, 0).type == GateType::U3);
}

TEST(CircuitBuilder, BuiltinCX_AliasesCX) {
    // Arrange — built-in CX gate must alias to CX
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "CX q[0],q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);

    // Assert
    EXPECT_TRUE(nth_gate(c, 0).type == GateType::CX);
}

// ═══════════════════════════════════════════════════════════════════
// 3. ERROR CASES
// ═══════════════════════════════════════════════════════════════════

TEST(CircuitBuilder, UnknownFunction_ThrowsQASMParseError) {
    // Arrange — rx(foo(0)) where foo is undefined
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(foo(0)) q[0];\n";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse(qasm), QASMParseError);
}

TEST(CircuitBuilder, UnknownIdent_ThrowsQASMParseError) {
    // Arrange — rx with unknown identifier (not pi)
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(euler) q[0];\n";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse(qasm), QASMParseError);
}

TEST(CircuitBuilder, DivisionByZero_ThrowsQASMParseError) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(1/0) q[0];\n";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse(qasm), QASMParseError);
}

TEST(CircuitBuilder, SqrtNegative_ThrowsQASMParseError) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(sqrt(-1)) q[0];\n";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse(qasm), QASMParseError);
}

TEST(CircuitBuilder, LnNonPositive_ThrowsQASMParseError) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "rx(ln(0)) q[0];\n";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse(qasm), QASMParseError);
}

TEST(CircuitBuilder, ExternalInclude_ThrowsQASMParseError) {
    // Arrange — only qelib1.inc is built-in; other includes are rejected
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"other.inc\";\n"
        "qreg q[1];\n";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse(qasm), QASMParseError);
}

TEST(CircuitBuilder, IfBodyMeasure_ThrowsCircuitError) {
    // Arrange — if(...) measure is unsupported in Phase 3
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "creg c[1];\n"
        "if(c==0) measure q[0] -> c[0];\n";

    // Act + Assert — CircuitError because if_then() only accepts GateOp
    Parser p;
    EXPECT_THROW(p.parse(qasm), CircuitError);
}

TEST(CircuitBuilder, IfBodyReset_ThrowsCircuitError) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "creg c[1];\n"
        "if(c==0) reset q[0];\n";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse(qasm), CircuitError);
}

TEST(CircuitBuilder, InlineDepthExceeded_ThrowsQASMParseError) {
    // Arrange — 6 levels of mutual recursion exceeds kMaxInlineDepth=5.
    // Each level adds one gate that is a call to the next user gate.
    // SemanticValidator doesn't detect recursion; CircuitBuilder catches it.
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "gate g1 a { h a; }\n"
        "gate g2 a { g1 a; }\n"
        "gate g3 a { g2 a; }\n"
        "gate g4 a { g3 a; }\n"
        "gate g5 a { g4 a; }\n"
        "gate g6 a { g5 a; }\n"
        "qreg q[1];\n"
        "g6 q[0];\n";

    // Act + Assert — inline depth 6 exceeds the limit of 5
    Parser p;
    EXPECT_THROW(p.parse(qasm), QASMParseError);
}

TEST(CircuitBuilder, ParseFile_BellQasm_ReturnsCorrectCircuit) {
    // Arrange — use the canonical acceptance test file
    const std::string path = QPLAYER_ACCEPTANCE_DATA_DIR "/qasm/bell.qasm";

    // Act
    Parser p;
    Circuit c = p.parse_file(path);

    // Assert — same structure as inline Bell test
    EXPECT_EQ(c.num_qubits(), QubitIndex{2});
    EXPECT_EQ(count_gate_ops(c),    std::size_t{2});
    EXPECT_EQ(count_measure_ops(c), std::size_t{2});
}

TEST(CircuitBuilder, ParseFile_Missing_ThrowsQASMParseError) {
    // Arrange — non-existent path
    const std::string path = "/tmp/qplayer_nonexistent_XYZZY_test.qasm";

    // Act + Assert
    Parser p;
    EXPECT_THROW(p.parse_file(path), QASMParseError);
}

// ═══════════════════════════════════════════════════════════════════
// 4. ROUND-TRIP TESTS
// ═══════════════════════════════════════════════════════════════════

TEST(CircuitBuilder, BellRoundtrip_ToQasm_ContainsKeyTokens) {
    // Arrange — parse Bell, emit QASM, verify key tokens present
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[2];\n"
        "creg c[2];\n"
        "h q[0];\n"
        "cx q[0],q[1];\n"
        "measure q[0] -> c[0];\n"
        "measure q[1] -> c[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);
    std::string out = c.to_qasm();

    // Assert — key tokens present in QASM output (order may vary)
    EXPECT_TRUE(out.find("h q[0]") != std::string::npos);
    EXPECT_TRUE(out.find("cx q[0]") != std::string::npos);
    EXPECT_TRUE(out.find("measure") != std::string::npos);
}

TEST(CircuitBuilder, OpaqueRoundtrip_ToQasm_ContainsOpaqueDecl) {
    // Arrange — opaque round-trip: parse → to_qasm should emit opaque decl
    //
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "opaque myop a;\n"
        "qreg q[1];\n";

    // Act
    Parser p;
    Circuit c = p.parse(qasm);
    std::string out = c.to_qasm();

    // Assert — "opaque myop" appears in the output
    EXPECT_TRUE(out.find("opaque myop") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════
// 5. ACCEPTANCE QASM FILES — parse without error
// ═══════════════════════════════════════════════════════════════════

TEST(CircuitBuilder, AcceptanceQasm_Bell_Parseable) {
    Parser p;
    EXPECT_NO_THROW(p.parse_file(
        QPLAYER_ACCEPTANCE_DATA_DIR "/qasm/bell.qasm"));
}

TEST(CircuitBuilder, AcceptanceQasm_Ghz5_Parseable) {
    Parser p;
    EXPECT_NO_THROW(p.parse_file(
        QPLAYER_ACCEPTANCE_DATA_DIR "/qasm/ghz_5.qasm"));
}

TEST(CircuitBuilder, AcceptanceQasm_Qft4_Parseable) {
    Parser p;
    EXPECT_NO_THROW(p.parse_file(
        QPLAYER_ACCEPTANCE_DATA_DIR "/qasm/qft_4.qasm"));
}
