// test_parser_broadcast.cpp — Unit tests for whole-register gate broadcast
// in the OpenQASM v2 parser pipeline (SemanticValidator + CircuitBuilder).
//
// OpenQASM v2 broadcast semantics:
//   h q;          (qreg q[N]) → h q[0]; h q[1]; ... h q[N-1];
//   cx q, r;      (both size N) → cx q[i],r[i] for i in [0,N).
//   cx q[0], r;   (r size N)   → cx q[0],r[i] for i in [0,N).
// Constraint: multiple whole-register args must share the same size.
//
// Tests:
//   1. Single-register broadcast: h q; (qreg q[3]) → 3 H gates.
//   2. Two-register broadcast: cx q, r; (size 3 each) → 3 CX gates.
//   3. Mixed broadcast: cx q[0], r; (r size 3) → 3 CX gates.
//   4. Size mismatch: cx q, r; (q[3],r[2]) → QASMParseError.
//   5. Indexed call is unchanged: h q[1]; → 1 H gate (regression).
//   6. Broadcast N=1: h q; (qreg q[1]) → 1 H gate.

#include "test_case.hpp"
#include "assertions.hpp"

#include <string>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "error.hpp"
#include "parser/parser.hpp"
#include "types.hpp"

using namespace qplayer;

// ── Helper: parse QASM text into a Circuit via the public Parser facade ───────

static Circuit parse_qasm(const std::string& src) {
    Parser p;
    return p.parse(src);
}

// ── Helper: count GateOps in a circuit ────────────────────────────────────────

static std::size_t count_gate_ops(const Circuit& c) {
    std::size_t n = 0;
    for (const auto& op : c.ops()) {
        if (std::holds_alternative<GateOp>(op)) ++n;
    }
    return n;
}

// ── Helper: return the i-th GateOp (0-based) ─────────────────────────────────

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

// ═══════════════════════════════════════════════════════════════════════════
// 1. Single-register broadcast: h q; with qreg q[3] → 3 H GateOps
// ═══════════════════════════════════════════════════════════════════════════

TEST(Broadcast, SingleReg_H_Size3_Produces3HGates) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[3];\n"
        "h q;\n";

    // Act
    Circuit c = parse_qasm(qasm);

    // Assert: exactly 3 H gates emitted, qubit indices 0, 1, 2.
    EXPECT_EQ(count_gate_ops(c), std::size_t{3});

    const GateOp& g0 = nth_gate(c, 0);
    const GateOp& g1 = nth_gate(c, 1);
    const GateOp& g2 = nth_gate(c, 2);

    EXPECT_TRUE(g0.type == GateType::H);
    EXPECT_EQ(g0.qubits.size(), std::size_t{1});
    EXPECT_EQ(g0.qubits[0], QubitIndex{0});

    EXPECT_TRUE(g1.type == GateType::H);
    EXPECT_EQ(g1.qubits[0], QubitIndex{1});

    EXPECT_TRUE(g2.type == GateType::H);
    EXPECT_EQ(g2.qubits[0], QubitIndex{2});
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Two-register broadcast: cx q, r; (both size 3) → 3 CX gates
// ═══════════════════════════════════════════════════════════════════════════

TEST(Broadcast, TwoReg_CX_Size3_Produces3CXGates) {
    // Arrange: q[3] and r[3] are declared after q, so r has offset 3.
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[3];\n"
        "qreg r[3];\n"
        "cx q, r;\n";

    // Act
    Circuit c = parse_qasm(qasm);

    // Assert: 3 CX gates: cx q[i], r[i] for i in {0,1,2}.
    EXPECT_EQ(count_gate_ops(c), std::size_t{3});

    for (std::size_t i = 0; i < 3; ++i) {
        const GateOp& g = nth_gate(c, i);
        EXPECT_TRUE(g.type == GateType::CX);
        EXPECT_EQ(g.qubits.size(), std::size_t{2});
        // q[i] → global index i (q starts at offset 0)
        EXPECT_EQ(g.qubits[0], static_cast<QubitIndex>(i));
        // r[i] → global index 3+i (r starts at offset 3)
        EXPECT_EQ(g.qubits[1], static_cast<QubitIndex>(3 + i));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Mixed broadcast: cx q[0], r; (r size 3) → 3 CX gates with q[0] fixed
// ═══════════════════════════════════════════════════════════════════════════

TEST(Broadcast, Mixed_CXScalarWholeReg_Size3_Produces3CXGates) {
    // Arrange: q[1] (scalar), r[3] (whole-register).
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "qreg r[3];\n"
        "cx q[0], r;\n";

    // Act
    Circuit c = parse_qasm(qasm);

    // Assert: 3 CX gates — q[0] repeated, r[i] varies.
    EXPECT_EQ(count_gate_ops(c), std::size_t{3});

    for (std::size_t i = 0; i < 3; ++i) {
        const GateOp& g = nth_gate(c, i);
        EXPECT_TRUE(g.type == GateType::CX);
        EXPECT_EQ(g.qubits.size(), std::size_t{2});
        // q[0] → global index 0 (q offset=0, index=0)
        EXPECT_EQ(g.qubits[0], QubitIndex{0});
        // r[i] → global index 1+i (r starts at offset 1, after q[1])
        EXPECT_EQ(g.qubits[1], static_cast<QubitIndex>(1 + i));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. Size mismatch: cx q, r; (q[3], r[2]) → QASMParseError
// ═══════════════════════════════════════════════════════════════════════════

TEST(Broadcast, SizeMismatch_CX_Q3_R2_ThrowsParseError) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[3];\n"
        "qreg r[2];\n"
        "cx q, r;\n";

    // Act + Assert: SemanticValidator must reject this.
    EXPECT_THROW(parse_qasm(qasm), qplayer::QASMParseError);
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. Indexed call regression: h q[1]; → exactly 1 H gate (no broadcast)
// ═══════════════════════════════════════════════════════════════════════════

TEST(Broadcast, IndexedCall_HQ1_ProducesSingleHGate) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[3];\n"
        "h q[1];\n";

    // Act
    Circuit c = parse_qasm(qasm);

    // Assert: exactly 1 H gate on qubit index 1.
    EXPECT_EQ(count_gate_ops(c), std::size_t{1});
    const GateOp& g = nth_gate(c, 0);
    EXPECT_TRUE(g.type == GateType::H);
    EXPECT_EQ(g.qubits[0], QubitIndex{1});
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. Single-element register broadcast: h q; (qreg q[1]) → 1 H gate
// ═══════════════════════════════════════════════════════════════════════════

TEST(Broadcast, SingleElementReg_H_Size1_Produces1HGate) {
    // Arrange
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[1];\n"
        "h q;\n";

    // Act
    Circuit c = parse_qasm(qasm);

    // Assert: exactly 1 H gate on qubit 0.
    EXPECT_EQ(count_gate_ops(c), std::size_t{1});
    const GateOp& g = nth_gate(c, 0);
    EXPECT_TRUE(g.type == GateType::H);
    EXPECT_EQ(g.qubits[0], QubitIndex{0});
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. oracle_20 core: h q; (qreg q[20]) → 20 H gates (oracle_20.qasm motivator)
// ═══════════════════════════════════════════════════════════════════════════

TEST(Broadcast, OracleCore_H_Size20_Produces20HGates) {
    // Arrange: minimal oracle_20 prelude — just the h q; broadcast.
    const std::string qasm =
        "OPENQASM 2.0;\n"
        "include \"qelib1.inc\";\n"
        "qreg q[20];\n"
        "h q;\n";

    // Act
    Circuit c = parse_qasm(qasm);

    // Assert: 20 H gates, each on qubit i for i in [0,20).
    EXPECT_EQ(count_gate_ops(c), std::size_t{20});
    for (std::size_t i = 0; i < 20; ++i) {
        const GateOp& g = nth_gate(c, i);
        EXPECT_TRUE(g.type == GateType::H);
        EXPECT_EQ(g.qubits[0], static_cast<QubitIndex>(i));
    }
}
