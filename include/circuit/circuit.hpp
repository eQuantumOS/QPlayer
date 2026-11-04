// circuit.hpp — Public Circuit IR builder facade.
// Method chaining returns Circuit& so users can write circ.h(0).cx(0,1)....
// Validation (qubit range, dimension match for opaque definitions) happens
// inside method bodies — out-of-range / duplicate-name failures throw
// CircuitError immediately.

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

// One creg as declared in OpenQASM v2 (`creg <name>[width];`). offset is the
// running cubit index assigned by Circuit when the register is added; it lets
// MeasureOp.cubit address a global cubit space across multiple registers
//
struct ClassicalRegister {
    std::string       name;
    ClassicalBitIndex width  = 0;
    ClassicalBitIndex offset = 0;
};

// Text drawing style for Circuit::draw.
//   Unicode: box-drawing glyphs (─ │ ┤ ├ ┼ ●), best readability, assumes a
//            UTF-8 terminal (default).
//   Ascii:   plain ASCII (- | + *), renders in any terminal / log file.
enum class CircuitDrawStyle { Unicode, Ascii };

class Circuit {
public:
    // ── Construction ───────────────────────────────────────────────
    // num_cubits > 0 auto-creates a default register named "c" at index 0
    // num_cubits == 0 leaves classical_registers_ empty.
    explicit Circuit(QubitIndex num_qubits, ClassicalBitIndex num_cubits = 0);

    // ── Single-qubit gates (chainable) — qelib1.inc id, x, y, z, h, s, sdg, t, tdg, sx, sxdg ─
    Circuit& id (QubitIndex q);                                                    //
    Circuit& h  (QubitIndex q);
    Circuit& x  (QubitIndex q);
    Circuit& y  (QubitIndex q);
    Circuit& z  (QubitIndex q);
    Circuit& s  (QubitIndex q);
    Circuit& sdg(QubitIndex q);
    Circuit& t  (QubitIndex q);
    Circuit& tdg(QubitIndex q);
    Circuit& sx  (QubitIndex q);                                                   // sqrt(X)
    Circuit& sxdg(QubitIndex q);                                                   // sqrt(X) dagger

    // ── Single-qubit rotations / U gates — qelib1.inc rx, ry, rz, u1, u2, u3 ─
    // Built-in U(theta,phi,lambda) aliases u3 at parse time.
    Circuit& rx(double theta,             QubitIndex q);
    Circuit& ry(double theta,             QubitIndex q);
    Circuit& rz(double theta,             QubitIndex q);
    Circuit& u1(double lam,               QubitIndex q);
    Circuit& u2(double phi, double lam,   QubitIndex q);
    Circuit& u3(double theta, double phi, double lam, QubitIndex q);

    // ── Two-qubit gates — qelib1.inc cx, cy, cz, ch, swap, crz, cu1, cu3 ─
    // Built-in CX a,b aliases cx at parse time.
    Circuit& cx  (QubitIndex ctrl, QubitIndex tgt);
    Circuit& cy  (QubitIndex ctrl, QubitIndex tgt);
    Circuit& cz  (QubitIndex ctrl, QubitIndex tgt);
    Circuit& ch  (QubitIndex ctrl, QubitIndex tgt);                                //
    Circuit& swap(QubitIndex q0,   QubitIndex q1);
    Circuit& crz (double theta,             QubitIndex ctrl, QubitIndex tgt);     //
    Circuit& cu1 (double lam,               QubitIndex ctrl, QubitIndex tgt);     //
    Circuit& cu3 (double theta, double phi, double lam,
                  QubitIndex ctrl, QubitIndex tgt);                                //
    // ECR (Echoed Cross-Resonance) — IBM Eagle native 2Q basis gate. Genuine 4x4,
    // self-inverse (Hermitian); created here or by basis translation, never via QASM.
    Circuit& ecr (QubitIndex q0, QubitIndex q1);

    // Google native 2Q basis gates. All genuine 4x4 unitaries, none
    // self-inverse, not in qelib1.inc — never created via OpenQASM v2.
    // iswap: [[1,0,0,0],[0,0,i,0],[0,i,0,0],[0,0,0,1]].
    // sqrt_iswap: [[1,0,0,0],[0,1/√2,i/√2,0],[0,i/√2,1/√2,0],[0,0,0,1]]; sqrt_iswap²=iswap.
    // fsim(theta,phi): fSim gate; sycamore() is fsim(π/2, π/6).
    Circuit& iswap     (QubitIndex q0, QubitIndex q1);
    Circuit& sqrt_iswap(QubitIndex q0, QubitIndex q1);
    Circuit& fsim      (double theta, double phi, QubitIndex q0, QubitIndex q1);
    Circuit& sycamore  (QubitIndex q0, QubitIndex q1);  // fsim(pi/2, pi/6)

    // ── Three-qubit gates — qelib1.inc ccx (Toffoli), cswap (Fredkin) ─
    Circuit& ccx  (QubitIndex c0, QubitIndex c1, QubitIndex tgt);
    Circuit& cswap(QubitIndex ctrl, QubitIndex t0, QubitIndex t1);                 //

    // ── Control flow ──────────────────────────────────────────────
    // measure: store qubit measurement into the given cubit.
    // measure_all: shorthand for measure(i, i) over every qubit (assumes the
    // default register width >= num_qubits, else CircuitError).
    // barrier with empty list applies to all qubits.
    // if_then: classical-conditioned single-gate dispatch.
    Circuit& measure   (QubitIndex q, ClassicalBitIndex cubit);
    Circuit& measure_all();
    Circuit& barrier   (std::initializer_list<QubitIndex> qubits = {});
    Circuit& reset     (QubitIndex q);
    Circuit& if_then   (ClassicalRegisterIndex creg,
                        std::uint64_t          value,
                        const GateOp&          gate);

    // ── Classical register management ───────────────────
    // add_creg: returns the index of the newly created register.
    //   - Duplicate name -> CircuitError.
    //   - width == 0     -> CircuitError.
    // creg(name): name lookup. Missing name -> CircuitError.
    ClassicalRegisterIndex add_creg(const std::string& name,
                                    ClassicalBitIndex  width);
    ClassicalRegisterIndex creg    (const std::string& name) const;

    const std::vector<ClassicalRegister>& classical_registers() const noexcept;

    // Sum of all register widths — synonym for num_cubits().
    ClassicalBitIndex total_cubits() const noexcept;

    // ── Expectation-value targets ──────────────
    // Pre-register a Pauli string whose expectation run() accumulates into
    // Result.expectation (model A). Qiskit qubit order: pauli[0] is the
    // highest qubit. Validation: I/X/Y/Z only, length == num_qubits, empty
    // string -> CircuitError. Duplicate registration is idempotent. Chainable.
    // Persisted by to_json/from_json so the target set survives round-trips
    // The one-shot post-hoc QRegister::expectation_value(pauli)
    // is a distinct quantity (single value vs nshot average).
    Circuit& add_expectation(std::string_view pauli);
    const std::vector<std::string>& expectation_targets() const noexcept;

    // ── Opaque definitions ──────────────────
    // declare_opaque: register stub (body = DeclarationOnly). Used by the
    //   QASM parser when it encounters `opaque <name> <qubits>;`. Calling
    //   define_opaque() afterwards promotes the body in-place exactly once
    //   (DeclarationOnly -> UnitaryMatrix or KrausChannel).
    // define_opaque overload set:
    //   - 1Q 2x2 std::array       (literal matrix syntax)
    //   - 2Q 4x4 std::array       (literal matrix syntax)
    //   - NQ vector<vector>       (general matrix, num_qubits >= 3 typical)
    //   - KrausChannel            (non-unitary channel)
    // Body promotion rules:
    //   DeclarationOnly -> UnitaryMatrix / KrausChannel : OK
    //   UnitaryMatrix   -> KrausChannel                  : CircuitError (single-direction promotion only)
    //   KrausChannel    -> UnitaryMatrix                 : CircuitError
    //   any concrete    -> DeclarationOnly               : CircuitError (no demotion)
    Circuit& declare_opaque(const std::string& name, QubitIndex num_qubits);

    Circuit& define_opaque(const std::string& name,
                           const std::array<std::array<std::complex<double>, 2>, 2>& matrix);
    Circuit& define_opaque(const std::string& name,
                           const std::array<std::array<std::complex<double>, 4>, 4>& matrix);
    Circuit& define_opaque(const std::string&   name,
                           QubitIndex           num_qubits,
                           const UnitaryMatrix& matrix);
    Circuit& define_opaque(const std::string&  name,
                           const KrausChannel& channel);

    bool                                   has_opaque_definition(const std::string& name) const;
    const OpaqueDefinition&                opaque_definition    (const std::string& name) const;
    const std::vector<OpaqueDefinition>&   opaque_definitions   () const noexcept;

    // ── Queries ────────────────────────────────────────────────────
    // All const noexcept -- simple member returns. ops() and metadata() are
    // library-internal collaborators (PassManager / Backend / serialiser);
    // user code should prefer the 25 builder methods.
    QubitIndex        num_qubits() const noexcept;
    ClassicalBitIndex num_cubits() const noexcept;        // == total_cubits().
    std::size_t       num_ops()    const noexcept;
    const std::vector<Op>&        ops()      const noexcept;
    const CircuitMetadata&        metadata() const noexcept;

    // True when the circuit contains an IfOp or a MeasureOp that is followed
    // by another non-Measure / non-Barrier op on the same qubit. Drives the
    // fast path vs per-shot path selection.
    bool has_classical_control() const;

    // ── Serialisation ────────────────
    // to_qasm: OpenQASM v2 emission with std::setprecision(17) for round-trip
    //   numeric fidelity. Kraus opaque -> CircuitError (QASM v2 cannot
    // express non-unitary channels). IdleOp omitted from output.
    // to_json / from_json / load_json_file: lossless JSON round-trip including
    //   opaque_definitions and IdleOp{type:"idle", ...} nodes.
    std::string to_qasm() const;
    std::string to_json() const;

    static Circuit from_json     (std::string_view text);
    static Circuit load_json_file(const std::string& path);

    // ── Diagram rendering ──────────────────────
    // Render the circuit as a human-readable text diagram. A sibling output
    // of to_qasm / to_json; delegates to the free fn draw_circuit_text below.
    // Walks ops() only (no backend), so Circuit owns the logic. measure /
    // barrier / IfOp / reset / opaque / params / multi-qubit control lines are
    // all visualised. Image output (matplotlib) is Python-only.
    std::string draw(CircuitDrawStyle style = CircuitDrawStyle::Unicode) const;

private:
    // Storage. friend declarations let the Backend / PassManager mutate the
    // op stream without breaking Circuit's invariants (defined in Phase 2).
    QubitIndex                       num_qubits_ = 0;
    std::vector<Op>                  ops_;
    std::vector<ClassicalRegister>   classical_registers_;
    std::vector<OpaqueDefinition>    opaque_definitions_;
    std::vector<std::string>         expectation_targets_;  //
    CircuitMetadata                  metadata_;

    // Serialiser bridge: serializer.cpp hands a decoded payload directly
    // into Circuit without exposing the private fields via setters. Used
    // only by Circuit::from_json / load_json_file. Free function declared
    // immediately after the class.
    friend Circuit detail_make_circuit_from_payload(QubitIndex,
                                                    std::vector<ClassicalRegister>,
                                                    std::vector<OpaqueDefinition>,
                                                    std::vector<Op>,
                                                    std::vector<std::string>,
                                                    CircuitMetadata);

    // Parser bridge: CircuitBuilder appends pre-built Op nodes for
    // BarrierOp (from vector<QubitIndex>) and Opaque GateOp where the public
    // builder API has no matching method. Not part of the user-facing API.
    friend void detail_circuit_push_op(Circuit& c, Op op);
};

// ── Serialiser free functions ─────────────────────────────────────────
// Defined in serializer.cpp and qasm_emitter.cpp. The Circuit member methods
// to_json / from_json / load_json_file / to_qasm delegate to these so the
// large encode / decode bodies stay in their own translation units.

std::string serialize_circuit_json  (const Circuit& c);
Circuit     deserialize_circuit_json(std::string_view text);
Circuit     load_circuit_json_file  (const std::string& path);
std::string emit_qasm_v2            (const Circuit& c);
// Defined in circuit_draw.cpp. Circuit::draw delegates here.
std::string draw_circuit_text       (const Circuit& c, CircuitDrawStyle style);

// Internal payload constructor -- friend-ed by Circuit. detail-scoped name
// keeps it out of the user-facing surface (not part of).
Circuit detail_make_circuit_from_payload(QubitIndex                                num_qubits,
                                         std::vector<ClassicalRegister>            regs,
                                         std::vector<OpaqueDefinition>             opaques,
                                         std::vector<Op>                           ops,
                                         std::vector<std::string>                  expectation_targets,
                                         CircuitMetadata                           meta);

// Internal op-append bridge for CircuitBuilder. Lets the parser's
// CircuitBuilder push pre-built Op nodes (BarrierOp, Opaque GateOp) directly
// into Circuit::ops_ without exposing a public push_back API. Not part of the
// user-facing surface. Declared as a free function (not a
// member) so CircuitBuilder can call it without including the full class
// interface in detail headers. Implemented in circuit.cpp.
void detail_circuit_push_op(Circuit& c, Op op);

}  // namespace qplayer
