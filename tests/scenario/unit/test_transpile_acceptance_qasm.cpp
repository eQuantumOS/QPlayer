// test_transpile_acceptance_qasm.cpp — Integration tests: parse acceptance QASM
// files and run Transpiler::transpile(level=1) on each.
//
// For each of the 9 acceptance QASM files the test verifies:
//   1. Parser::parse_file() succeeds (or fails with a known exception).
//   2. Transpiler::transpile(circuit, 1) succeeds without exception.
//   3. The resulting circuit has the expected num_ops (pre-measured, fixed).
//
// Expected num_ops were measured empirically by running the Level-1 pipeline
// during development and are anchored here as regression guards.
//
// oracle_20.qasm uses whole-register gate syntax ("h q;" without index), now
// supported via broadcast semantics (OpenQASM v2). The circuit parses
// 275 ops and transpile level 1 does not reduce it (no adjacent inverse pairs).
//
// QASM file paths use the QPLAYER_ACCEPTANCE_DATA_DIR compile-time macro
// (set by tests/CMakeLists.txt to the absolute path of tests/).
// Parser includes: parser/parser.hpp, transpiler/transpiler.hpp.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstddef>
#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/parser/parser.hpp"
#include "../../../include/transpiler/transpiler.hpp"

using qplayer::Circuit;
using qplayer::QASMParseError;
using qplayer::Transpiler;

// Convenience macro: build the absolute path to a QASM file.
#define QASM_PATH(name) QPLAYER_ACCEPTANCE_DATA_DIR "/qasm/" name

// ── Case 1: opt_cancel.qasm — Level 1 collapses 4 inverse pairs, lone H survives.
// Measured: before=9 ops (1 lone H + 4 pairs of 2), after=1 op (lone H).

TEST(TranspileAcceptanceQasm, OptCancel_Level1_OneOpRemains) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("opt_cancel.qasm"));

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: only the lone H survives all cancellations.
    // (9 ops before -> 1 op after, consistent with test_transpiler.cpp)
    EXPECT_EQ(out.num_ops(), std::size_t{1});
}

// ── Case 2: bell.qasm — H + CX + measure + measure — no reduction at Level 1.
// Measured: before=4 ops, after=4 ops.

TEST(TranspileAcceptanceQasm, Bell_Level1_FourOpsPreserved) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("bell.qasm"));
    const std::size_t before = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 4 ops (H, CX, measure×2) — no pairs to cancel in Level 1.
    EXPECT_EQ(before, std::size_t{4});
    EXPECT_EQ(out.num_ops(), std::size_t{4});
}

// ── Case 3: ghz_5.qasm — H + 4 CX + 5 measure — no reduction at Level 1.
// Measured: before=10 ops, after=10 ops.

TEST(TranspileAcceptanceQasm, Ghz5_Level1_TenOpsPreserved) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("ghz_5.qasm"));
    const std::size_t before = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 10 ops (1 H + 4 CX + 5 measure) — no inversions to cancel.
    EXPECT_EQ(before, std::size_t{10});
    EXPECT_EQ(out.num_ops(), std::size_t{10});
}

// ── Case 4: qft_4.qasm — 4-qubit QFT: H/cu1/swap gates — no reduction at Level 1.
// Measured: before=12 ops, after=12 ops.

TEST(TranspileAcceptanceQasm, Qft4_Level1_TwelveOpsPreserved) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("qft_4.qasm"));
    const std::size_t before = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 12 ops (4 H + 6 CU1 + 2 SWAP) — no adjacent inverse pairs.
    EXPECT_EQ(before, std::size_t{12});
    EXPECT_EQ(out.num_ops(), std::size_t{12});
}

// ── Case 5: oracle_10.qasm — 10-qubit mixed gate circuit — no reduction at Level 1.
// Measured: before=116 ops, after=116 ops.
// Level-1 passes do not reduce this circuit because no adjacent inverse pairs exist.

TEST(TranspileAcceptanceQasm, Oracle10_Level1_NoReduction) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("oracle_10.qasm"));
    const std::size_t before = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 116 ops before and after — diverse gate mix prevents Level-1 cancellation.
    EXPECT_EQ(before, std::size_t{116});
    EXPECT_EQ(out.num_ops(), std::size_t{116});
}

// ── Case 6: oracle_20.qasm — uses whole-register syntax "h q;" (without index).
// Broadcast semantics (OpenQASM v2) expand "h q;" with qreg q[20]
// 20 individual H gates. Measured: before=275 ops, after=275 ops.
// Level-1 transpile does not reduce this circuit (no adjacent inverse pairs).

TEST(TranspileAcceptanceQasm, Oracle20_BroadcastSyntax_ParsesAndTranspiles) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("oracle_20.qasm"));
    const std::size_t before = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 275 ops before and after — diverse gate mix prevents Level-1 cancellation.
    EXPECT_EQ(before, std::size_t{275});
    EXPECT_EQ(out.num_ops(), std::size_t{275});
}

// ── Case 7: sparse_100q.qasm — 100-qubit GHZ-like — no reduction at Level 1.
// Measured: before=200 ops, after=200 ops.
// Verifies that the Level-1 pipeline handles large circuits without exception.

TEST(TranspileAcceptanceQasm, Sparse100q_Level1_TwoHundredOpsPreserved) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("sparse_100q.qasm"));
    const std::size_t before = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 200 ops (1 H + 99 CX + 100 measure) — no inversions to cancel.
    EXPECT_EQ(before, std::size_t{200});
    EXPECT_EQ(out.num_ops(), std::size_t{200});
}

// ── Case 8: noisy_bell.qasm — same gate topology as bell.qasm — no reduction.
// Measured: before=4 ops, after=4 ops.
// The noise model is injected at runtime (Phase 8); the circuit itself is clean.

TEST(TranspileAcceptanceQasm, NoisyBell_Level1_FourOpsPreserved) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("noisy_bell.qasm"));
    const std::size_t before = c.num_ops();

    // Act
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 4 ops (H, CX, measure×2) — same topology as bell.qasm, no reduction.
    EXPECT_EQ(before, std::size_t{4});
    EXPECT_EQ(out.num_ops(), std::size_t{4});
}

// ── Case 9: compile_swap.qasm — 4-qubit circuit with non-adjacent CX.
// Measured: before=8 ops, after=8 ops.
// No device_json supplied -> routing pass not active; circuit unchanged by Level 1.

TEST(TranspileAcceptanceQasm, CompileSwap_Level1_NoRouting_EightOpsPreserved) {
    // Arrange
    qplayer::Parser p;
    Circuit c = p.parse_file(QASM_PATH("compile_swap.qasm"));
    const std::size_t before = c.num_ops();

    // Act: Level 1 without device_json -> no SWAP routing, no topological change.
    Transpiler t;
    Circuit out = t.transpile(std::move(c), 1);

    // Assert: 8 ops (H + CX + X×2 + measure×4) — no adjacent inverse pairs.
    EXPECT_EQ(before, std::size_t{8});
    EXPECT_EQ(out.num_ops(), std::size_t{8});
}
