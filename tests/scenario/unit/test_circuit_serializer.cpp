// test_circuit_serializer.cpp -- Phase 2 round-trip + QASM emission tests
//
//
// Round-trip principle: a Circuit serialised via to_json() and
// then re-parsed via from_json() must compare equal across every observable
// surface: num_qubits, classical_registers, opaque_definitions (including
// the 3-variant body discriminator), ops stream, and CircuitMetadata.
//
// QASM emission: the produced text follows the
// emission table -- header lines, qreg/creg declarations, gates, measure
// arrow, barrier, conditional, reset. Kraus opaque throws (section C).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <complex>
#include <string>
#include <variant>
#include <vector>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/detail/json_value.hpp"
#include "../../../include/error.hpp"

using qplayer::Circuit;
using qplayer::CircuitError;
using qplayer::ClassicalBitIndex;
using qplayer::DeclarationOnly;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::KrausChannel;
using qplayer::QubitIndex;
using qplayer::UnitaryMatrix;
using qplayer::detail::JsonValue;
using qplayer::detail::parse_json;
using qplayer::detail::write_json;

// ─────────────────────────────────────────────────────────────────────────────
// JSON helper sanity (parser + writer round-trip)
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonValue, RoundTrip_PrimitivesAndContainers) {
    const std::string src = R"({
  "a": null,
  "b": true,
  "c": 42,
  "d": -1.5,
  "e": "hello \"world\"",
  "f": [1, 2, 3],
  "g": { "x": false }
})";

    const JsonValue v = parse_json(src);
    EXPECT_TRUE(v.is_object());
    EXPECT_TRUE(v.at("a").is_null());
    EXPECT_TRUE(v.at("b").as_bool());
    EXPECT_EQ(v.at("c").as_int(), static_cast<std::int64_t>(42));
    EXPECT_NEAR(v.at("d").as_double(), -1.5, 1e-15);
    EXPECT_EQ(v.at("e").as_string(), std::string{"hello \"world\""});
    EXPECT_EQ(v.at("f").as_array().size(), static_cast<std::size_t>(3));
    EXPECT_FALSE(v.at("g").at("x").as_bool());

    // Re-emit + re-parse: structurally equivalent.
    const std::string emitted = write_json(v, /*pretty=*/true);
    const JsonValue   reparsed = parse_json(emitted);
    EXPECT_EQ(reparsed.at("c").as_int(), static_cast<std::int64_t>(42));
}

TEST(JsonValue, SyntaxError_CarriesOffset) {
    bool threw = false;
    std::string msg;
    try {
        parse_json("{\"x\": ,}");
    } catch (const CircuitError& e) {
        threw = true;
        msg = e.what();
    }
    EXPECT_TRUE(threw);
    EXPECT_TRUE(msg.find("offset") != std::string::npos);
}

TEST(JsonValue, RejectsTrailingComma) {
    EXPECT_THROW(parse_json("[1, 2, 3,]"),  CircuitError);
    EXPECT_THROW(parse_json("{\"a\": 1,}"), CircuitError);
}

TEST(JsonValue, RejectsNaNInfLiterals) {
    EXPECT_THROW(parse_json("NaN"),  CircuitError);
    EXPECT_THROW(parse_json("[Infinity]"), CircuitError);
}

// ─────────────────────────────────────────────────────────────────────────────
// Circuit JSON round-trip
// ─────────────────────────────────────────────────────────────────────────────

// Helper: compare two Circuits across the observable surface so the
// individual round-trip tests stay readable.
void expect_circuits_equal(const Circuit& a, const Circuit& b) {
    EXPECT_EQ(a.num_qubits(), b.num_qubits());
    EXPECT_EQ(a.num_ops(),    b.num_ops());
    EXPECT_EQ(a.classical_registers().size(), b.classical_registers().size());
    EXPECT_EQ(a.opaque_definitions().size(),  b.opaque_definitions().size());

    for (std::size_t i = 0; i < a.classical_registers().size(); ++i) {
        EXPECT_EQ(a.classical_registers()[i].name,   b.classical_registers()[i].name);
        EXPECT_EQ(a.classical_registers()[i].width,  b.classical_registers()[i].width);
        EXPECT_EQ(a.classical_registers()[i].offset, b.classical_registers()[i].offset);
    }
    for (std::size_t i = 0; i < a.opaque_definitions().size(); ++i) {
        EXPECT_EQ(a.opaque_definitions()[i].name,       b.opaque_definitions()[i].name);
        EXPECT_EQ(a.opaque_definitions()[i].num_qubits, b.opaque_definitions()[i].num_qubits);
        EXPECT_EQ(a.opaque_definitions()[i].body.index(),
                  b.opaque_definitions()[i].body.index());
    }
    // Op index equivalence -- the variant indices match alternative-by-
    // alternative. Deep-field equality is covered by the per-Op round-trip
    // tests below.
    for (std::size_t i = 0; i < a.num_ops(); ++i) {
        EXPECT_EQ(a.ops()[i].index(), b.ops()[i].index());
    }
}

TEST(CircuitSerializer, RoundTrip_EmptyCircuit) {
    Circuit a(0, 0);
    Circuit b = Circuit::from_json(a.to_json());
    expect_circuits_equal(a, b);
}

TEST(CircuitSerializer, RoundTrip_BellPair_PreservesAllOps) {
    Circuit a(2, 2);
    a.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    Circuit b = Circuit::from_json(a.to_json());
    expect_circuits_equal(a, b);

    // Spot-check that the GateOp body survived (type + qubits + name).
    const auto* g0 = std::get_if<GateOp>(&b.ops()[0]);
    EXPECT_TRUE(g0 != nullptr);
    if (g0) {
        EXPECT_TRUE(g0->type == GateType::H);
        EXPECT_EQ(g0->name, std::string{"h"});
        EXPECT_EQ(g0->qubits.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(g0->qubits[0], static_cast<QubitIndex>(0));
    }
}

TEST(CircuitSerializer, RoundTrip_RotationParamsBitExact) {
    Circuit a(1, 0);
    a.rx(0.7853981633974483, 0);     // pi/4
    a.u3(1.0, 2.0, 3.0, 0);

    Circuit b = Circuit::from_json(a.to_json());
    const auto* rx = std::get_if<GateOp>(&b.ops()[0]);
    EXPECT_TRUE(rx != nullptr);
    if (rx) {
        EXPECT_EQ(rx->params.size(), static_cast<std::size_t>(1));
        EXPECT_NEAR(rx->params[0], 0.7853981633974483, 0.0);  // bit-exact via setprecision(17).
    }
    const auto* u3 = std::get_if<GateOp>(&b.ops()[1]);
    EXPECT_TRUE(u3 != nullptr);
    if (u3) {
        EXPECT_EQ(u3->params.size(), static_cast<std::size_t>(3));
        EXPECT_NEAR(u3->params[0], 1.0, 0.0);
        EXPECT_NEAR(u3->params[1], 2.0, 0.0);
        EXPECT_NEAR(u3->params[2], 3.0, 0.0);
    }
}

TEST(CircuitSerializer, RoundTrip_AllNonGateAlternatives) {
    Circuit a(2, 2);
    a.barrier({0, 1});
    GateOp body;
    body.type = GateType::X;
    body.name = "x";
    body.qubits.push_back(0);
    a.if_then(0, 1, body);
    a.reset(1);

    Circuit b = Circuit::from_json(a.to_json());
    expect_circuits_equal(a, b);
    EXPECT_EQ(b.ops()[0].index(), static_cast<std::size_t>(2));  // BarrierOp
    EXPECT_EQ(b.ops()[1].index(), static_cast<std::size_t>(3));  // IfOp
    EXPECT_EQ(b.ops()[2].index(), static_cast<std::size_t>(0));  // GateOp (Reset)
}

TEST(CircuitSerializer, RoundTrip_IdleOpPreservedInJson) {
    // Builders never emit IdleOp; we materialise one via the payload
    // constructor by writing JSON directly and parsing it back.
    const std::string src = R"({
  "version": 1,
  "num_qubits": 1,
  "classical_registers": [],
  "opaque_definitions": [],
  "metadata": {"name": ""},
  "ops": [{"type": "idle", "qubit": 0, "duration_ns": 12.5}]
})";

    Circuit c = Circuit::from_json(src);
    EXPECT_EQ(c.num_ops(), static_cast<std::size_t>(1));
    const auto* idle = std::get_if<qplayer::IdleOp>(&c.ops()[0]);
    EXPECT_TRUE(idle != nullptr);
    if (idle) {
        EXPECT_EQ(idle->qubit, static_cast<QubitIndex>(0));
        EXPECT_NEAR(idle->duration_ns, 12.5, 1e-15);
    }

    // Re-emit -> the JSON keeps the idle node.
    const std::string out = c.to_json();
    EXPECT_TRUE(out.find("\"idle\"") != std::string::npos);
    EXPECT_TRUE(out.find("\"duration_ns\"") != std::string::npos);
}

TEST(CircuitSerializer, RoundTrip_OpaqueDeclarationOnlyAndMatrix) {
    Circuit a(2, 0);
    a.declare_opaque("u_stub", 1);

    std::array<std::array<std::complex<double>, 2>, 2> id1{};
    id1[0][0] = {1.0, 0.0}; id1[1][1] = {1.0, 0.0};
    a.define_opaque("u_real", id1);

    Circuit b = Circuit::from_json(a.to_json());
    EXPECT_TRUE(b.has_opaque_definition("u_stub"));
    EXPECT_TRUE(b.has_opaque_definition("u_real"));
    EXPECT_TRUE(std::holds_alternative<DeclarationOnly>(b.opaque_definition("u_stub").body));
    EXPECT_TRUE(std::holds_alternative<UnitaryMatrix>(b.opaque_definition("u_real").body));
}

TEST(CircuitSerializer, RoundTrip_OpaqueKrausChannelPreservesOperators) {
    Circuit a(1, 0);
    KrausChannel ch;
    const double s = 1.0 / std::sqrt(2.0);
    UnitaryMatrix k(2, std::vector<std::complex<double>>(2));
    k[0][0] = {s, 0.0}; k[1][1] = {s, 0.0};
    ch.operators.push_back(k);
    ch.operators.push_back(k);
    a.define_opaque("noise_id", ch);

    Circuit b = Circuit::from_json(a.to_json());
    EXPECT_TRUE(b.has_opaque_definition("noise_id"));
    const auto& def = b.opaque_definition("noise_id");
    EXPECT_TRUE(std::holds_alternative<KrausChannel>(def.body));
    if (std::holds_alternative<KrausChannel>(def.body)) {
        EXPECT_EQ(std::get<KrausChannel>(def.body).operators.size(), static_cast<std::size_t>(2));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Error cases: malformed input must throw CircuitError("circuit.json: ...")
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitSerializer, FromJson_MissingNumQubits_Throws) {
    EXPECT_THROW(Circuit::from_json("{\"version\": 1, \"ops\": []}"), CircuitError);
}

TEST(CircuitSerializer, FromJson_UnknownBodyType_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "opaque_definitions": [{"name": "u", "num_qubits": 1, "body": {"type": "potato"}}],
  "ops": []
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_UnknownOpType_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "ops": [{"type": "warp_drive"}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_NonObjectRoot_Throws) {
    EXPECT_THROW(Circuit::from_json("[1, 2, 3]"), CircuitError);
}

TEST(CircuitSerializer, LoadJsonFile_MissingFile_Throws) {
    EXPECT_THROW(Circuit::load_json_file("/tmp/nonexistent_qplayer_xyz.json"),
                 CircuitError);
}

// ─────────────────────────────────────────────────────────────────────────────
// Semantic validation of decoded payloads (audit #2, Phase 8-F)
//
// from_json / load_json_file are public Python API and used to feed indices
// straight into detail_make_circuit_from_payload, an explicit validation-bypass
// bridge. An out-of-range cubit reached result.cpp cubit_vals[...] and produced a
// heap OOB write -> SIGSEGV. Every check below must reject before construction.
// ─────────────────────────────────────────────────────────────────────────────

// The exact reported repro: a Bell circuit whose last measure cubit is rewritten
// to 1000000 while total_cubits == 2.
TEST(CircuitSerializer, FromJson_MeasureCubitOutOfRange_Throws) {
    const std::string src = R"({
  "num_qubits": 2,
  "classical_registers": [{"name": "c", "width": 2, "offset": 0}],
  "ops": [{"type": "measure", "qubit": 1, "cubit": 1000000}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_MeasureQubitOutOfRange_Throws) {
    const std::string src = R"({
  "num_qubits": 2,
  "classical_registers": [{"name": "c", "width": 2, "offset": 0}],
  "ops": [{"type": "measure", "qubit": 7, "cubit": 0}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_GateQubitOutOfRange_Throws) {
    const std::string src = R"({
  "num_qubits": 2,
  "ops": [{"type": "gate", "name": "h", "gate_type": 0, "qubits": [5], "params": []}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

// cx q[1],q[1] is rejected at builder time (check_distinct); JSON must match.
TEST(CircuitSerializer, FromJson_MultiQubitGateSameQubitTwice_Throws) {
    const std::string src = R"({
  "num_qubits": 2,
  "ops": [{"type": "gate", "name": "cx", "gate_type": 12, "qubits": [1, 1], "params": []}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_BarrierQubitOutOfRange_Throws) {
    const std::string src = R"({
  "num_qubits": 2,
  "ops": [{"type": "barrier", "qubits": [0, 9]}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_IdleQubitOutOfRange_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "ops": [{"type": "idle", "qubit": 4, "duration_ns": 10.0}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_IfCregIndexOutOfRange_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "classical_registers": [{"name": "c", "width": 1, "offset": 0}],
  "ops": [{"type": "if", "creg": 3, "value": 1,
           "body": {"type": "gate", "name": "x", "gate_type": 1, "qubits": [0], "params": []}}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

// The if body is a GateOp too and gets the same range check.
TEST(CircuitSerializer, FromJson_IfBodyQubitOutOfRange_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "classical_registers": [{"name": "c", "width": 1, "offset": 0}],
  "ops": [{"type": "if", "creg": 0, "value": 1,
           "body": {"type": "gate", "name": "x", "gate_type": 1, "qubits": [6], "params": []}}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

// A bogus offset would silently re-point resolve_cbit() at the wrong global cubit,
// so offsets must match the cumulative layout add_creg() builds.
TEST(CircuitSerializer, FromJson_CregOffsetNotCumulative_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "classical_registers": [{"name": "a", "width": 2, "offset": 0},
                          {"name": "b", "width": 1, "offset": 7}],
  "ops": []
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_CregZeroWidth_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "classical_registers": [{"name": "a", "width": 0, "offset": 0}],
  "ops": []
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_DuplicateCregName_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "classical_registers": [{"name": "a", "width": 1, "offset": 0},
                          {"name": "a", "width": 1, "offset": 1}],
  "ops": []
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_ExpectationTargetInvalidChar_Throws) {
    const std::string src = R"({
  "num_qubits": 2,
  "expectation_targets": ["ZQ"],
  "ops": []
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

TEST(CircuitSerializer, FromJson_ExpectationTargetWrongLength_Throws) {
    const std::string src = R"({
  "num_qubits": 3,
  "expectation_targets": ["ZZ"],
  "ops": []
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}

// Round-trip safety: validation must be no stricter than the builder, so any
// API-built circuit still survives to_json() -> from_json().
TEST(CircuitSerializer, RoundTrip_ValidationDoesNotRejectApiBuiltCircuit) {
    Circuit c(3, 0);
    c.add_creg("a", 2);
    c.add_creg("b", 1);
    c.h(0).cx(0, 1).ccx(0, 1, 2).barrier({0, 1, 2})
     .measure(0, 0).measure(1, 1).measure(2, 2)
     .add_expectation("ZZZ");
    GateOp if_body;
    if_body.type   = GateType::X;
    if_body.name   = "x";
    if_body.qubits = {0};
    c.if_then(c.creg("a"), 3, if_body);

    EXPECT_NO_THROW(Circuit::from_json(c.to_json()));
    const Circuit back = Circuit::from_json(c.to_json());
    EXPECT_EQ(back.num_ops(), c.num_ops());
    EXPECT_EQ(back.total_cubits(), c.total_cubits());
}

// ─────────────────────────────────────────────────────────────────────────────
// QASM v2 emission
// ─────────────────────────────────────────────────────────────────────────────

TEST(QasmEmitter, BellPair_ProducesExpectedHeaderAndBody) {
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    const std::string out = c.to_qasm();

    EXPECT_TRUE(out.find("OPENQASM 2.0;") != std::string::npos);
    EXPECT_TRUE(out.find("include \"qelib1.inc\";") != std::string::npos);
    EXPECT_TRUE(out.find("qreg q[2];") != std::string::npos);
    EXPECT_TRUE(out.find("creg c[2];") != std::string::npos);
    EXPECT_TRUE(out.find("h q[0];")    != std::string::npos);
    EXPECT_TRUE(out.find("cx q[0], q[1];") != std::string::npos);
    EXPECT_TRUE(out.find("measure q[0] -> c[0];") != std::string::npos);
    EXPECT_TRUE(out.find("measure q[1] -> c[1];") != std::string::npos);
}

TEST(QasmEmitter, RotationGates_EmitParamsWithFullPrecision) {
    Circuit c(1, 0);
    c.rx(0.7853981633974483, 0);   // pi/4
    const std::string out = c.to_qasm();
    // specifies setprecision(17) for round-trip safety.
    // setprecision(17) of pi/4 emits "0.78539816339744828" (one extra digit
    // vs the literal -- IEEE-754 double round-trip property). Assert the
    // "rx(" + "q[0]" markers + a sane digit count instead of a literal match.
    const auto rx_pos = out.find("rx(");
    EXPECT_TRUE(rx_pos != std::string::npos);
    const auto close = out.find(") q[0];", rx_pos);
    EXPECT_TRUE(close != std::string::npos);
    if (rx_pos != std::string::npos && close != std::string::npos) {
        const std::string token = out.substr(rx_pos + 3, close - rx_pos - 3);
        // setprecision(17) must produce at least 15 significant digits for
        // a number near pi/4.
        EXPECT_TRUE(token.size() >= static_cast<std::size_t>(15));
    }
}

TEST(QasmEmitter, Barrier_EmptyAndExplicit) {
    Circuit a(2, 0);
    a.barrier();
    Circuit b(2, 0);
    b.barrier({0, 1});

    const std::string oa = a.to_qasm();
    const std::string ob = b.to_qasm();
    EXPECT_TRUE(oa.find("barrier;") != std::string::npos);
    EXPECT_TRUE(ob.find("barrier q[0], q[1];") != std::string::npos);
}

TEST(QasmEmitter, IfOp_EmitsConditional) {
    Circuit c(2, 1);
    GateOp body;
    body.type = GateType::X;
    body.name = "x";
    body.qubits.push_back(1);
    c.if_then(0, 1, body);

    const std::string out = c.to_qasm();
    EXPECT_TRUE(out.find("if (c==1) x q[1];") != std::string::npos);
}

TEST(QasmEmitter, Reset_EmitsResetToken) {
    Circuit c(1, 0);
    c.reset(0);
    const std::string out = c.to_qasm();
    EXPECT_TRUE(out.find("reset q[0];") != std::string::npos);
}

TEST(QasmEmitter, IdleOp_OmittedFromOutput) {
    // IdleOp materialised via JSON; QASM emission must skip it.
    const std::string src = R"({
  "version": 1,
  "num_qubits": 1,
  "classical_registers": [],
  "opaque_definitions": [],
  "metadata": {"name": ""},
  "ops": [{"type": "idle", "qubit": 0, "duration_ns": 5.0}]
})";
    Circuit c = Circuit::from_json(src);
    const std::string out = c.to_qasm();
    EXPECT_TRUE(out.find("idle") == std::string::npos);
    EXPECT_TRUE(out.find("duration") == std::string::npos);
}

TEST(QasmEmitter, KrausOpaque_Throws) {
    // QASM v2 cannot express non-unitary channels.
    Circuit c(1, 0);
    KrausChannel ch;
    UnitaryMatrix k(2, std::vector<std::complex<double>>(2));
    const double s = 1.0 / std::sqrt(2.0);
    k[0][0] = {s, 0.0}; k[1][1] = {s, 0.0};
    ch.operators.push_back(k);
    ch.operators.push_back(k);
    c.define_opaque("noise_id", ch);

    EXPECT_THROW(static_cast<void>(c.to_qasm()), CircuitError);
}

TEST(QasmEmitter, OpaqueDeclaration_EmittedAsOpaqueLine) {
    Circuit c(2, 0);
    c.declare_opaque("u_custom", 2);
    const std::string out = c.to_qasm();
    EXPECT_TRUE(out.find("opaque u_custom") != std::string::npos);
    EXPECT_TRUE(out.find("q0, q1;")         != std::string::npos);
}

TEST(QasmEmitter, MultiRegister_EmitsBothCregLines) {
    Circuit c(1, 2);            // default "c" width 2
    c.add_creg("aux", 3);
    const std::string out = c.to_qasm();
    EXPECT_TRUE(out.find("creg c[2];")   != std::string::npos);
    EXPECT_TRUE(out.find("creg aux[3];") != std::string::npos);
}

// The payload bridge bypasses Circuit::if_then(), so the dead-branch rule has to
// be repeated in the JSON validator (audit #23).
TEST(CircuitSerializer, FromJson_IfValueWiderThanRegister_Throws) {
    const std::string src = R"({
  "num_qubits": 1,
  "classical_registers": [{"name": "c", "width": 2, "offset": 0}],
  "ops": [{"type": "if", "creg": 0, "value": 9,
           "body": {"type": "gate", "name": "x", "gate_type": 1, "qubits": [0], "params": []}}]
})";
    EXPECT_THROW(Circuit::from_json(src), CircuitError);
}
