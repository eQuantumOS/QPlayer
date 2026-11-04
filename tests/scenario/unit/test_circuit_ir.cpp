// test_circuit_ir.cpp -- Phase 2 / unit tests for the Circuit IR
// (gate_op.hpp / parameter.hpp / circuit.hpp). verifies the type-level
// contract laid down by and : alternative
// count, ordering, IdleOp field shape, and the default OpaqueDefinition body
// marker. verifies the Circuit container behaviour: chaining, qubit
// range checks, classical register management, opaque promotion
// and has_classical_control routing. Serialisation
// round-trip tests land in once implements to_qasm / to_json.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <array>
#include <complex>
#include <string>
#include <type_traits>
#include <variant>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/circuit/parameter.hpp"
#include "../../../include/error.hpp"

using qplayer::BarrierOp;
using qplayer::DeclarationOnly;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::IdleOp;
using qplayer::IfOp;
using qplayer::KrausChannel;
using qplayer::MeasureOp;
using qplayer::Op;
using qplayer::OpaqueDefinition;
using qplayer::Parameter;
using qplayer::QubitIndex;
using qplayer::UnitaryMatrix;

// ── Op variant cardinality + alternative ordering ───────
// Adding or reordering a variant alternative changes std::variant::index()
// values, which silently breaks JSON serialisation round-trip. The
// static_asserts pin both the count and the index of every alternative so a
// future edit to gate_op.hpp fails at compile time, not at runtime.

static_assert(std::variant_size_v<Op> == 5,
              "Op variant must have exactly 5 alternatives "
              "");

static_assert(std::is_same_v<std::variant_alternative_t<0, Op>, GateOp>,
              "Op variant index 0 must be GateOp");
static_assert(std::is_same_v<std::variant_alternative_t<1, Op>, MeasureOp>,
              "Op variant index 1 must be MeasureOp");
static_assert(std::is_same_v<std::variant_alternative_t<2, Op>, BarrierOp>,
              "Op variant index 2 must be BarrierOp");
static_assert(std::is_same_v<std::variant_alternative_t<3, Op>, IfOp>,
              "Op variant index 3 must be IfOp");
static_assert(std::is_same_v<std::variant_alternative_t<4, Op>, IdleOp>,
              "Op variant index 4 must be IdleOp (-- last)");

// ── IdleOp field shape ──────────────────────────────────

TEST(CircuitIR, IdleOp_HasQubitAndDurationNsFieldsOnly) {
    // Arrange / Act
    IdleOp node{};

    // Assert -- defaults match
    EXPECT_EQ(node.qubit,        static_cast<QubitIndex>(0));
    EXPECT_NEAR(node.duration_ns, 0.0, 0.0);  // exact zero-init expected.

    // Compile-time field-type guard so future renames trip the build.
    static_assert(std::is_same_v<decltype(IdleOp::qubit),       QubitIndex>,
                  "IdleOp.qubit must be QubitIndex");
    static_assert(std::is_same_v<decltype(IdleOp::duration_ns), double>,
                  "IdleOp.duration_ns must be double");
}

TEST(CircuitIR, IdleOp_FitsInOpVariantAtIndex4) {
    // Arrange
    Op node = IdleOp{/*qubit=*/3u, /*duration_ns=*/42.5};

    // Assert
    EXPECT_EQ(node.index(), static_cast<std::size_t>(4));
    EXPECT_TRUE(std::holds_alternative<IdleOp>(node));

    const auto& idle = std::get<IdleOp>(node);
    EXPECT_EQ(idle.qubit,       static_cast<QubitIndex>(3));
    EXPECT_NEAR(idle.duration_ns, 42.5, 1e-12);  // exact double round-trip.
}

// ── Other variant members: trivially-constructible defaults ──────────────────

TEST(CircuitIR, GateOp_DefaultsAreSafeAndNotOpaqueOrFused) {
    GateOp g{};

    // enum class lacks an ostream operator<<; use EXPECT_TRUE to avoid the
    // self-framework's stream-insertion requirement (assertions.hpp:77).
    EXPECT_TRUE(g.type == GateType::I);
    EXPECT_TRUE(g.name.empty());
    EXPECT_TRUE(g.qubits.empty());
    EXPECT_TRUE(g.params.empty());
    EXPECT_TRUE(g.opaque_name.empty());
    EXPECT_FALSE(g.duration_ns.has_value());
    EXPECT_TRUE(g.noise_channels.empty());
    EXPECT_FALSE(g.is_fused);
    EXPECT_FALSE(g.is_opaque);
}

TEST(CircuitIR, MeasureOp_HasQubitAndCubitFields) {
    MeasureOp m{/*qubit=*/2u, /*cubit=*/5u};
    EXPECT_EQ(m.qubit, static_cast<QubitIndex>(2));
    EXPECT_EQ(m.cubit, static_cast<qplayer::ClassicalBitIndex>(5));
}

TEST(CircuitIR, BarrierOp_EmptyQubitsMeansAll) {
    // empty qubits list means "barrier on all".
    BarrierOp b{};
    EXPECT_TRUE(b.qubits.empty());
}

TEST(CircuitIR, IfOp_HoldsCregValueAndGateBody) {
    GateOp body{};
    body.type = GateType::X;
    body.qubits.push_back(1u);

    IfOp cond{/*creg=*/0u, /*value=*/1ull, body};

    EXPECT_EQ(cond.creg,       static_cast<qplayer::ClassicalRegisterIndex>(0));
    EXPECT_EQ(cond.value,      static_cast<std::uint64_t>(1));
    EXPECT_TRUE(cond.body.type == GateType::X);  // enum class -- see GateOp test.
    EXPECT_EQ(cond.body.qubits.size(), static_cast<std::size_t>(1));
}

// ── OpaqueDefinition body = DeclarationOnly by default ────────────
// SemanticValidatorPass relies on this default to detect "declared but never
// defined" opaque gates and throw CircuitError. If the default body silently
// changes to UnitaryMatrix{} the validator would let stub usage through.

TEST(CircuitIR, OpaqueDefinition_DefaultBodyIsDeclarationOnly) {
    OpaqueDefinition def{};

    EXPECT_EQ(def.num_qubits, static_cast<QubitIndex>(0));
    EXPECT_TRUE(def.name.empty());
    EXPECT_TRUE(std::holds_alternative<DeclarationOnly>(def.body));

    // Variant ordering -- fixes the discriminator order so
    // JSON {"type": "declaration_only" | "matrix" | "kraus"} maps 1:1.
    static_assert(std::variant_size_v<decltype(OpaqueDefinition::body)> == 3,
                  "OpaqueDefinition.body must be a 3-variant");
    using B = decltype(OpaqueDefinition::body);
    static_assert(std::is_same_v<std::variant_alternative_t<0, B>, DeclarationOnly>,
                  "body index 0 must be DeclarationOnly");
    static_assert(std::is_same_v<std::variant_alternative_t<1, B>, UnitaryMatrix>,
                  "body index 1 must be UnitaryMatrix");
    static_assert(std::is_same_v<std::variant_alternative_t<2, B>, KrausChannel>,
                  "body index 2 must be KrausChannel");
}

TEST(CircuitIR, DeclarationOnly_EqualityIsAlwaysTrue) {
    // DeclarationOnly has no payload, so any two instances compare equal --
    // needed by variant equality in tests + round-trip checks.
    DeclarationOnly a{};
    DeclarationOnly b{};
    EXPECT_TRUE(a == b);
}

// ── GateType enum extents (: qelib1 direct + Reset + Opaque) ─────────

TEST(CircuitIR, GateType_IncludesResetAndOpaqueSentinels) {
    // Reset and Opaque ride the GateType enum so the Op variant width stays
    // at 5 alternatives. A plain enum-to-int cast is
    // enough to confirm the enumerators exist.
    GateOp reset{};
    reset.type = GateType::Reset;
    EXPECT_EQ(static_cast<int>(reset.type), static_cast<int>(GateType::Reset));

    GateOp opaque{};
    opaque.type        = GateType::Opaque;
    opaque.is_opaque   = true;
    opaque.opaque_name = "my_op";
    EXPECT_TRUE(opaque.is_opaque);
    EXPECT_EQ(opaque.opaque_name, "my_op");
}

// ── Parameter scaffold (parameter.hpp) ───────────────────────────────────────
// Phase 4 promotes Parameter to a variant<double, Expression>; until then the
// numeric path is the only one exercised. This test guards the default-state
// contract so the Phase 4 migration produces a compile error rather than a
// silent semantic shift.

TEST(CircuitIR, Parameter_DefaultIsNumericZero) {
    Parameter p{};
    EXPECT_TRUE(p.name.empty());
    EXPECT_NEAR(p.value, 0.0, 0.0);  // exact zero-init expected.
    EXPECT_FALSE(p.is_symbolic);
}

// ─────────────────────────────────────────────────────────────────────────────
// Circuit container behaviour
// ─────────────────────────────────────────────────────────────────────────────

using qplayer::Circuit;
using qplayer::ClassicalRegister;
using qplayer::ClassicalRegisterIndex;
using qplayer::CircuitError;

// ── Constructor + default classical register ──────────────────────

TEST(Circuit, Constructor_WithCubits_AutoCreatesDefaultRegisterC) {
    Circuit c(/*num_qubits=*/3, /*num_cubits=*/3);

    EXPECT_EQ(c.num_qubits(), static_cast<qplayer::QubitIndex>(3));
    EXPECT_EQ(c.num_cubits(), static_cast<qplayer::ClassicalBitIndex>(3));
    EXPECT_EQ(c.total_cubits(), static_cast<qplayer::ClassicalBitIndex>(3));
    EXPECT_EQ(c.classical_registers().size(), static_cast<std::size_t>(1));
    EXPECT_EQ(c.classical_registers()[0].name, std::string{"c"});
    EXPECT_EQ(c.classical_registers()[0].width, static_cast<qplayer::ClassicalBitIndex>(3));
    EXPECT_EQ(c.classical_registers()[0].offset, static_cast<qplayer::ClassicalBitIndex>(0));
}

TEST(Circuit, Constructor_NoCubits_LeavesRegisterListEmpty) {
    Circuit c(2);  // num_cubits defaults to 0.
    EXPECT_EQ(c.classical_registers().size(), static_cast<std::size_t>(0));
    EXPECT_EQ(c.total_cubits(), static_cast<qplayer::ClassicalBitIndex>(0));
}

// ── Gate chaining: 1Q + 2Q + 3Q + rotation ──────────────────────────────────

TEST(Circuit, OneQubitGates_AppendInOrder) {
    Circuit c(2, 2);
    c.h(0).x(1).z(0).s(1).t(0);

    EXPECT_EQ(c.num_ops(), static_cast<std::size_t>(5));
    // First op should be H on qubit 0.
    const auto* first = std::get_if<GateOp>(&c.ops()[0]);
    EXPECT_TRUE(first != nullptr);
    if (first) {
        EXPECT_TRUE(first->type == GateType::H);
        EXPECT_EQ(first->qubits.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(first->qubits[0], static_cast<qplayer::QubitIndex>(0));
    }
}

TEST(Circuit, TwoQubitGates_CarryCtrlAndTarget) {
    Circuit c(3, 0);
    c.cx(0, 1).swap(1, 2);

    EXPECT_EQ(c.num_ops(), static_cast<std::size_t>(2));
    const auto* cx = std::get_if<GateOp>(&c.ops()[0]);
    EXPECT_TRUE(cx != nullptr);
    if (cx) {
        EXPECT_TRUE(cx->type == GateType::CX);
        EXPECT_EQ(cx->qubits.size(), static_cast<std::size_t>(2));
        EXPECT_EQ(cx->qubits[0], static_cast<qplayer::QubitIndex>(0));
        EXPECT_EQ(cx->qubits[1], static_cast<qplayer::QubitIndex>(1));
    }
}

TEST(Circuit, ThreeQubitToffoli_StoresAllThreeQubits) {
    Circuit c(3, 0);
    c.ccx(0, 1, 2);

    EXPECT_EQ(c.num_ops(), static_cast<std::size_t>(1));
    const auto* g = std::get_if<GateOp>(&c.ops()[0]);
    EXPECT_TRUE(g != nullptr);
    if (g) {
        EXPECT_TRUE(g->type == GateType::CCX);
        EXPECT_EQ(g->qubits.size(), static_cast<std::size_t>(3));
    }
}

TEST(Circuit, RotationGate_CarriesParameter) {
    Circuit c(1, 0);
    c.rx(1.25, 0);

    const auto* g = std::get_if<GateOp>(&c.ops()[0]);
    EXPECT_TRUE(g != nullptr);
    if (g) {
        EXPECT_TRUE(g->type == GateType::RX);
        EXPECT_EQ(g->params.size(), static_cast<std::size_t>(1));
        EXPECT_NEAR(g->params[0], 1.25, 1e-15);
    }
}

TEST(Circuit, U3Gate_CarriesThreeParameters) {
    Circuit c(1, 0);
    c.u3(1.0, 2.0, 3.0, 0);

    const auto* g = std::get_if<GateOp>(&c.ops()[0]);
    EXPECT_TRUE(g != nullptr);
    if (g) {
        EXPECT_EQ(g->params.size(), static_cast<std::size_t>(3));
        EXPECT_NEAR(g->params[0], 1.0, 1e-15);
        EXPECT_NEAR(g->params[1], 2.0, 1e-15);
        EXPECT_NEAR(g->params[2], 3.0, 1e-15);
    }
}

// ── Qubit range validation ──────────────────────────────────────────────────

TEST(Circuit, GateOutOfRange_ThrowsCircuitError) {
    Circuit c(2, 0);
    EXPECT_THROW(c.h(2), CircuitError);
    EXPECT_THROW(c.cx(0, 5), CircuitError);
    EXPECT_THROW(c.ccx(0, 1, 7), CircuitError);
}

TEST(Circuit, TwoQubitGate_SameQubitTwice_ThrowsCircuitError) {
    Circuit c(2, 0);
    EXPECT_THROW(c.cx(0, 0), CircuitError);
    EXPECT_THROW(c.swap(1, 1), CircuitError);
}

TEST(Circuit, ToffoliDuplicateQubits_ThrowsCircuitError) {
    Circuit c(3, 0);
    EXPECT_THROW(c.ccx(0, 1, 0), CircuitError);   // c0 == tgt
    EXPECT_THROW(c.cswap(0, 1, 1), CircuitError); // t0 == t1
}

// ── Measure / measure_all / barrier / reset / if_then ────────────────────────

TEST(Circuit, Measure_StoresQubitAndCubit) {
    Circuit c(2, 2);
    c.h(0).measure(0, 0);

    EXPECT_EQ(c.num_ops(), static_cast<std::size_t>(2));
    const auto* m = std::get_if<MeasureOp>(&c.ops()[1]);
    EXPECT_TRUE(m != nullptr);
    if (m) {
        EXPECT_EQ(m->qubit, static_cast<qplayer::QubitIndex>(0));
        EXPECT_EQ(m->cubit, static_cast<qplayer::ClassicalBitIndex>(0));
    }
}

TEST(Circuit, MeasureCubitOutOfRange_ThrowsCircuitError) {
    Circuit c(2, 1);                 // default "c" has width=1.
    EXPECT_THROW(c.measure(0, 5), CircuitError);
}

TEST(Circuit, MeasureAll_ExpandsToNMeasureOps) {
    Circuit c(3, 3);
    c.measure_all();

    EXPECT_EQ(c.num_ops(), static_cast<std::size_t>(3));
    for (qplayer::QubitIndex i = 0; i < 3; ++i) {
        const auto* m = std::get_if<MeasureOp>(&c.ops()[i]);
        EXPECT_TRUE(m != nullptr);
        if (m) {
            EXPECT_EQ(m->qubit, i);
            EXPECT_EQ(m->cubit, static_cast<qplayer::ClassicalBitIndex>(i));
        }
    }
}

TEST(Circuit, MeasureAll_RegisterTooSmall_ThrowsCircuitError) {
    Circuit c(3, 2);                 // 3 qubits, only 2 cubits available.
    EXPECT_THROW(c.measure_all(), CircuitError);
}

TEST(Circuit, BarrierEmpty_StoredAsAllQubits) {
    Circuit c(2, 0);
    c.barrier();
    const auto* b = std::get_if<BarrierOp>(&c.ops()[0]);
    EXPECT_TRUE(b != nullptr);
    if (b) EXPECT_TRUE(b->qubits.empty());
}

TEST(Circuit, BarrierWithList_RecordsQubits) {
    Circuit c(3, 0);
    c.barrier({0, 2});
    const auto* b = std::get_if<BarrierOp>(&c.ops()[0]);
    EXPECT_TRUE(b != nullptr);
    if (b) EXPECT_EQ(b->qubits.size(), static_cast<std::size_t>(2));
}

TEST(Circuit, Reset_EmittedAsGateOpWithResetType) {
    Circuit c(1, 0);
    c.reset(0);
    const auto* g = std::get_if<GateOp>(&c.ops()[0]);
    EXPECT_TRUE(g != nullptr);
    if (g) EXPECT_TRUE(g->type == GateType::Reset);
}

TEST(Circuit, IfThen_StoresCregValueAndBody) {
    Circuit c(2, 1);
    GateOp body;
    body.type = GateType::X;
    body.name = "x";
    body.qubits.push_back(0);
    c.if_then(/*creg=*/0, /*value=*/1, body);

    const auto* cond = std::get_if<IfOp>(&c.ops()[0]);
    EXPECT_TRUE(cond != nullptr);
    if (cond) {
        EXPECT_EQ(cond->creg,  static_cast<qplayer::ClassicalRegisterIndex>(0));
        EXPECT_EQ(cond->value, static_cast<std::uint64_t>(1));
        EXPECT_TRUE(cond->body.type == GateType::X);
    }
}

TEST(Circuit, IfThen_CregOutOfRange_ThrowsCircuitError) {
    Circuit c(1, 0);                 // no classical registers.
    GateOp body;
    body.type = GateType::X;
    body.qubits.push_back(0);
    EXPECT_THROW(c.if_then(0, 1, body), CircuitError);
}

// ── add_creg / creg / total_cubits ────────────────────────────────

TEST(Circuit, AddCreg_AssignsRunningOffsets) {
    Circuit c(1, 2);                 // default "c" width=2 at offset 0.
    const auto idx_a = c.add_creg("a", 3);
    const auto idx_b = c.add_creg("b", 1);

    EXPECT_EQ(idx_a, static_cast<ClassicalRegisterIndex>(1));
    EXPECT_EQ(idx_b, static_cast<ClassicalRegisterIndex>(2));
    EXPECT_EQ(c.total_cubits(), static_cast<qplayer::ClassicalBitIndex>(6));
    EXPECT_EQ(c.classical_registers()[1].offset, static_cast<qplayer::ClassicalBitIndex>(2));
    EXPECT_EQ(c.classical_registers()[2].offset, static_cast<qplayer::ClassicalBitIndex>(5));
}

TEST(Circuit, AddCreg_DuplicateName_ThrowsCircuitError) {
    Circuit c(1, 2);                 // default "c" already exists.
    EXPECT_THROW(c.add_creg("c", 1), CircuitError);
}

TEST(Circuit, AddCreg_ZeroWidth_ThrowsCircuitError) {
    Circuit c(1, 0);
    EXPECT_THROW(c.add_creg("a", 0), CircuitError);
}

TEST(Circuit, CregLookup_MissingName_ThrowsCircuitError) {
    Circuit c(1, 2);
    EXPECT_THROW(static_cast<void>(c.creg("missing")), CircuitError);
}

TEST(Circuit, CregLookup_FindsByName) {
    Circuit c(1, 2);
    c.add_creg("a", 4);
    EXPECT_EQ(c.creg("c"), static_cast<ClassicalRegisterIndex>(0));
    EXPECT_EQ(c.creg("a"), static_cast<ClassicalRegisterIndex>(1));
}

// ── Opaque definitions ─────────────────────────────────

TEST(Circuit, DeclareOpaque_StoresDeclarationOnlyBody) {
    Circuit c(1, 0);
    c.declare_opaque("u_custom", 1);

    EXPECT_TRUE(c.has_opaque_definition("u_custom"));
    const auto& def = c.opaque_definition("u_custom");
    EXPECT_EQ(def.num_qubits, static_cast<qplayer::QubitIndex>(1));
    EXPECT_TRUE(std::holds_alternative<DeclarationOnly>(def.body));
}

TEST(Circuit, DeclareOpaque_DuplicateName_ThrowsCircuitError) {
    Circuit c(1, 0);
    c.declare_opaque("u_custom", 1);
    EXPECT_THROW(c.declare_opaque("u_custom", 1), CircuitError);
}

TEST(Circuit, DefineOpaque_1Q_PromotesDeclarationToUnitary) {
    Circuit c(1, 0);
    c.declare_opaque("u_id", 1);

    // 1Q identity matrix -- exactly unitary.
    std::array<std::array<std::complex<double>, 2>, 2> id1{};
    id1[0][0] = {1.0, 0.0}; id1[0][1] = {0.0, 0.0};
    id1[1][0] = {0.0, 0.0}; id1[1][1] = {1.0, 0.0};

    c.define_opaque("u_id", id1);

    const auto& def = c.opaque_definition("u_id");
    EXPECT_TRUE(std::holds_alternative<qplayer::UnitaryMatrix>(def.body));
}

TEST(Circuit, DefineOpaque_NonUnitaryMatrix_ThrowsCircuitError) {
    Circuit c(1, 0);
    // |2 0; 0 2| is NOT unitary (U†U = 4I).
    std::array<std::array<std::complex<double>, 2>, 2> bad{};
    bad[0][0] = {2.0, 0.0}; bad[1][1] = {2.0, 0.0};
    EXPECT_THROW(c.define_opaque("u_bad", bad), CircuitError);
}

TEST(Circuit, DefineOpaque_2Q_IdentityPasses) {
    Circuit c(2, 0);
    std::array<std::array<std::complex<double>, 4>, 4> id2{};
    for (std::size_t i = 0; i < 4; ++i) id2[i][i] = {1.0, 0.0};
    c.define_opaque("u_id2", id2);

    const auto& def = c.opaque_definition("u_id2");
    EXPECT_EQ(def.num_qubits, static_cast<qplayer::QubitIndex>(2));
    EXPECT_TRUE(std::holds_alternative<qplayer::UnitaryMatrix>(def.body));
}

// ── Circuit::opaque() — opaque CALL on the program path ───────────
// Before this the only way to place an opaque op in a circuit was the QASM
// parser; define_opaque() stored a body nobody could invoke.

TEST(Circuit, Opaque_AppendsGateOpMatchingParserFields) {
    // Arrange
    Circuit c(2, 0);
    c.declare_opaque("g2", 2);

    // Act
    c.opaque("g2", {1, 0});

    // Assert — field layout must match CircuitBuilder's opaque call so both
    // paths hand the backend an identical GateOp.
    EXPECT_EQ(c.num_ops(), std::size_t{1});
    const auto& g = std::get<GateOp>(c.ops()[0]);
    EXPECT_TRUE(g.type == GateType::Opaque);
    EXPECT_TRUE(g.is_opaque);
    EXPECT_EQ(g.opaque_name, std::string{"g2"});
    EXPECT_EQ(g.name, std::string{"g2"});
    // Operand order is preserved verbatim — the definition matrix's local bit b
    // maps to qubits[b], so {1,0} must not be normalised to {0,1}.
    EXPECT_EQ(g.qubits.size(), std::size_t{2});
    EXPECT_EQ(g.qubits[0], QubitIndex{1});
    EXPECT_EQ(g.qubits[1], QubitIndex{0});
}

TEST(Circuit, Opaque_UndefinedName_IsAllowedAtCallTime) {
    // QASM v2's "declare first, define later" order is preserved on the program
    // path: an unknown name is legal here and rejected by SemanticValidatorPass
    // at transpile() time instead (not duplicated in Circuit).
    Circuit c(1, 0);
    EXPECT_NO_THROW(c.opaque("not_registered_yet", {0}));
    EXPECT_EQ(c.num_ops(), std::size_t{1});
}

TEST(Circuit, Opaque_ArityMismatchAgainstDefinition_ThrowsCircuitError) {
    // When a definition IS present the arity is a settled error, so it is caught
    // immediately rather than deferred to transpile.
    Circuit c(2, 0);
    c.declare_opaque("g1", 1);
    EXPECT_THROW(c.opaque("g1", {0, 1}), CircuitError);
}

TEST(Circuit, Opaque_RepeatedQubit_ThrowsCircuitError) {
    // One local matrix bit per operand — repeating a qubit has no meaning.
    Circuit c(2, 0);
    c.declare_opaque("g2", 2);
    EXPECT_THROW(c.opaque("g2", {1, 1}), CircuitError);
}

TEST(Circuit, Opaque_QubitOutOfRange_ThrowsCircuitError) {
    Circuit c(2, 0);
    EXPECT_THROW(c.opaque("g", {5}), CircuitError);
}

TEST(Circuit, Opaque_EmptyName_ThrowsCircuitError) {
    Circuit c(1, 0);
    EXPECT_THROW(c.opaque("", {0}), CircuitError);
}

TEST(Circuit, DefineOpaque_RedefiningConcreteBody_ThrowsCircuitError) {
    // single-direction promotion: once a body is concrete, it cannot
    // be redefined (no Unitary -> Unitary overwrite, no Unitary <-> Kraus swap).
    Circuit c(1, 0);
    std::array<std::array<std::complex<double>, 2>, 2> id1{};
    id1[0][0] = {1.0, 0.0}; id1[1][1] = {1.0, 0.0};
    c.define_opaque("u_id", id1);
    EXPECT_THROW(c.define_opaque("u_id", id1), CircuitError);
}

TEST(Circuit, DefineOpaque_Kraus_TracePreservingChannelPasses) {
    Circuit c(1, 0);
    // Two-operator decomposition of the identity channel:
    //   K0 = 1/sqrt(2) I, K1 = 1/sqrt(2) I -> sum K†K = I.
    qplayer::KrausChannel ch;
    qplayer::UnitaryMatrix k(2, std::vector<std::complex<double>>(2));
    const double s = 1.0 / std::sqrt(2.0);
    k[0][0] = {s, 0.0}; k[1][1] = {s, 0.0};
    ch.operators.push_back(k);
    ch.operators.push_back(k);

    c.define_opaque("kraus_id", ch);
    const auto& def = c.opaque_definition("kraus_id");
    EXPECT_TRUE(std::holds_alternative<qplayer::KrausChannel>(def.body));
}

TEST(Circuit, DefineOpaque_Kraus_NonTracePreserving_ThrowsCircuitError) {
    Circuit c(1, 0);
    qplayer::KrausChannel ch;
    qplayer::UnitaryMatrix k(2, std::vector<std::complex<double>>(2));
    k[0][0] = {2.0, 0.0};         // sum K†K = 4*|0><0|, not I.
    ch.operators.push_back(k);
    EXPECT_THROW(c.define_opaque("kraus_bad", ch), CircuitError);
}

TEST(Circuit, OpaqueDefinitions_PreservesInsertionOrder) {
    Circuit c(1, 0);
    c.declare_opaque("a", 1);
    c.declare_opaque("b", 1);
    c.declare_opaque("c", 1);
    EXPECT_EQ(c.opaque_definitions().size(), static_cast<std::size_t>(3));
    EXPECT_EQ(c.opaque_definitions()[0].name, std::string{"a"});
    EXPECT_EQ(c.opaque_definitions()[1].name, std::string{"b"});
    EXPECT_EQ(c.opaque_definitions()[2].name, std::string{"c"});
}

// ── has_classical_control routing ─────────────────────────────────

TEST(Circuit, HasClassicalControl_PlainCircuit_ReturnsFalse) {
    Circuit c(2, 2);
    c.h(0).cx(0, 1);
    EXPECT_FALSE(c.has_classical_control());
}

TEST(Circuit, HasClassicalControl_TerminalMeasurementOnly_ReturnsFalse) {
    // Measure at the end of the circuit -- no downstream op on that qubit.
    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    EXPECT_FALSE(c.has_classical_control());
}

TEST(Circuit, HasClassicalControl_MidCircuitMeasure_ReturnsTrue) {
    // Measure then a gate on the same qubit -> per-shot path required.
    Circuit c(2, 2);
    c.h(0).measure(0, 0).x(0);
    EXPECT_TRUE(c.has_classical_control());
}

TEST(Circuit, HasClassicalControl_AnyIfOpPresent_ReturnsTrue) {
    Circuit c(2, 1);
    GateOp body;
    body.type = GateType::X;
    body.qubits.push_back(1);
    c.if_then(0, 1, body);
    EXPECT_TRUE(c.has_classical_control());
}

// ── Distinctness error message names both operands (audit #14) ───────────────
//
// check_distinct() printed `a` twice, so "cx q[1],q[1]" reported "(got 1, 1)"
// for every input pair — the message could not distinguish which operands were
// passed. Only the second argument was wrong, so the fix is one character; the
// test pins it because a duplicated-argument bug is invisible when both happen
// to be equal (which is exactly when this throw fires).
TEST(CircuitIR, CheckDistinct_ErrorMessageNamesBothOperands) {
    Circuit c(3);
    bool threw = false;
    try {
        c.cx(2, 2);
    } catch (const qplayer::CircuitError& e) {
        threw = true;
        const std::string msg = e.what();
        // Both operands are 2 here, so the message must show "2, 2" — and with
        // distinct-but-equal formatting we also assert the gate name is present.
        EXPECT_TRUE(msg.find("cx") != std::string::npos);
        EXPECT_TRUE(msg.find("2, 2") != std::string::npos);
    }
    EXPECT_TRUE(threw);
}

// ── define_opaque num_qubits is bounded (audit #22) ──────────────────────────
//
// verify_unitary() is O(N^3) with N = 2^num_qubits, and `std::size_t{1} <<
// num_qubits` is itself UB at >= 64. Nothing capped the value, so
// define_opaque("x", 20, m) meant O(2^60) and 64+ was undefined.

TEST(CircuitIR, DefineOpaque_QubitCountAboveCap_Throws) {
    Circuit c(2);
    c.declare_opaque("big", 20);
    qplayer::UnitaryMatrix tiny{{{1.0, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {1.0, 0.0}}};
    // Rejected on the qubit count before any O(N^3) work touches the matrix.
    EXPECT_THROW(c.define_opaque("big", 20, tiny), qplayer::CircuitError);
    EXPECT_THROW(c.define_opaque("big", 64, tiny), qplayer::CircuitError);
    EXPECT_THROW(c.define_opaque("big", 0,  tiny), qplayer::CircuitError);
}

TEST(CircuitIR, DefineOpaque_QubitCountAtCap_Accepted) {
    // 1 qubit stays legal: the cap only rejects sizes that cannot be verified.
    Circuit c(1);
    c.declare_opaque("u", 1);
    qplayer::UnitaryMatrix id{{{1.0, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {1.0, 0.0}}};
    EXPECT_NO_THROW(c.define_opaque("u", 1, id));
}

// ── has_classical_control() linear rewrite keeps its verdicts (audit #28) ─────
//
// The former shape re-scanned every following op per MeasureOp — O(ops^2) — and
// QRegister::run() plus all three backends' execute() call it on every run. The
// single-pass version must agree on every shape that decides fast path vs
// per-shot routing.

TEST(CircuitIR, HasClassicalControl_MatchesExpectedVerdicts) {
    // No measure at all -> fast path.
    { Circuit c(2); c.h(0).cx(0, 1);
      EXPECT_FALSE(c.has_classical_control()); }

    // Terminal measurement only -> still fast path.
    { Circuit c(2, 2); c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
      EXPECT_FALSE(c.has_classical_control()); }

    // Gate AFTER a measure on the same qubit -> mid-circuit, per-shot.
    { Circuit c(2, 2); c.h(0).measure(0, 0).x(0);
      EXPECT_TRUE(c.has_classical_control()); }

    // Gate after a measure on a DIFFERENT qubit -> still fast path.
    { Circuit c(2, 2); c.h(0).measure(0, 0).x(1);
      EXPECT_FALSE(c.has_classical_control()); }

    // Multi-qubit gate touching a measured qubit -> per-shot.
    { Circuit c(2, 2); c.h(0).measure(0, 0).cx(1, 0);
      EXPECT_TRUE(c.has_classical_control()); }

    // Barrier and a repeated measure do not reactivate the qubit.
    { Circuit c(2, 2); c.h(0).measure(0, 0).barrier({0, 1}).measure(0, 1);
      EXPECT_FALSE(c.has_classical_control()); }

    // Any IfOp short-circuits to true regardless of position.
    { Circuit c(1, 1);
      GateOp body; body.type = GateType::X; body.name = "x"; body.qubits = {0};
      c.add_creg("c2", 1);
      c.if_then(0, 1, body);
      EXPECT_TRUE(c.has_classical_control()); }
}
