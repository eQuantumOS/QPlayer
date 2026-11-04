// semantic_validator.cpp — AST-level semantic validator for OpenQASM v2
// See header for full responsibility list.
//
// One-pass forward-only validation: QASM v2 requires declarations before use
// so we register symbols as we encounter declaration statements and
// validate call/reference sites immediately afterwards.
//
// Lookup precedence for gate names:
//   1. user_gates_ — user-defined `gate` bodies shadow standard names.
//   2. opaques_    — opaque declarations.
//   3. kQelib1Signatures — built-in U/CX + 25 qelib1 gates.
// Undeclared names after all three lookups → QASMParseError.

#include "parser/detail/semantic_validator.hpp"
#include "error.hpp"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace qplayer::detail {

// ── Constructor ───────────────────────────────────────────────────────────────

SemanticValidator::SemanticValidator(std::string file_name)
    : file_(std::move(file_name)) {}

// ── Public entry point ────────────────────────────────────────────────────────

void SemanticValidator::validate(const Program& program) {
    for (const auto& stmt : program.statements) {
        validate_statement(stmt);
    }
}

// ── Registration helpers ──────────────────────────────────────────────────────

void SemanticValidator::register_qreg(const QRegDecl& decl) {
    // Duplicate qreg name: same name used twice.
    if (qregs_.count(decl.name)) {
        throw qplayer::QASMParseError{
            file_, decl.line, decl.col,
            "duplicate qreg declaration: '" + decl.name + "'"};
    }
    // A qreg name must not shadow a creg — they live in separate namespaces but
    // QASM v2 forbids aliasing register names across kinds for readability.
    if (cregs_.count(decl.name)) {
        throw qplayer::QASMParseError{
            file_, decl.line, decl.col,
            "qreg name '" + decl.name +
                "' already used as a creg in this program"};
    }
    qregs_[decl.name] = decl.size;
}

void SemanticValidator::register_creg(const CRegDecl& decl) {
    if (cregs_.count(decl.name)) {
        throw qplayer::QASMParseError{
            file_, decl.line, decl.col,
            "duplicate creg declaration: '" + decl.name + "'"};
    }
    if (qregs_.count(decl.name)) {
        throw qplayer::QASMParseError{
            file_, decl.line, decl.col,
            "creg name '" + decl.name +
                "' already used as a qreg in this program"};
    }
    cregs_[decl.name] = decl.size;
}

void SemanticValidator::register_gate_def(const GateDef& def) {
    // gate and opaque share one namespace.
    if (user_gates_.count(def.name) || opaques_.count(def.name)) {
        throw qplayer::QASMParseError{
            file_, def.line, def.col,
            "duplicate gate definition: '" + def.name +
                "' already declared in this program"};
    }
    GateSignature sig;
    sig.name       = "";   // not used for lookup; key is the map key
    sig.num_params = static_cast<int>(def.params.size());
    sig.num_qubits = static_cast<int>(def.qargs.size());
    user_gates_.emplace(def.name, sig);
}

void SemanticValidator::register_opaque(const OpaqueDecl& decl) {
    if (user_gates_.count(decl.name) || opaques_.count(decl.name)) {
        throw qplayer::QASMParseError{
            file_, decl.line, decl.col,
            "duplicate opaque declaration: '" + decl.name +
                "' already declared as a gate or opaque"};
    }
    GateSignature sig;
    sig.name       = "";
    sig.num_params = static_cast<int>(decl.params.size());
    sig.num_qubits = static_cast<int>(decl.qargs.size());
    opaques_.emplace(decl.name, sig);
}

// ── Gate-signature lookup ─────────────────────────────────────────────────────

const GateSignature* SemanticValidator::lookup_gate(
    const std::string& name) const {
    // 1. User-defined gates shadow everything.
    auto it = user_gates_.find(name);
    if (it != user_gates_.end()) return &it->second;

    // 2. Opaque declarations.
    auto oit = opaques_.find(name);
    if (oit != opaques_.end()) return &oit->second;

    // 3. Standard qelib1 + built-in U/CX.
    return find_qelib1_signature(name);
}

// ── Statement dispatcher ──────────────────────────────────────────────────────

void SemanticValidator::validate_statement(const AstStmt& stmt) {
    std::visit([this](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, VersionDecl>) {
            // Version header: no semantic constraint beyond parser acceptance.
            (void)s;
        } else if constexpr (std::is_same_v<T, IncludeStmt>) {
            // Include path: resolution is responsibility. The validator
            // notes qelib1.inc presence conceptually but takes no action here —
            // all standard gate signatures are already in kQelib1Signatures.
            (void)s;
        } else if constexpr (std::is_same_v<T, QRegDecl>) {
            register_qreg(s);
        } else if constexpr (std::is_same_v<T, CRegDecl>) {
            register_creg(s);
        } else if constexpr (std::is_same_v<T, GateDef>) {
            // Register the name first, then validate the body.
            register_gate_def(s);
            validate_gate_def_body(s);
        } else if constexpr (std::is_same_v<T, OpaqueDecl>) {
            register_opaque(s);
        } else if constexpr (std::is_same_v<T, GateCall>) {
            validate_gate_call_toplevel(s);
        } else if constexpr (std::is_same_v<T, MeasureStmt>) {
            validate_measure(s);
        } else if constexpr (std::is_same_v<T, ResetStmt>) {
            validate_reset(s);
        } else if constexpr (std::is_same_v<T, BarrierStmt>) {
            validate_barrier(s);
        } else if constexpr (std::is_same_v<T, IfStmt>) {
            validate_if(s);
        }
    }, stmt.v);
}

// ── Register-reference helpers ────────────────────────────────────────────────

void SemanticValidator::validate_reg_ref_toplevel(const RegRef& ref,
                                                   bool is_qubit,
                                                   bool allow_whole) {
    if (is_qubit) {
        auto it = qregs_.find(ref.name);
        if (it == qregs_.end()) {
            throw qplayer::QASMParseError{
                file_, ref.line, ref.col,
                "undeclared qubit register '" + ref.name + "'"};
        }
        if (!ref.index.has_value()) {
            if (!allow_whole) {
                throw qplayer::QASMParseError{
                    file_, ref.line, ref.col,
                    "qubit register '" + ref.name +
                        "' used without an index (whole-register reference "
                        "not allowed here)"};
            }
            return;  // whole-register reference is valid in barrier context
        }
        const int idx  = *ref.index;
        const int size = it->second;
        if (idx < 0 || idx >= size) {
            std::ostringstream oss;
            oss << "qubit index " << idx << " out of range for register '"
                << ref.name << "' (size " << size << ")";
            throw qplayer::QASMParseError{file_, ref.line, ref.col, oss.str()};
        }
    } else {
        auto it = cregs_.find(ref.name);
        if (it == cregs_.end()) {
            throw qplayer::QASMParseError{
                file_, ref.line, ref.col,
                "undeclared classical register '" + ref.name + "'"};
        }
        if (!ref.index.has_value()) {
            if (!allow_whole) {
                throw qplayer::QASMParseError{
                    file_, ref.line, ref.col,
                    "classical register '" + ref.name +
                        "' used without an index"};
            }
            return;
        }
        const int idx  = *ref.index;
        const int size = it->second;
        if (idx < 0 || idx >= size) {
            std::ostringstream oss;
            oss << "classical bit index " << idx
                << " out of range for register '" << ref.name
                << "' (size " << size << ")";
            throw qplayer::QASMParseError{file_, ref.line, ref.col, oss.str()};
        }
    }
}

void SemanticValidator::validate_reg_ref_in_body(
    const RegRef& ref, const std::set<std::string>& formal_qubits) {
    // Inside a gate_def body, only formal qubit parameter names are legal.
    if (!formal_qubits.count(ref.name)) {
        throw qplayer::QASMParseError{
            file_, ref.line, ref.col,
            "gate body references unknown name '" + ref.name +
                "': only formal qubit parameters are allowed inside a gate body"};
    }
    // A formal qubit name inside a gate body must NOT carry a concrete index
    // (e.g. `h q[0]` inside a gate body is illegal — use just `h q`).
    if (ref.index.has_value()) {
        throw qplayer::QASMParseError{
            file_, ref.line, ref.col,
            "formal qubit '" + ref.name +
                "' inside a gate body must not use an index subscript"};
    }
}

// ── Gate call validation ──────────────────────────────────────────────────────

void SemanticValidator::validate_gate_call_toplevel(const GateCall& call) {
    const GateSignature* sig = lookup_gate(call.name);
    if (!sig) {
        throw qplayer::QASMParseError{
            file_, call.line, call.col,
            "unknown gate '" + call.name +
                "': not declared and not in qelib1 standard library"};
    }

    // Parameter count check.
    const int np = static_cast<int>(call.params.size());
    if (np != sig->num_params) {
        std::ostringstream oss;
        oss << "gate '" << call.name << "' expects " << sig->num_params
            << " parameter(s) but got " << np;
        throw qplayer::QASMParseError{file_, call.line, call.col, oss.str()};
    }

    // Qubit argument count check.
    const int nq = static_cast<int>(call.qargs.size());
    if (nq != sig->num_qubits) {
        std::ostringstream oss;
        oss << "gate '" << call.name << "' expects " << sig->num_qubits
            << " qubit argument(s) but got " << nq;
        throw qplayer::QASMParseError{file_, call.line, call.col, oss.str()};
    }

    // Validate each qubit argument. Whole-register references (no index) are
    // allowed here: they trigger broadcast semantics in CircuitBuilder (OpenQASM
    // v2 — gate applied to every qubit in the register).
    for (const auto& qarg : call.qargs) {
        validate_reg_ref_toplevel(qarg, /*is_qubit=*/true, /*allow_whole=*/true);
    }

    // Broadcast size consistency check: all whole-register arguments must refer
    // to registers of the same size. Mixed sizes are rejected with an error.
    int broadcast_size = -1;
    std::string first_whole_name;
    for (const auto& qarg : call.qargs) {
        if (!qarg.index.has_value()) {
            auto it = qregs_.find(qarg.name);
            if (it == qregs_.end()) continue;  // already caught above
            const int reg_size = it->second;
            if (broadcast_size < 0) {
                broadcast_size    = reg_size;
                first_whole_name  = qarg.name;
            } else if (reg_size != broadcast_size) {
                std::ostringstream oss;
                oss << "broadcast size mismatch: register '" << first_whole_name
                    << "' (size " << broadcast_size << ") vs '" << qarg.name
                    << "' (size " << reg_size << ")";
                throw qplayer::QASMParseError{file_, qarg.line, qarg.col, oss.str()};
            }
        }
    }
}

void SemanticValidator::validate_gate_call_in_body(
    const GateCall& call, const std::set<std::string>& formal_qubits,
    const std::set<std::string>& formal_params) {
    // Gate called from inside a gate_def body must itself be a known gate.
    const GateSignature* sig = lookup_gate(call.name);
    if (!sig) {
        throw qplayer::QASMParseError{
            file_, call.line, call.col,
            "unknown gate '" + call.name +
                "' referenced inside gate body"};
    }

    // Parameter count: expressions inside a gate body may reference formal
    // parameter names (IdentExpr). We only check the count here; expression
    // type validation is responsibility.
    (void)formal_params;  // checked at count level only
    const int np = static_cast<int>(call.params.size());
    if (np != sig->num_params) {
        std::ostringstream oss;
        oss << "gate '" << call.name << "' inside gate body: expects "
            << sig->num_params << " parameter(s) but got " << np;
        throw qplayer::QASMParseError{file_, call.line, call.col, oss.str()};
    }

    // Qubit argument count.
    const int nq = static_cast<int>(call.qargs.size());
    if (nq != sig->num_qubits) {
        std::ostringstream oss;
        oss << "gate '" << call.name << "' inside gate body: expects "
            << sig->num_qubits << " qubit argument(s) but got " << nq;
        throw qplayer::QASMParseError{file_, call.line, call.col, oss.str()};
    }

    // Validate each qubit arg against the formal qubit parameter set.
    for (const auto& qarg : call.qargs) {
        validate_reg_ref_in_body(qarg, formal_qubits);
    }
}

// ── Statement validators ──────────────────────────────────────────────────────

void SemanticValidator::validate_measure(const MeasureStmt& stmt) {
    // Whole-register measure (e.g. "measure q -> c;") is legal in OpenQASM v2
    // : shorthand for measure q[i] -> c[i] for each i in [0, size).
    validate_reg_ref_toplevel(stmt.qubit, /*is_qubit=*/true,  /*allow_whole=*/true);
    validate_reg_ref_toplevel(stmt.cbit,  /*is_qubit=*/false, /*allow_whole=*/true);

    // When both sides are whole-register, they must have matching sizes.
    if (!stmt.qubit.index.has_value() && !stmt.cbit.index.has_value()) {
        auto qit = qregs_.find(stmt.qubit.name);
        auto cit = cregs_.find(stmt.cbit.name);
        if (qit != qregs_.end() && cit != cregs_.end()) {
            if (qit->second != cit->second) {
                std::ostringstream oss;
                oss << "measure broadcast size mismatch: qreg '" << stmt.qubit.name
                    << "' (size " << qit->second << ") vs creg '"
                    << stmt.cbit.name << "' (size " << cit->second << ")";
                throw qplayer::QASMParseError{
                    file_, stmt.qubit.line, stmt.qubit.col, oss.str()};
            }
        }
    }
}

void SemanticValidator::validate_reset(const ResetStmt& stmt) {
    validate_reg_ref_toplevel(stmt.qubit, /*is_qubit=*/true);
}

void SemanticValidator::validate_barrier(const BarrierStmt& stmt) {
    // barrier with no arguments is legal in QASM v2 (applies to all qubits).
    for (const auto& qarg : stmt.qargs) {
        // Barrier allows whole-register references (no index).
        validate_reg_ref_toplevel(qarg, /*is_qubit=*/true, /*allow_whole=*/true);
    }
}

void SemanticValidator::validate_if(const IfStmt& stmt) {
    // The condition register must be a creg, not a qreg.
    if (qregs_.count(stmt.creg_name) && !cregs_.count(stmt.creg_name)) {
        throw qplayer::QASMParseError{
            file_, stmt.line, stmt.col,
            "if-condition register '" + stmt.creg_name +
                "' is a qreg; only creg is allowed as if condition"};
    }
    // The register must be declared.
    auto it = cregs_.find(stmt.creg_name);
    if (it == cregs_.end()) {
        throw qplayer::QASMParseError{
            file_, stmt.line, stmt.col,
            "if-condition register '" + stmt.creg_name +
                "' is not declared"};
    }

    // Value range: 0 ≤ N < 2^width. Width ≤ 30 guard: 2^30 fits in int32; for
    // larger widths we promote to uint64 to avoid signed overflow.
    const int width = it->second;
    if (width <= 0) {
        throw qplayer::QASMParseError{
            file_, stmt.line, stmt.col,
            "if-condition register '" + stmt.creg_name +
                "' has invalid width " + std::to_string(width)};
    }
    // Maximum legal value: 2^width - 1.  Use uint64 to handle width up to 63.
    // OpenQASM v2 IfOp::value is uint64_t, so we mirror
    // that limit here.  Width > 63 would overflow uint64; in practice QASM v2
    // creg widths are small, so we cap at 63 defensively.
    const int    capped_width = (width > 63) ? 63 : width;
    const uint64_t max_val =
        (capped_width == 63)
            ? UINT64_MAX
            : ((uint64_t{1} << capped_width) - 1);

    if (stmt.value < 0) {
        throw qplayer::QASMParseError{
            file_, stmt.line, stmt.col,
            "if-condition value must be non-negative"};
    }
    const auto uval = static_cast<uint64_t>(stmt.value);
    if (uval > max_val) {
        std::ostringstream oss;
        oss << "if-condition value " << stmt.value
            << " exceeds maximum " << max_val
            << " for creg '" << stmt.creg_name
            << "' of width " << width;
        throw qplayer::QASMParseError{file_, stmt.line, stmt.col, oss.str()};
    }

    // Validate the body statement (GateCall, MeasureStmt, or ResetStmt are the
    // only legal if-bodies in QASM v2; the AstParser already enforces this).
    if (stmt.body) {
        validate_statement(*stmt.body);
    }
}

void SemanticValidator::validate_gate_def_body(const GateDef& def) {
    // Build sets of formal names for quick membership tests.
    std::set<std::string> fqubits(def.qargs.begin(), def.qargs.end());
    std::set<std::string> fparams(def.params.begin(), def.params.end());

    for (const auto& call : def.body) {
        validate_gate_call_in_body(call, fqubits, fparams);
    }
}

}  // namespace qplayer::detail
