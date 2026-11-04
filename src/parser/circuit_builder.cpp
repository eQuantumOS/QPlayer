/// @file circuit_builder.cpp
/// @brief AST → Circuit IR transformation.
///
/// Implements CircuitBuilder::build() which converts a validated Program AST
/// into a qplayer::Circuit object by executing five sequential passes:
/// Pass 1: QRegDecl  → qubit offset table, total qubit count.
/// Pass 2: CRegDecl  → classical register creation + offset table.
/// Pass 3: OpaqueDecl → Circuit::declare_opaque() (DeclarationOnly).
/// Pass 4: GateDef   → user_gates_ cache (body emitted at call sites).
/// Pass 5: Statements → Circuit op emission (GateCall / Measure / Reset /
/// Barrier / If / IncludeStmt / VersionDecl skipped).
///
/// Expression evaluation supports: pi, sin, cos, tan, exp, ln, sqrt, +, -, *,
/// /, ^ (pow), unary -. Unknown identifiers / functions throw QASMParseError.
///
/// if-body restriction: only GateCall is supported as an if-then body in Phase
/// 3 (Circuit::if_then accepts GateOp only). Measure/Reset as if-body throws
/// CircuitError. Lifting this restriction is an open item; the grammar
/// itself allows the full set.

#include "parser/detail/circuit_builder.hpp"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "error.hpp"
#include "log.hpp"
#include "parser/detail/qelib1_builtin.hpp"

// GCC 13 false-positive -Wmaybe-uninitialized triggered by variant push_back
// patterns inside circuit.cpp (same suppression rationale as circuit.cpp).
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace qplayer::detail {

// ── Construction ──────────────────────────────────────────────────────────────

CircuitBuilder::CircuitBuilder(std::string file_name)
    : file_(std::move(file_name)) {}

// ── Helper: throw QASMParseError at a source location ────────────────────────

namespace {
[[noreturn]] void parse_error(const std::string& file, int line, int col,
                               const std::string& msg) {
    throw qplayer::QASMParseError{file, line, col, msg};
}
}  // namespace

// ── Register resolution ───────────────────────────────────────────────────────

QubitIndex CircuitBuilder::resolve_qubit(const RegRef& ref) const {
    auto it = qreg_by_name_.find(ref.name);
    if (it == qreg_by_name_.end()) {
        parse_error(file_, ref.line, ref.col,
                    "unknown qreg '" + ref.name + "'");
    }
    const QregInfo* qi = it->second;
    if (!ref.index.has_value()) {
        // Whole-register reference is only valid in barrier; emitters that need
        // a single qubit should always have an index. Guard defensively.
        parse_error(file_, ref.line, ref.col,
                    "qreg '" + ref.name + "' used without index in scalar context");
    }
    const int idx = *ref.index;
    if (idx < 0 || static_cast<QubitIndex>(idx) >= qi->size) {
        parse_error(file_, ref.line, ref.col,
                    "qreg '" + ref.name + "' index " + std::to_string(idx) +
                    " out of range [0," + std::to_string(qi->size) + ")");
    }
    return qi->offset + static_cast<QubitIndex>(idx);
}

ClassicalBitIndex CircuitBuilder::resolve_cbit(const RegRef& ref) const {
    auto it = creg_by_name_.find(ref.name);
    if (it == creg_by_name_.end()) {
        parse_error(file_, ref.line, ref.col,
                    "unknown creg '" + ref.name + "'");
    }
    const CregInfo* ci = it->second;
    if (!ref.index.has_value()) {
        parse_error(file_, ref.line, ref.col,
                    "creg '" + ref.name + "' used without index in scalar context");
    }
    const int idx = *ref.index;
    if (idx < 0 || static_cast<ClassicalBitIndex>(idx) >= ci->size) {
        parse_error(file_, ref.line, ref.col,
                    "creg '" + ref.name + "' index " + std::to_string(idx) +
                    " out of range [0," + std::to_string(ci->size) + ")");
    }
    return ci->offset + static_cast<ClassicalBitIndex>(idx);
}

ClassicalRegisterIndex CircuitBuilder::resolve_creg(const std::string& name) const {
    auto it = creg_idx_.find(name);
    if (it == creg_idx_.end()) {
        parse_error(file_, 0, 0, "unknown creg '" + name + "'");
    }
    return it->second;
}

// ── Expression evaluator ──────────────────────────────────────────────────────

double CircuitBuilder::eval_expr(
    const ExprNode& node,
    const std::unordered_map<std::string, double>& param_map,
    int default_line, int default_col) const
{
    return std::visit([&](const auto& v) -> double {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, NumberLit>) {
            try {
                return std::stod(v.text);
            } catch (const std::invalid_argument&) {
                parse_error(file_, v.line, v.col,
                            "invalid numeric literal: '" + v.text + "'");
            } catch (const std::out_of_range&) {
                parse_error(file_, v.line, v.col,
                            "numeric literal out of double range: '" + v.text + "'");
            }
        }

        else if constexpr (std::is_same_v<T, IdentExpr>) {
            // Check parameter substitution first (gate formal parameters).
            auto pit = param_map.find(v.name);
            if (pit != param_map.end()) return pit->second;
            // Known symbolic constant.
            if (v.name == "pi") return M_PI;
            parse_error(file_, v.line, v.col,
                        "unknown expression name: '" + v.name + "'");
        }

        else if constexpr (std::is_same_v<T, UnaryNegExpr>) {
            return -eval_expr(*v.operand, param_map, default_line, default_col);
        }

        else if constexpr (std::is_same_v<T, BinaryExpr>) {
            double lhs = eval_expr(*v.left,  param_map, v.line, v.col);
            double rhs = eval_expr(*v.right, param_map, v.line, v.col);
            switch (v.op) {
                case BinaryOp::Add: return lhs + rhs;
                case BinaryOp::Sub: return lhs - rhs;
                case BinaryOp::Mul: return lhs * rhs;
                case BinaryOp::Div:
                    if (rhs == 0.0) {
                        parse_error(file_, v.line, v.col,
                                    "division by zero in parameter expression");
                    }
                    return lhs / rhs;
                case BinaryOp::Pow: {
                    // Guard the two domain errors std::pow answers with NaN: a
                    // negative base with a fractional exponent, and 0 raised to a
                    // negative power. Without this the NaN flows into a gate
                    // parameter and only surfaces much later as a diverged state
                    // (audit #7 side finding).
                    if (lhs < 0.0 && rhs != std::floor(rhs)) {
                        parse_error(file_, v.line, v.col,
                                    "negative base with fractional exponent in "
                                    "parameter expression (result is not real)");
                    }
                    if (lhs == 0.0 && rhs < 0.0) {
                        parse_error(file_, v.line, v.col,
                                    "zero raised to a negative power in parameter "
                                    "expression");
                    }
                    return std::pow(lhs, rhs);
                }
            }
            // Unreachable; silence compiler warning.
            return 0.0;
        }

        else if constexpr (std::is_same_v<T, CallExpr>) {
            if (v.args.size() != 1) {
                parse_error(file_, v.line, v.col,
                            "function '" + v.name + "' requires exactly 1 argument "
                            "(got " + std::to_string(v.args.size()) + ")");
            }
            double arg = eval_expr(*v.args[0], param_map, v.line, v.col);

            if (v.name == "sin")  return std::sin(arg);
            if (v.name == "cos")  return std::cos(arg);
            if (v.name == "tan")  return std::tan(arg);
            if (v.name == "exp")  return std::exp(arg);
            if (v.name == "ln") {
                if (arg <= 0.0) {
                    parse_error(file_, v.line, v.col,
                                "ln() argument must be positive (got " +
                                std::to_string(arg) + ")");
                }
                return std::log(arg);
            }
            if (v.name == "sqrt") {
                if (arg < 0.0) {
                    parse_error(file_, v.line, v.col,
                                "sqrt() argument must be non-negative (got " +
                                std::to_string(arg) + ")");
                }
                return std::sqrt(arg);
            }
            parse_error(file_, v.line, v.col,
                        "unknown function in expression: '" + v.name + "'");
        }

        // Unreachable; satisfy compiler.
        return 0.0;
    }, node.v);
}

// ── Built-in gate dispatch ────────────────────────────────────────────────────
// Maps a resolved (type, params, qubits) tuple to the appropriate Circuit
// builder method. Throws QASMParseError when param/qubit counts don't match
// what the type requires (should not happen after SemanticValidator, but
// defensive error messaging aids debugging).

void CircuitBuilder::emit_builtin_gate(
    qplayer::Circuit& c,
    GateType type,
    const std::vector<double>& params,
    const std::vector<QubitIndex>& qubits,
    const std::string& name,
    int line, int col)
{
    // Convenience lambdas for checked param / qubit access.
    auto p = [&](std::size_t i) -> double {
        if (i >= params.size()) {
            parse_error(file_, line, col,
                        "gate '" + name + "': expected param[" +
                        std::to_string(i) + "] but only " +
                        std::to_string(params.size()) + " params given");
        }
        return params[i];
    };
    auto q = [&](std::size_t i) -> QubitIndex {
        if (i >= qubits.size()) {
            parse_error(file_, line, col,
                        "gate '" + name + "': expected qubit[" +
                        std::to_string(i) + "] but only " +
                        std::to_string(qubits.size()) + " qubits given");
        }
        return qubits[i];
    };

    switch (type) {
        // 1Q Clifford+T
        case GateType::I:    c.id (q(0));                           break;
        case GateType::H:    c.h  (q(0));                           break;
        case GateType::X:    c.x  (q(0));                           break;
        case GateType::Y:    c.y  (q(0));                           break;
        case GateType::Z:    c.z  (q(0));                           break;
        case GateType::S:    c.s  (q(0));                           break;
        case GateType::Sdg:  c.sdg(q(0));                           break;
        case GateType::T:    c.t  (q(0));                           break;
        case GateType::Tdg:  c.tdg(q(0));                           break;
        case GateType::SX:   c.sx  (q(0));                          break;
        case GateType::SXdg: c.sxdg(q(0));                          break;
        // 1Q rotations / U gates
        case GateType::RX:   c.rx(p(0), q(0));                      break;
        case GateType::RY:   c.ry(p(0), q(0));                      break;
        case GateType::RZ:   c.rz(p(0), q(0));                      break;
        case GateType::U1:   c.u1(p(0), q(0));                      break;
        case GateType::U2:   c.u2(p(0), p(1), q(0));                break;
        case GateType::U3:   c.u3(p(0), p(1), p(2), q(0));          break;
        // 2Q gates
        case GateType::CX:   c.cx  (q(0), q(1));                    break;
        case GateType::CY:   c.cy  (q(0), q(1));                    break;
        case GateType::CZ:   c.cz  (q(0), q(1));                    break;
        case GateType::CH:   c.ch  (q(0), q(1));                    break;
        case GateType::SWAP: c.swap(q(0), q(1));                    break;
        case GateType::CRZ:  c.crz (p(0), q(0), q(1));              break;
        case GateType::CU1:  c.cu1 (p(0), q(0), q(1));              break;
        case GateType::CU3:  c.cu3 (p(0), p(1), p(2), q(0), q(1)); break;
        // ECR — native 2Q basis gate (never from QASM, but valid in Circuit IR replay).
        case GateType::ECR:       c.ecr       (q(0), q(1));              break;
        // Google native 2Q basis gates (never from QASM, valid in Circuit IR replay).
        case GateType::ISWAP:     c.iswap     (q(0), q(1));              break;
        case GateType::SQRT_ISWAP: c.sqrt_iswap(q(0), q(1));            break;
        case GateType::FSIM:      c.fsim      (p(0), p(1), q(0), q(1)); break;
        // 3Q gates
        case GateType::CCX:   c.ccx  (q(0), q(1), q(2));            break;
        case GateType::CSwap: c.cswap(q(0), q(1), q(2));            break;
        // Reset is emitted as GateOp{Reset} inside Circuit::reset().
        case GateType::Reset: c.reset(q(0));                         break;
        case GateType::Opaque:
            // Opaque calls should never reach this path; they are handled
            // separately in emit_gate_call before this dispatch is invoked.
            parse_error(file_, line, col,
                        "internal error: emit_builtin_gate called for Opaque type");
    }
}

// ── GateOp builder for if-then body ──────────────────────────────────────────

qplayer::GateOp CircuitBuilder::build_if_body_gate(
    const GateCall& call,
    const std::unordered_map<std::string, QubitIndex>& qarg_map,
    const std::unordered_map<std::string, double>& param_map)
{
    // Resolve parameters.
    std::vector<double> params;
    params.reserve(call.params.size());
    for (const auto& ep : call.params) {
        params.push_back(eval_expr(*ep, param_map, call.line, call.col));
    }

    // Resolve qubits.
    std::vector<QubitIndex> qubits;
    qubits.reserve(call.qargs.size());
    for (const auto& ref : call.qargs) {
        // Inside a gate body qarg_map is populated; at top level it is empty.
        if (!qarg_map.empty()) {
            auto it = qarg_map.find(ref.name);
            if (it != qarg_map.end()) {
                qubits.push_back(it->second);
                continue;
            }
        }
        qubits.push_back(resolve_qubit(ref));
    }

    // Check for opaque.
    if (opaques_.count(call.name)) {
        const OpaqueDecl* od = opaques_.at(call.name);
        GateOp g;
        g.type        = GateType::Opaque;
        g.name        = call.name;
        g.opaque_name = call.name;
        g.qubits      = qubits;
        g.params      = params;
        g.is_opaque   = true;
        (void)od;
        return g;
    }

    // Check for user gate inline in if-body — not supported at Phase 3.
    if (user_gates_.count(call.name)) {
        throw qplayer::CircuitError{
            "if(...) body contains a user-defined gate '" + call.name +
            "'; only built-in gates are supported inside if-then in Phase 3"};
    }

    // Built-in gate.
    auto opt_type = find_qelib1_gate_type(call.name);
    if (!opt_type.has_value()) {
        parse_error(file_, call.line, call.col,
                    "unknown gate in if-body: '" + call.name + "'");
    }
    GateType type = *opt_type;

    GateOp g;
    g.type   = type;
    g.name   = call.name;
    g.qubits = qubits;
    g.params = params;
    return g;
}

// ── Gate call emitter ─────────────────────────────────────────────────────────

void CircuitBuilder::emit_gate_call(
    qplayer::Circuit& c,
    const GateCall& call,
    const std::unordered_map<std::string, QubitIndex>& qarg_map,
    const std::unordered_map<std::string, double>& param_map,
    int inline_depth)
{
    // Broadcast expansion (OpenQASM v2): at top-level (empty qarg_map),
    // a whole-register qarg (no index) causes the gate to be applied once per
    // qubit in that register. Multiple whole-register args must share the same
    // size (guaranteed by SemanticValidator). Scalar args repeat unchanged.
    // Inside a gate body (qarg_map non-empty) all refs are formal scalars.
    if (qarg_map.empty()) {
        // Determine broadcast count N: size of the first whole-register arg.
        int broadcast_n = 0;
        for (const auto& ref : call.qargs) {
            if (!ref.index.has_value()) {
                auto it = qreg_by_name_.find(ref.name);
                if (it != qreg_by_name_.end()) {
                    broadcast_n = static_cast<int>(it->second->size);
                    break;
                }
            }
        }
        if (broadcast_n >= 1) {
            // Emit N scalar calls by replacing whole-register refs with ref[i].
            for (int i = 0; i < broadcast_n; ++i) {
                GateCall scalar_call;
                scalar_call.name   = call.name;
                scalar_call.params = call.params;
                scalar_call.line   = call.line;
                scalar_call.col    = call.col;
                scalar_call.qargs.reserve(call.qargs.size());
                for (const auto& ref : call.qargs) {
                    if (!ref.index.has_value()) {
                        // Whole-register: substitute index i.
                        RegRef r;
                        r.name  = ref.name;
                        r.index = i;
                        r.line  = ref.line;
                        r.col   = ref.col;
                        scalar_call.qargs.push_back(r);
                    } else {
                        // Scalar (indexed) ref: unchanged across all iterations.
                        scalar_call.qargs.push_back(ref);
                    }
                }
                // Recursive call with all refs now indexed → broadcast_n == 0
                // for the recursive invocation, so no infinite recursion.
                emit_gate_call(c, scalar_call, qarg_map, param_map, inline_depth);
            }
            return;
        }
        // broadcast_n == 0: no whole-register refs — fall through to scalar path.
        // broadcast_n == 1: single-element register behaves like a scalar call.
    }

    // Resolve actual parameter values.
    std::vector<double> params;
    params.reserve(call.params.size());
    for (const auto& ep : call.params) {
        params.push_back(eval_expr(*ep, param_map, call.line, call.col));
    }

    // Resolve actual qubit arguments.
    std::vector<QubitIndex> qubits;
    qubits.reserve(call.qargs.size());
    for (const auto& ref : call.qargs) {
        // Inside a gate body formal qubit names are in qarg_map.
        if (!qarg_map.empty()) {
            auto it = qarg_map.find(ref.name);
            if (it != qarg_map.end()) {
                qubits.push_back(it->second);
                continue;
            }
        }
        qubits.push_back(resolve_qubit(ref));
    }

    // 1. User-defined gate → inline expansion.
    {
        auto it = user_gates_.find(call.name);
        if (it != user_gates_.end()) {
            inline_user_gate(c, call, *it->second, qarg_map, param_map,
                             inline_depth);
            return;
        }
    }

    // 2. Opaque gate -> emit GateOp{Opaque}. Circuit exposes no public opaque-call
    // builder and ops_ is private, so we append through the free friend function
    // detail_circuit_push_op() (declared at the bottom of circuit.hpp, defined in
    // circuit.cpp). That keeps the public API surface unchanged — the alternative,
    // rebuilding the Circuit from a buffered op vector, would cost a full copy per
    // opaque call.
    {
        auto it = opaques_.find(call.name);
        if (it != opaques_.end()) {
            GateOp g;
            g.type        = GateType::Opaque;
            g.name        = call.name;
            g.opaque_name = call.name;
            g.qubits      = qubits;
            g.params      = params;
            g.is_opaque   = true;
            // Push via the bridge function (declared in circuit.hpp).
            qplayer::detail_circuit_push_op(c, qplayer::Op{std::move(g)});
            return;
        }
    }

    // 3. Built-in (qelib1) gate.
    auto opt_type = find_qelib1_gate_type(call.name);
    if (!opt_type.has_value()) {
        parse_error(file_, call.line, call.col,
                    "unknown gate '" + call.name + "'");
    }
    emit_builtin_gate(c, *opt_type, params, qubits, call.name,
                      call.line, call.col);
}

// ── User gate inline expansion ────────────────────────────────────────────────

void CircuitBuilder::inline_user_gate(
    qplayer::Circuit& c,
    const GateCall& call,
    const GateDef& def,
    const std::unordered_map<std::string, QubitIndex>& outer_qarg_map,
    const std::unordered_map<std::string, double>& outer_param_map,
    int inline_depth)
{
    if (inline_depth >= kMaxInlineDepth) {
        parse_error(file_, call.line, call.col,
                    "gate '" + call.name + "': inline depth limit (" +
                    std::to_string(kMaxInlineDepth) + ") exceeded; "
                    "possible recursive gate definition");
    }

    // Build formal → actual qubit map.
    std::unordered_map<std::string, QubitIndex> qarg_map;
    if (def.qargs.size() != call.qargs.size()) {
        parse_error(file_, call.line, call.col,
                    "gate '" + call.name + "': formal qubit count " +
                    std::to_string(def.qargs.size()) + " != actual " +
                    std::to_string(call.qargs.size()));
    }
    for (std::size_t i = 0; i < def.qargs.size(); ++i) {
        const RegRef& ref = call.qargs[i];
        QubitIndex actual;
        // Resolve through outer map first (we may be inside a nested inline).
        if (!outer_qarg_map.empty()) {
            auto it = outer_qarg_map.find(ref.name);
            if (it != outer_qarg_map.end()) {
                actual = it->second;
                qarg_map[def.qargs[i]] = actual;
                continue;
            }
        }
        actual = resolve_qubit(ref);
        qarg_map[def.qargs[i]] = actual;
    }

    // Build formal → actual parameter map.
    std::unordered_map<std::string, double> param_map;
    if (def.params.size() != call.params.size()) {
        parse_error(file_, call.line, call.col,
                    "gate '" + call.name + "': formal param count " +
                    std::to_string(def.params.size()) + " != actual " +
                    std::to_string(call.params.size()));
    }
    for (std::size_t i = 0; i < def.params.size(); ++i) {
        double val = eval_expr(*call.params[i], outer_param_map,
                               call.line, call.col);
        param_map[def.params[i]] = val;
    }

    // Emit each body statement with the new binding maps.
    for (const GateCall& body_call : def.body) {
        emit_gate_call(c, body_call, qarg_map, param_map,
                       inline_depth + 1);
    }
}

// ── Statement emitter ─────────────────────────────────────────────────────────

void CircuitBuilder::emit_statement(
    qplayer::Circuit& c,
    const AstStmt& stmt,
    int inline_depth)
{
    std::visit([&](const auto& s) {
        using T = std::decay_t<decltype(s)>;

        // Pass-1/2/3/4 handled nodes — skip in emit pass.
        if constexpr (std::is_same_v<T, VersionDecl>) { /* no-op */ }
        else if constexpr (std::is_same_v<T, QRegDecl>)  { /* no-op */ }
        else if constexpr (std::is_same_v<T, CRegDecl>)  { /* no-op */ }
        else if constexpr (std::is_same_v<T, GateDef>)   { /* no-op */ }
        else if constexpr (std::is_same_v<T, OpaqueDecl>) { /* no-op */ }

        else if constexpr (std::is_same_v<T, IncludeStmt>) {
            // qelib1.inc is built-in — treated as a no-op.
            // Any other include is an external file, which is out of scope for
            // Phase 3. Throw a clear parse error so users see a
            // descriptive message rather than a gate-not-found failure later.
            if (!is_qelib1_inc(s.path)) {
                parse_error(file_, s.line, s.col,
                            "external include \"" + s.path + "\" is not supported "
                            "in Phase 3; only \"qelib1.inc\" is built-in");
            }
        }

        else if constexpr (std::is_same_v<T, GateCall>) {
            static const std::unordered_map<std::string, QubitIndex> empty_qmap;
            static const std::unordered_map<std::string, double>     empty_pmap;
            emit_gate_call(c, s, empty_qmap, empty_pmap, inline_depth);
        }

        else if constexpr (std::is_same_v<T, MeasureStmt>) {
            // Whole-register measure: "measure q -> c;" expands to
            // measure q[i] -> c[i] for i in [0, size). Both must be whole-register
            // refs (SemanticValidator enforces matching sizes) or both indexed.
            const bool q_whole = !s.qubit.index.has_value();
            const bool c_whole = !s.cbit.index.has_value();
            if (q_whole || c_whole) {
                // Determine broadcast size from whichever side is whole-register.
                int bcast_n = 0;
                if (q_whole) {
                    auto it = qreg_by_name_.find(s.qubit.name);
                    if (it != qreg_by_name_.end())
                        bcast_n = static_cast<int>(it->second->size);
                } else {
                    auto it = creg_by_name_.find(s.cbit.name);
                    if (it != creg_by_name_.end())
                        bcast_n = static_cast<int>(it->second->size);
                }
                for (int i = 0; i < bcast_n; ++i) {
                    RegRef qref = s.qubit;
                    if (q_whole) qref.index = i;
                    RegRef cref = s.cbit;
                    if (c_whole) cref.index = i;
                    c.measure(resolve_qubit(qref), resolve_cbit(cref));
                }
            } else {
                c.measure(resolve_qubit(s.qubit), resolve_cbit(s.cbit));
            }
        }

        else if constexpr (std::is_same_v<T, ResetStmt>) {
            QubitIndex q = resolve_qubit(s.qubit);
            c.reset(q);
        }

        else if constexpr (std::is_same_v<T, BarrierStmt>) {
            if (s.qargs.empty()) {
                // Empty barrier → all qubits (QASM v2 spec).
                c.barrier();
            } else {
                std::vector<QubitIndex> qs;
                qs.reserve(s.qargs.size());
                // Collect all global qubit indices; whole-register refs expand
                // to every qubit in that register.
                for (const RegRef& ref : s.qargs) {
                    if (ref.index.has_value()) {
                        qs.push_back(resolve_qubit(ref));
                    } else {
                        // Whole-register reference: expand to all qubits.
                        auto it = qreg_by_name_.find(ref.name);
                        if (it == qreg_by_name_.end()) {
                            parse_error(file_, ref.line, ref.col,
                                        "unknown qreg '" + ref.name + "'");
                        }
                        const QregInfo* qi = it->second;
                        for (QubitIndex k = 0; k < qi->size; ++k) {
                            qs.push_back(qi->offset + k);
                        }
                    }
                }
                // Build initializer_list equivalent via a range loop on a
                // temporary; Circuit::barrier takes initializer_list<QubitIndex>
                // so we call it with a lambda workaround.
                // Since C++ has no vector → initializer_list cast, we use the
                // BarrierOp directly via detail_circuit_push_op.
                qplayer::BarrierOp b;
                b.qubits = qs;
                qplayer::detail_circuit_push_op(c, qplayer::Op{std::move(b)});
            }
        }

        else if constexpr (std::is_same_v<T, IfStmt>) {
            ClassicalRegisterIndex creg_idx = resolve_creg(s.creg_name);

            // The if body must be a GateCall (Phase 3 constraint).
            const AstStmt& body_stmt = *s.body;
            if (!std::holds_alternative<GateCall>(body_stmt.v)) {
                // Measure/Reset as if-body: unsupported in Phase 3.
                // Name the actual body kind instead of labelling everything that
                // is not a measure as "reset" (audit #23).
                const char* body_kind = "statement";
                if (std::holds_alternative<MeasureStmt>(body_stmt.v))    body_kind = "measure";
                else if (std::holds_alternative<ResetStmt>(body_stmt.v)) body_kind = "reset";
                else if (std::holds_alternative<BarrierStmt>(body_stmt.v)) body_kind = "barrier";
                else if (std::holds_alternative<IfStmt>(body_stmt.v))    body_kind = "if";
                throw qplayer::CircuitError{
                    std::string{"if(...) "} + body_kind +
                    " is not supported by the Circuit API in Phase 3 "
                    "(if_then() only accepts GateOp body)"};
            }
            const GateCall& body_call = std::get<GateCall>(body_stmt.v);

            static const std::unordered_map<std::string, QubitIndex> empty_qmap;
            static const std::unordered_map<std::string, double>     empty_pmap;
            GateOp body_op = build_if_body_gate(body_call, empty_qmap, empty_pmap);

            // No width check here: SemanticValidatorPass already rejects an
            // if-condition value the register cannot hold (semantic_validator.cpp
            // "exceeds maximum"), so this path is unreachable for QASM input. The
            // equivalent guard for the C++ API lives in Circuit::if_then().

            c.if_then(creg_idx, static_cast<std::uint64_t>(s.value), body_op);
        }

    }, stmt.v);
}

// ── Main build() method ───────────────────────────────────────────────────────

qplayer::Circuit CircuitBuilder::build(const Program& program) {
    // ── Pass 1: QRegDecl — build qubit offset table ───────────────────────────
    QubitIndex total_qubits = 0;
    for (const AstStmt& stmt : program.statements) {
        if (!std::holds_alternative<QRegDecl>(stmt.v)) continue;
        const QRegDecl& decl = std::get<QRegDecl>(stmt.v);
        if (decl.size <= 0) {
            parse_error(file_, decl.line, decl.col,
                        "qreg '" + decl.name + "' size must be positive (got " +
                        std::to_string(decl.size) + ")");
        }
        QregInfo qi;
        qi.name   = decl.name;
        qi.offset = total_qubits;
        qi.size   = static_cast<QubitIndex>(decl.size);
        total_qubits += qi.size;
        qregs_.push_back(std::move(qi));
    }
    // Build the name -> QregInfo* map only after qregs_ is fully populated:
    // taking &qregs_.back() inside the push loop dangles when a later push_back
    // reallocates the vector (manifested as size 0 for earlier multi-qreg decls).
    for (const QregInfo& qi : qregs_) {
        qreg_by_name_[qi.name] = &qi;
    }

    // ── Pass 2: CRegDecl — create Circuit + classical registers ──────────────
    // Circuit constructor with num_cubits=0 defers all classical register
    // creation to add_creg() calls below.
    qplayer::Circuit c{total_qubits, 0};

    ClassicalBitIndex creg_offset = 0;
    for (const AstStmt& stmt : program.statements) {
        if (!std::holds_alternative<CRegDecl>(stmt.v)) continue;
        const CRegDecl& decl = std::get<CRegDecl>(stmt.v);
        if (decl.size <= 0) {
            parse_error(file_, decl.line, decl.col,
                        "creg '" + decl.name + "' size must be positive (got " +
                        std::to_string(decl.size) + ")");
        }
        auto idx = c.add_creg(decl.name,
                               static_cast<ClassicalBitIndex>(decl.size));
        creg_idx_[decl.name] = idx;

        CregInfo ci;
        ci.name   = decl.name;
        ci.offset = creg_offset;
        ci.size   = static_cast<ClassicalBitIndex>(decl.size);
        creg_offset += ci.size;
        cregs_.push_back(std::move(ci));
    }
    // Same reallocation hazard as qregs_: build the name map after cregs_ is
    // fully populated so the stored pointers stay valid for multi-creg circuits.
    for (const CregInfo& ci : cregs_) {
        creg_by_name_[ci.name] = &ci;
    }

    // ── Pass 3: OpaqueDecl → Circuit::declare_opaque ──────────────
    for (const AstStmt& stmt : program.statements) {
        if (!std::holds_alternative<OpaqueDecl>(stmt.v)) continue;
        const OpaqueDecl& decl = std::get<OpaqueDecl>(stmt.v);
        c.declare_opaque(decl.name,
                         static_cast<QubitIndex>(decl.qargs.size()));
        opaques_[decl.name] = &decl;
    }

    // ── Pass 4: GateDef → user_gates_ cache ──────────────────────────────────
    // Gate bodies are not emitted here — they are inlined at each call site.
    for (const AstStmt& stmt : program.statements) {
        if (!std::holds_alternative<GateDef>(stmt.v)) continue;
        const GateDef& def = std::get<GateDef>(stmt.v);
        user_gates_[def.name] = &def;
    }

    // ── Pass 5: Executable statements → Circuit ops ───────────────────────────
    for (const AstStmt& stmt : program.statements) {
        emit_statement(c, stmt, 0);
    }

    return c;
}

}  // namespace qplayer::detail

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic pop
#endif
