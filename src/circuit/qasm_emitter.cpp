// qasm_emitter.cpp -- Circuit -> OpenQASM v2 text emission.
// Implements emit_qasm_v2(const Circuit&) used by Circuit::to_qasm().
//
// Emission rules:
//   * Header:   OPENQASM 2.0;
//               include "qelib1.inc";
//   * Qubits:   qreg q[n];
//   * Cubits:   creg <name>[<width>];   (one line per ClassicalRegister)
//   * Opaque:   opaque <name> q1, ..., qk;   (matrix body cannot be expressed
//               in QASM v2; we emit a comment noting the bypass.)
//   * Gates:    h q[i]; / rx(theta) q[i]; / cx q[a], q[b]; ...
//   * Measure:  measure q[i] -> <reg>[<offset>];   (global cubit decoded
// back into the owning register via offsets.)
//   * Barrier:  barrier q[a], q[b]; / barrier;     (empty list -> bare)
//   * Reset:    reset q[i];
//   * IfOp:     if (<reg>==<value>) <body-gate>;
// * IdleOp: omitted (-- no QASM v2 representation).
//   * Kraus opaque: CircuitError throw (lossless round-trip impossible).

#include "circuit/circuit.hpp"

#include <iomanip>
#include <sstream>
#include <string>
#include <variant>

#include "error.hpp"

namespace qplayer {

namespace {

// Render a double with std::setprecision(17) so QRegister::parse() recovers
// the exact value. Avoids trailing zeros that some
// foreign QASM parsers refuse.
std::string format_param(double v) {
    std::ostringstream out;
    out << std::setprecision(17) << v;
    std::string s = out.str();
    // Make sure the literal looks like a float ("1" -> "1.0") so the
    // QPlayer parser's number rule recognises it as a real parameter.
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

void emit_qubit(std::ostringstream& out, QubitIndex q) {
    out << "q[" << q << "]";
}

// Resolve a global cubit index back to its owning register + local offset
// Linear scan -- classical_registers is small in practice
// (typically 1-2 entries), so the cost is irrelevant compared to I/O.
std::pair<const ClassicalRegister*, ClassicalBitIndex>
locate_cubit(const std::vector<ClassicalRegister>& regs, ClassicalBitIndex cubit) {
    for (const auto& cr : regs) {
        if (cubit >= cr.offset && cubit < cr.offset + cr.width) {
            return {&cr, static_cast<ClassicalBitIndex>(cubit - cr.offset)};
        }
    }
    return {nullptr, 0};
}

void emit_cubit_ref(std::ostringstream& out,
                    const std::vector<ClassicalRegister>& regs,
                    ClassicalBitIndex cubit) {
    const auto [cr, local] = locate_cubit(regs, cubit);
    if (cr == nullptr) {
        throw CircuitError{"to_qasm: cubit " + std::to_string(cubit) +
                           " has no owning ClassicalRegister"};
    }
    out << cr->name << '[' << local << ']';
}

// Render a single GateOp (no surrounding semicolon). Used both for top-level
// gate emission and for IfOp body emission.
void emit_gate_call(std::ostringstream& out, const GateOp& g) {
    // Opaque dispatch: emit the user-provided opaque_name as-is. Builders
    // store the gate's canonical name in `name` for qelib1 gates, and in
    // `opaque_name` for opaque calls.
    // ECR has no OpenQASM v2 spelling (not in qelib1.inc); emitting "ecr ..."
    // would produce QASM that cannot be re-parsed. Throw rather than emit a
    // lossy/invalid token (— ECR is not a QASM-exposed gate).
    if (g.type == GateType::ECR) {
        throw CircuitError{"to_qasm: cannot emit ECR gate (no OpenQASM v2 "
                           "representation; use to_json for round-trip)"};
    }
    // Google native 2Q gates have no OpenQASM v2 spelling.
    if (g.type == GateType::ISWAP) {
        throw CircuitError{"to_qasm: cannot emit iSWAP gate (no OpenQASM v2 "
                           "representation; use to_json for round-trip)"};
    }
    if (g.type == GateType::SQRT_ISWAP) {
        throw CircuitError{"to_qasm: cannot emit sqrt_iSWAP gate (no OpenQASM v2 "
                           "representation; use to_json for round-trip)"};
    }
    if (g.type == GateType::FSIM) {
        throw CircuitError{"to_qasm: cannot emit fSim gate (no OpenQASM v2 "
                           "representation; use to_json for round-trip)"};
    }

    const std::string& callable = g.is_opaque ? g.opaque_name : g.name;
    out << callable;

    if (!g.params.empty()) {
        out << '(';
        for (std::size_t i = 0; i < g.params.size(); ++i) {
            if (i > 0) out << ", ";
            out << format_param(g.params[i]);
        }
        out << ')';
    }

    out << ' ';
    for (std::size_t i = 0; i < g.qubits.size(); ++i) {
        if (i > 0) out << ", ";
        emit_qubit(out, g.qubits[i]);
    }
}

void emit_op(std::ostringstream& out,
             const Op& op,
             const std::vector<ClassicalRegister>& regs,
             const std::vector<ClassicalRegister>& /*regs_for_if*/) {
    if (const auto* g = std::get_if<GateOp>(&op)) {
        // Reset is encoded as GateOp{type=Reset, name="reset"}, but QASM v2
        // spells it as `reset q[i];` (no qelib1 entry). The current builder
        // sets name="reset" so the generic emit_gate_call path naturally
        // produces the right token without needing a special case here.
        emit_gate_call(out, *g);
        out << ";\n";
        return;
    }

    if (const auto* m = std::get_if<MeasureOp>(&op)) {
        out << "measure ";
        emit_qubit(out, m->qubit);
        out << " -> ";
        emit_cubit_ref(out, regs, m->cubit);
        out << ";\n";
        return;
    }

    if (const auto* b = std::get_if<BarrierOp>(&op)) {
        out << "barrier";
        if (!b->qubits.empty()) {
            out << ' ';
            for (std::size_t i = 0; i < b->qubits.size(); ++i) {
                if (i > 0) out << ", ";
                emit_qubit(out, b->qubits[i]);
            }
        }
        out << ";\n";
        return;
    }

    if (const auto* i = std::get_if<IfOp>(&op)) {
        if (i->creg >= regs.size()) {
            throw CircuitError{"to_qasm: IfOp.creg out of range"};
        }
        out << "if (" << regs[i->creg].name << "==" << i->value << ") ";
        emit_gate_call(out, i->body);
        out << ";\n";
        return;
    }

    if (std::holds_alternative<IdleOp>(op)) {
        // : IdleOp has no QASM v2 representation. Omit silently --
        // round-trip equivalence is defined under the IdleOp-stripped
        // normalisation.
        return;
    }

    throw CircuitError{"to_qasm: unknown Op variant alternative"};
}

}  // namespace

std::string emit_qasm_v2(const Circuit& c) {
    // Pre-check: Kraus opaque cannot be expressed in QASM v2 -- throw before
    // we emit any text so the user gets a single clear diagnostic instead
    // of a partial dump.
    for (const auto& def : c.opaque_definitions()) {
        if (std::holds_alternative<KrausChannel>(def.body)) {
            throw CircuitError{"to_qasm: cannot emit Kraus opaque \"" + def.name +
                               "\" to QASM v2 -- use to_json() instead "
                               ""};
        }
    }

    std::ostringstream out;
    out << "OPENQASM 2.0;\n";
    out << "include \"qelib1.inc\";\n";
    out << '\n';

    // Opaque declarations. Matrix bodies cannot be expressed inline; we emit
    // the declaration + a comment so the user knows the body was bypassed.
    for (const auto& def : c.opaque_definitions()) {
        out << "opaque " << def.name;
        out << ' ';
        for (QubitIndex q = 0; q < def.num_qubits; ++q) {
            if (q > 0) out << ", ";
            out << 'q' << q;
        }
        out << ";\n";
        if (std::holds_alternative<UnitaryMatrix>(def.body)) {
            out << "// opaque \"" << def.name
                << "\": matrix body bypassed; use to_json() for lossless round-trip\n";
        }
    }
    if (!c.opaque_definitions().empty()) out << '\n';

    // Registers: one qreg q[N] plus one creg per ClassicalRegister.
    out << "qreg q[" << c.num_qubits() << "];\n";
    for (const auto& cr : c.classical_registers()) {
        out << "creg " << cr.name << '[' << cr.width << "];\n";
    }
    if (c.num_qubits() > 0 || !c.classical_registers().empty()) out << '\n';

    // Body.
    for (const auto& op : c.ops()) {
        emit_op(out, op, c.classical_registers(), c.classical_registers());
    }

    return out.str();
}

}  // namespace qplayer
