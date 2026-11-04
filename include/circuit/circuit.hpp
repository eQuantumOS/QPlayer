/// @file circuit.hpp
/// @brief Public Circuit IR builder facade.
/// @ingroup api
///
/// Method chaining returns Circuit& so users can write circ.h(0).cx(0,1)....
/// Validation (qubit range, dimension match for opaque definitions) happens
/// inside method bodies — out-of-range / duplicate-name failures throw
/// CircuitError immediately.

#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "types.hpp"
#include "circuit/gate_op.hpp"
#include "circuit/metadata.hpp"
#include "circuit/parameter.hpp"
#include "noise/noise_model.hpp"   // KrausChannel + UnitaryMatrix

namespace qplayer {

/// One creg as declared in OpenQASM v2 (`creg <name>[width];`). offset is the
/// running cubit index assigned by Circuit when the register is added; it lets
/// MeasureOp.cubit address a global cubit space across multiple registers
///
struct ClassicalRegister {
    /// Register name as declared in QASM (`creg <name>[width];`).
    std::string       name;
    /// Number of classical bits in this register.
    ClassicalBitIndex width  = 0;
    /// Index of this register's bit 0 within the global cubit space.
    ClassicalBitIndex offset = 0;
};

/// Text drawing style for Circuit::draw.
///   Unicode: box-drawing glyphs (─ │ ┤ ├ ┼ ●), best readability, assumes a
///            UTF-8 terminal (default).
///   Ascii:   plain ASCII (- | + *), renders in any terminal / log file.
enum class CircuitDrawStyle { Unicode, Ascii };

/// Public Circuit IR builder. Gate methods return `Circuit&` so calls chain:
/// `circ.h(0).cx(0, 1).measure(0, 0)`.
class Circuit {
public:
    /// Constructs an empty circuit of the given width.
    ///
    /// @param num_qubits Number of qubits the circuit spans.
    /// @param num_cubits Classical bits to pre-allocate. A value > 0 auto-creates
    /// a default register named "c" at index 0; 0
    ///                   leaves the circuit with no classical register.
    explicit Circuit(QubitIndex num_qubits, ClassicalBitIndex num_cubits = 0);

    /// @name Single-qubit gates (qelib1.inc id, x, y, z, h, s, sdg, t, tdg, sx, sxdg)
    ///
    /// Every builder in this group shares one contract: it appends one GateOp to
    /// the end of the op stream, takes the target qubit `q` as an index that is
    /// validated against num_qubits() (out of range throws CircuitError before the
    /// circuit is mutated), and returns `*this` so calls chain as
    /// `circ.h(0).x(1)`. Angles, where present, are in radians.
    /// @{

    /// Identity — a placeholder op that carries a duration for idle noise.
    Circuit& id (QubitIndex q);
    /// Hadamard.
    Circuit& h  (QubitIndex q);
    /// Pauli X.
    Circuit& x  (QubitIndex q);
    /// Pauli Y.
    Circuit& y  (QubitIndex q);
    /// Pauli Z.
    Circuit& z  (QubitIndex q);
    /// Phase gate S = diag(1, i).
    Circuit& s  (QubitIndex q);
    /// Inverse phase gate S^dagger.
    Circuit& sdg(QubitIndex q);
    /// T gate = diag(1, exp(i*pi/4)).
    Circuit& t  (QubitIndex q);
    /// Inverse T gate.
    Circuit& tdg(QubitIndex q);
    /// Square root of X.
    Circuit& sx  (QubitIndex q);
    /// Inverse square root of X.
    Circuit& sxdg(QubitIndex q);
    /// @}

    /// @name Single-qubit rotations and U gates (qelib1.inc rx, ry, rz, u1, u2, u3)
    ///
    /// Same contract as the single-qubit gate group: one appended GateOp, operand
    /// validated against num_qubits(), `*this` returned for chaining. Angles are in
    /// radians and are not normalised. The built-in U(theta, phi, lambda) aliases u3
    /// at parse time.
    /// @{

    /// Rotation about X by @p theta.
    Circuit& rx(double theta,             QubitIndex q);
    /// Rotation about Y by @p theta.
    Circuit& ry(double theta,             QubitIndex q);
    /// Rotation about Z by @p theta.
    Circuit& rz(double theta,             QubitIndex q);
    /// Single-parameter phase gate u1(lambda) = diag(1, exp(i*lambda)).
    Circuit& u1(double lam,               QubitIndex q);
    /// Two-parameter gate u2(phi, lambda) = u3(pi/2, phi, lambda).
    Circuit& u2(double phi, double lam,   QubitIndex q);
    /// General single-qubit gate u3(theta, phi, lambda).
    Circuit& u3(double theta, double phi, double lam, QubitIndex q);
    /// @}

    /// @name Two-qubit gates
    ///
    /// Same contract as the single-qubit groups, with both operands validated
    /// against num_qubits(). Operand order is significant and is never normalised:
    /// for controlled gates the first operand is the control, and even for the
    /// symmetric-matrix gates a swapped pair is a different op on the circuit graph
    /// (so cx(0,1) and cx(1,0) never cancel each other).
    /// @{

    /// Controlled X (CNOT). The built-in QASM `CX a,b` aliases this at parse time.
    Circuit& cx  (QubitIndex ctrl, QubitIndex tgt);
    /// Controlled Y.
    Circuit& cy  (QubitIndex ctrl, QubitIndex tgt);
    /// Controlled Z.
    Circuit& cz  (QubitIndex ctrl, QubitIndex tgt);
    /// Controlled Hadamard.
    Circuit& ch  (QubitIndex ctrl, QubitIndex tgt);
    /// Exchanges the states of two qubits.
    Circuit& swap(QubitIndex q0,   QubitIndex q1);
    /// Controlled Z-rotation by @p theta.
    Circuit& crz (double theta,             QubitIndex ctrl, QubitIndex tgt);
    /// Controlled phase gate cu1(lambda).
    Circuit& cu1 (double lam,               QubitIndex ctrl, QubitIndex tgt);
    /// Controlled general single-qubit gate cu3(theta, phi, lambda).
    Circuit& cu3 (double theta, double phi, double lam,
                  QubitIndex ctrl, QubitIndex tgt);
    /// Echoed Cross-Resonance — IBM Eagle's native 2Q basis gate.
    ///
    /// A genuine 4x4 unitary and self-inverse (Hermitian). Reachable only through
    /// this builder or basis translation; OpenQASM v2 cannot express it.
    Circuit& ecr (QubitIndex q0, QubitIndex q1);
    /// iSWAP = [[1,0,0,0],[0,0,i,0],[0,i,0,0],[0,0,0,1]].
    ///
    /// One of the Google native 2Q basis gates: genuine 4x4 unitaries, none
    /// self-inverse, absent from qelib1.inc and therefore never built from QASM.
    Circuit& iswap     (QubitIndex q0, QubitIndex q1);
    /// Square root of iSWAP, so sqrt_iswap applied twice equals iswap.
    Circuit& sqrt_iswap(QubitIndex q0, QubitIndex q1);
    /// fSim(theta, phi) — Google's tunable two-qubit gate.
    Circuit& fsim      (double theta, double phi, QubitIndex q0, QubitIndex q1);
    /// The Sycamore gate, equal to fsim(pi/2, pi/6).
    Circuit& sycamore  (QubitIndex q0, QubitIndex q1);
    /// @}

    /// @name Three-qubit gates (qelib1.inc ccx, cswap)
    ///
    /// Same contract as the two-qubit group: all three operands validated against
    /// num_qubits(), order significant, `*this` returned for chaining.
    /// @{

    /// Toffoli — X on @p tgt when both controls are set.
    Circuit& ccx  (QubitIndex c0, QubitIndex c1, QubitIndex tgt);
    /// Fredkin — swaps @p t0 and @p t1 when @p ctrl is set.
    Circuit& cswap(QubitIndex ctrl, QubitIndex t0, QubitIndex t1);
    /// @}

    /// Measures one qubit into one classical bit.
    /// @param q     Qubit to measure.
    /// @param cubit Destination bit in the global cubit space.
    /// @return *this, so calls chain.
    /// @throws CircuitError @p q or @p cubit is out of range.
    Circuit& measure   (QubitIndex q, ClassicalBitIndex cubit);

    /// Measures every qubit into the like-numbered classical bit.
    /// @return *this, so calls chain.
    /// @throws CircuitError The classical space is narrower than num_qubits().
    Circuit& measure_all();

    /// Appends a barrier, which optimisation passes and gate fusion never cross.
    /// @param qubits Qubits to bar; an empty list bars every qubit.
    /// @return *this, so calls chain.
    /// @throws CircuitError A listed qubit is out of range.
    Circuit& barrier   (std::initializer_list<QubitIndex> qubits = {});

    /// Resets one qubit to |0>.
    /// @param q Qubit to reset.
    /// @return *this, so calls chain.
    /// @throws CircuitError @p q is out of range.
    Circuit& reset     (QubitIndex q);

    /// Appends a single gate conditioned on a classical register value.
    /// @param creg  Index into classical_registers() holding the condition.
    /// @param value Value the register must equal for @p gate to execute.
    /// @param gate  Gate to run when the condition holds.
    /// @return *this, so calls chain.
    /// @throws CircuitError @p creg is out of range or @p gate has an invalid operand.
    Circuit& if_then   (ClassicalRegisterIndex creg,
                        std::uint64_t          value,
                        const GateOp&          gate);

    /// Declares a new classical register.
    /// @param name  Register name, unique within the circuit.
    /// @param width Number of bits; must be non-zero.
    /// @return Index of the created register, usable with if_then().
    /// @throws CircuitError The name is already taken or @p width is 0.
    ClassicalRegisterIndex add_creg(const std::string& name,
                                    ClassicalBitIndex  width);

    /// Looks up a classical register by name.
    /// @param name Register name.
    /// @return Index of the register.
    /// @throws CircuitError No register carries that name.
    ClassicalRegisterIndex creg    (const std::string& name) const;

    /// All declared classical registers, in declaration order.
    /// @return Reference to the register list.
    const std::vector<ClassicalRegister>& classical_registers() const noexcept;

    /// Sum of all register widths — synonym for num_cubits().
    /// @return Total classical bits addressable by measure().
    ClassicalBitIndex total_cubits() const noexcept;

    /// ── Expectation-value targets ──────────────
    /// Pre-register a Pauli string whose expectation run() accumulates into
    /// Result.expectation (model A). Qiskit qubit order: pauli[0] is the
    /// highest qubit. Validation: I/X/Y/Z only, length == num_qubits, empty
    /// string -> CircuitError. Duplicate registration is idempotent. Chainable.
    /// Persisted by to_json/from_json so the target set survives round-trips
    /// The one-shot post-hoc QRegister::expectation_value(pauli)
    /// is a distinct quantity (single value vs nshot average).
    /// @param pauli Pauli string of length num_qubits(), big-endian (pauli[0] is
    ///              the highest qubit), containing only I/X/Y/Z.
    /// @return *this, so calls chain.
    /// @throws CircuitError The string is empty, mis-sized, or holds a non-Pauli
    ///                      character.
    Circuit& add_expectation(std::string_view pauli);

    /// Pauli strings registered for accumulation during run().
    /// @return Reference to the target list, in registration order.
    const std::vector<std::string>& expectation_targets() const noexcept;

    /// ── Opaque definitions (+) ──────────────────
    /// declare_opaque: register stub (body = DeclarationOnly). Used by the
    ///   QASM parser when it encounters `opaque <name> <qubits>;`. Calling
    ///   define_opaque() afterwards promotes the body in-place exactly once
    ///   (DeclarationOnly -> UnitaryMatrix or KrausChannel).
    /// define_opaque overload set:
    ///   - 1Q 2x2 std::array       (literal matrix syntax)
    ///   - 2Q 4x4 std::array       (literal matrix syntax)
    ///   - NQ vector<vector>       (general matrix, num_qubits >= 3 typical)
    ///   - KrausChannel            (non-unitary channel)
    /// Body promotion rules:
    ///   DeclarationOnly -> UnitaryMatrix / KrausChannel : OK
    ///   UnitaryMatrix   -> KrausChannel                  : CircuitError (single-direction promotion only)
    ///   KrausChannel    -> UnitaryMatrix                 : CircuitError
    ///   any concrete    -> DeclarationOnly               : CircuitError (no demotion)
    /// Registers an opaque gate name with no body yet.
    /// @param name       Opaque gate name.
    /// @param num_qubits Arity the later definition must match.
    /// @return *this, so calls chain.
    /// @throws CircuitError The name already carries a concrete body.
    Circuit& declare_opaque(const std::string& name, QubitIndex num_qubits);

    /// @name Opaque body definition
    ///
    /// All four overloads share one contract: they attach a concrete body to
    /// @p name, promoting a DeclarationOnly stub exactly once, and return `*this`.
    /// Promotion is single-direction — DeclarationOnly may become UnitaryMatrix or
    /// KrausChannel, but a concrete body can neither switch kind nor revert to a
    /// stub, and any attempt throws CircuitError. A matrix body must be
    /// unitary and a channel body trace-preserving, or the call throws.
    /// @{

    /// Defines a single-qubit body from a literal 2x2 matrix.
    Circuit& define_opaque(const std::string& name,
                           const std::array<std::array<std::complex<double>, 2>, 2>& matrix);
    /// Defines a two-qubit body from a literal 4x4 matrix.
    Circuit& define_opaque(const std::string& name,
                           const std::array<std::array<std::complex<double>, 4>, 4>& matrix);
    /// Defines an n-qubit body from a general 2^n x 2^n matrix.
    Circuit& define_opaque(const std::string&   name,
                           QubitIndex           num_qubits,
                           const UnitaryMatrix& matrix);
    /// Defines a non-unitary body from a Kraus channel.
    Circuit& define_opaque(const std::string&  name,
                           const KrausChannel& channel);
    /// @}

    /// opaque(): append an opaque CALL to the circuit. Produces the
    ///   same GateOp the QASM parser builds for `<name> q[i], q[j];` — the
    ///   program path had no way to place an opaque op before this.
    /// Qubit order is significant: the definition matrix's local bit b maps to
    ///   qubits[b], so opaque("g", {2, 0}) and opaque("g", {0, 2}) differ.
    /// A definition is NOT required yet — QASM v2's "declare first, define
    ///   later" order is preserved on this path too, and SemanticValidatorPass
    /// rejects a still-undefined opaque at transpile() time.
    ///   When a definition already exists its arity is checked immediately.
    /// @param name   Opaque gate name to call.
    /// @param qubits  Operands in definition-local bit order: local bit b of the
    ///                definition matrix maps to qubits[b], so the order is
    ///                significant and never normalised.
    /// @return *this, so calls chain.
    /// @throws CircuitError An operand is out of range, or a definition already
    ///                      exists and its arity differs from the operand count.
    Circuit& opaque(const std::string& name, std::initializer_list<QubitIndex> qubits);

    /// Whether an opaque name is registered at all, stub included.
    /// A DeclarationOnly stub from declare_opaque() answers true — callers that
    /// need executability must also test holds_alternative<DeclarationOnly> on
    /// the body, which is why SemanticValidatorPass checks the two separately.
    /// @param name Opaque gate name.
    /// @return true when opaque_definition() will return without throwing.
    bool                                   has_opaque_definition(const std::string& name) const;

    /// Definition registered for an opaque name.
    /// @param name Opaque gate name.
    /// @return Reference to the stored definition.
    /// @throws CircuitError The name was never declared or defined.
    const OpaqueDefinition&                opaque_definition    (const std::string& name) const;

    /// All opaque definitions held by this circuit.
    /// @return Reference to the definition list, in registration order.
    const std::vector<OpaqueDefinition>&   opaque_definitions   () const noexcept;

    /// ── Queries ────────────────────────────────────────────────────
    /// All const noexcept -- simple member returns. ops() and metadata() are
    /// library-internal collaborators (PassManager / Backend / serialiser);
    /// user code should prefer the 25 builder methods.
    /// @return Number of qubits the circuit spans.
    QubitIndex        num_qubits() const noexcept;
    /// @return Total classical bits; identical to total_cubits().
    ClassicalBitIndex num_cubits() const noexcept;
    /// @return Number of ops currently in the stream.
    std::size_t       num_ops()    const noexcept;
    /// @return Reference to the op stream, in execution order.
    const std::vector<Op>&        ops()      const noexcept;
    /// @return Reference to the circuit-level annotations (layout, truncation map).
    const CircuitMetadata&        metadata() const noexcept;

    /// True when the circuit contains an IfOp or a MeasureOp that is followed
    /// by another non-Measure / non-Barrier op on the same qubit.
    ///
    /// This is the LITERAL question — "does this circuit use classical control".
    /// For "must this circuit run per shot", ask requires_per_shot(): the two
    /// were one predicate until, and merging them would make the GPU's
    /// "classical-control circuits not supported" rejection fire on a plain
    /// reset circuit.
    /// @return true when the circuit uses classical control.
    bool has_classical_control() const;

    /// True when execution must take the per-shot path.
    ///
    /// That is has_classical_control(), OR a Reset landing on a qubit a gate has
    /// already touched. A reset is a measurement plus a conditional X, so it is
    /// STOCHASTIC: evolving it once and drawing nshots from the result reports a
    /// single collapse branch nshots times, which on an entangled qubit is a
    /// visible error rather than a subtlety.
    ///
    /// Deliberately conservative — `x q; reset q` is deterministic and still
    /// returns true. Deciding otherwise would need static state tracking, which
    /// is simulation; a false positive only costs speed, while a false negative
    /// is a wrong answer. The common idiom (a leading `reset q[i];` on untouched
    /// qubits) is not caught, so the cost in practice is small.
    /// @return true when execution must take the per-shot path.
    bool requires_per_shot() const;

    /// ── Serialisation ────────────────
    /// to_qasm: OpenQASM v2 emission with std::setprecision(17) for round-trip
    ///   numeric fidelity. Kraus opaque -> CircuitError (QASM v2 cannot
    /// express non-unitary channels). IdleOp omitted from output.
    /// to_json / from_json / load_json_file: lossless JSON round-trip including
    ///   opaque_definitions and IdleOp{type:"idle", ...} nodes.
    /// Emits OpenQASM v2 source for this circuit.
    /// @return The QASM text, numbers written at 17 significant digits so a
    ///         round-trip is numerically lossless.
    /// @throws CircuitError The circuit holds a Kraus opaque, which QASM v2 cannot
    ///                      express.
    std::string to_qasm() const;

    /// Serialises the circuit to JSON.
    /// @return JSON text covering the op stream, opaque definitions, expectation
    ///         targets and IdleOp nodes — a lossless round-trip with from_json().
    std::string to_json() const;

    /// Rebuilds a circuit from JSON text produced by to_json().
    /// @param text JSON document.
    /// @return The decoded circuit.
    /// @throws CircuitError Malformed JSON or a schema violation.
    static Circuit from_json     (std::string_view text);

    /// Rebuilds a circuit from a JSON file produced by to_json().
    /// @param path Path to the JSON file.
    /// @return The decoded circuit.
    /// @throws CircuitError I/O failure, malformed JSON, or a schema violation.
    static Circuit load_json_file(const std::string& path);

    /// ── Diagram rendering ──────────────────────
    /// Render the circuit as a human-readable text diagram. A sibling output
    /// of to_qasm / to_json; delegates to the free fn draw_circuit_text below.
    /// Walks ops() only (no backend), so Circuit owns the logic. measure /
    /// barrier / IfOp / reset / opaque / params / multi-qubit control lines are
    /// all visualised. Image output (matplotlib) is Python-only.
    /// @param style Character set used for the diagram.
    /// @return The rendered diagram.
    std::string draw(CircuitDrawStyle style = CircuitDrawStyle::Unicode) const;

private:
    /// Storage. friend declarations let the Backend / PassManager mutate the
    /// op stream without breaking Circuit's invariants (defined in Phase 2).
    QubitIndex                       num_qubits_ = 0;
    std::vector<Op>                  ops_;
    std::vector<ClassicalRegister>   classical_registers_;
    std::vector<OpaqueDefinition>    opaque_definitions_;
    std::vector<std::string>         expectation_targets_;  //
    CircuitMetadata                  metadata_;

    /// Serialiser bridge: serializer.cpp hands a decoded payload directly
    /// into Circuit without exposing the private fields via setters. Used
    /// only by Circuit::from_json / load_json_file. Free function declared
    /// immediately after the class.
    friend Circuit detail_make_circuit_from_payload(QubitIndex                     num_qubits,
                                                    std::vector<ClassicalRegister> regs,
                                                    std::vector<OpaqueDefinition>  opaques,
                                                    std::vector<Op>                ops,
                                                    std::vector<std::string>       expectation_targets,
                                                    CircuitMetadata                meta);

    /// Parser bridge: CircuitBuilder appends pre-built Op nodes for
    /// BarrierOp (from vector<QubitIndex>) and Opaque GateOp where the public
    /// builder API has no matching method. Not part of the user-facing API.
    friend void detail_circuit_push_op(Circuit& c, Op op);
};

// ── Serialiser free functions ─────────────────────────────────────────
// Defined in serializer.cpp and qasm_emitter.cpp. The Circuit member methods
// to_json / from_json / load_json_file / to_qasm delegate to these so the
// large encode / decode bodies stay in their own translation units.

/// Encodes a circuit as JSON; Circuit::to_json() delegates here.
/// @param c Circuit to encode.
/// @return The JSON text.
std::string serialize_circuit_json  (const Circuit& c);

/// Decodes JSON text into a circuit; Circuit::from_json() delegates here.
/// @param text JSON document.
/// @return The decoded circuit.
/// @throws CircuitError Malformed JSON or a schema violation.
Circuit     deserialize_circuit_json(std::string_view text);

/// Reads and decodes a JSON file; Circuit::load_json_file() delegates here.
/// @param path Path to the JSON file.
/// @return The decoded circuit.
/// @throws CircuitError I/O failure, malformed JSON, or a schema violation.
Circuit     load_circuit_json_file  (const std::string& path);

/// Emits OpenQASM v2 text; Circuit::to_qasm() delegates here.
/// @param c Circuit to emit.
/// @return The QASM text.
/// @throws CircuitError The circuit holds a Kraus opaque.
std::string emit_qasm_v2            (const Circuit& c);
/// Renders a circuit as a text diagram; Circuit::draw() delegates here
/// Defined in circuit_draw.cpp.
/// @param c     Circuit to render.
/// @param style Character set used for the diagram.
/// @return The rendered diagram.
std::string draw_circuit_text       (const Circuit& c, CircuitDrawStyle style);

/// Internal payload constructor -- friend-ed by Circuit. detail-scoped name
/// keeps it out of the user-facing surface (not part).
/// @param num_qubits          Circuit width.
/// @param regs                Classical registers to install.
/// @param opaques             Opaque definitions to install.
/// @param ops                 Op stream to install.
/// @param expectation_targets Pre-registered Pauli strings.
/// @param meta                Circuit-level annotations.
/// @return The assembled circuit.
Circuit detail_make_circuit_from_payload(QubitIndex                                num_qubits,
                                         std::vector<ClassicalRegister>            regs,
                                         std::vector<OpaqueDefinition>             opaques,
                                         std::vector<Op>                           ops,
                                         std::vector<std::string>                  expectation_targets,
                                         CircuitMetadata                           meta);

/// Internal op-append bridge for CircuitBuilder. Lets the parser's
/// CircuitBuilder push pre-built Op nodes (BarrierOp, Opaque GateOp) directly
/// into Circuit::ops_ without exposing a public push_back API. Not part of the
/// user-facing surface. Declared as a free function (not a
/// member) so CircuitBuilder can call it without including the full class
/// interface in detail headers. Implemented in circuit.cpp.
/// @param c  Circuit to append to.
/// @param op Pre-built op to append.
void detail_circuit_push_op(Circuit& c, Op op);

}  // namespace qplayer
