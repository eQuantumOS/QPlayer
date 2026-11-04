// test_circuit_draw.cpp -- Circuit::draw text diagram tests.
//
// draw() is a pure IR walk (no backend), so these tests construct circuits with
// the public builder and assert structural properties of the rendered text:
// wire labels, gate boxes, control markers, the classical wire, style switching
// (Unicode vs Ascii glyphs), and QRegister::draw delegation. Exact byte-for-byte
// snapshots are avoided so cosmetic spacing tweaks do not break the suite; the
// assertions instead pin the invariants a user relies on.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/qregister.hpp"

using qplayer::Circuit;
using qplayer::CircuitDrawStyle;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::QRegister;
using qplayer::SimType;

namespace {
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
int count_lines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}
}  // namespace

TEST(CircuitDraw, EmptyCircuit_ReturnsPlaceholder) {
    // Arrange
    Circuit circ(0, 0);

    // Act
    std::string text = circ.draw();

    // Assert
    EXPECT_TRUE(contains(text, "empty circuit"));
}

TEST(CircuitDraw, BellState_Unicode_ShowsWiresGatesAndClassical) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act
    std::string text = circ.draw();  // Unicode default

    // Assert
    EXPECT_TRUE(contains(text, "q0:"));     // qubit wire labels
    EXPECT_TRUE(contains(text, "q1:"));
    EXPECT_TRUE(contains(text, "c:"));      // classical wire present (measures)
    EXPECT_TRUE(contains(text, "[H]"));     // Hadamard box
    EXPECT_TRUE(contains(text, "[X]"));     // cx target box
    EXPECT_TRUE(contains(text, "[M]"));     // measurement box
    EXPECT_TRUE(contains(text, "\xE2\x96\xA0"));  // control marker U+25A0 (=)
    EXPECT_TRUE(contains(text, "\xE2\x94\x80"));  // horizontal wire U+2500
}

TEST(CircuitDraw, BellState_Ascii_UsesAsciiGlyphsOnly) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    // Act
    std::string text = circ.draw(CircuitDrawStyle::Ascii);

    // Assert
    EXPECT_TRUE(contains(text, "-"));   // ascii wire
    EXPECT_TRUE(contains(text, "*"));   // ascii control marker
    EXPECT_TRUE(contains(text, "="));   // ascii classical wire
    EXPECT_TRUE(contains(text, "[X]"));
    // No unicode box-drawing bytes (U+2500 lead byte 0xE2) in Ascii mode.
    EXPECT_TRUE(text.find("\xE2") == std::string::npos);
}

TEST(CircuitDraw, Toffoli_ShowsTwoControlsAndXTarget) {
    // Arrange
    Circuit circ(3, 0);
    circ.ccx(0, 1, 2);

    // Act
    std::string text = circ.draw();

    // Assert
    EXPECT_TRUE(contains(text, "[X]"));   // Toffoli target labelled X, not CCX
    EXPECT_FALSE(contains(text, "CCX"));
    // Two control markers (U+25A0) on the control wires.
    std::size_t first = text.find("\xE2\x96\xA0");
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(text.find("\xE2\x96\xA0", first + 1) != std::string::npos);
}

TEST(CircuitDraw, RotationGate_ShowsParameter) {
    // Arrange
    Circuit circ(1, 0);
    circ.rx(1.5708, 0);

    // Act
    std::string text = circ.draw();

    // Assert
    EXPECT_TRUE(contains(text, "RX"));
    EXPECT_TRUE(contains(text, "1.5708"));
}

TEST(CircuitDraw, Conditional_DrawsClassicalWireWithValue) {
    // Arrange
    Circuit circ(2, 2);
    GateOp x;
    x.type = GateType::X;
    x.name = "x";
    x.qubits = {1};
    circ.h(0).if_then(circ.creg("c"), 1, x);

    // Act
    std::string text = circ.draw();

    // Assert
    EXPECT_TRUE(contains(text, "c:"));     // classical wire for the condition
    EXPECT_TRUE(contains(text, "=1"));     // compared value annotation
    EXPECT_TRUE(contains(text, "[X]"));    // conditioned body gate
}

TEST(CircuitDraw, RowCount_MatchesQubitsPlusClassical) {
    // Arrange: 3 qubits, with measures -> 2*3 wire/connector rows + classical.
    Circuit circ(3, 3);
    circ.h(0).measure(0, 0);

    // Act
    std::string text = circ.draw();

    // Assert: rows = 2*n + 1 (classical present) => 7 trailing newlines.
    EXPECT_EQ(count_lines(text), 7);
}

TEST(CircuitDraw, QRegisterDraw_DelegatesToCircuit) {
    // Arrange
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    QRegister qr(SimType::FullState);

    // Act
    std::string via_circuit  = circ.draw();
    std::string via_register = qr.draw(circ);

    // Assert: QRegister::draw is a pure delegate.
    EXPECT_EQ(via_register, via_circuit);
}
