/// @file circuit_draw.cpp
/// @brief -- Circuit -> human-readable text diagram.
///
/// Implements draw_circuit_text(const Circuit&, CircuitDrawStyle) used by
/// Circuit::draw(). A sibling of qasm_emitter.cpp / serializer.cpp: walks ops()
/// only (no backend), so Circuit owns the logic (QRegister keeps a thin delegate).
///
/// Layout model (single line per qubit, Qiskit-text inspired):
/// * Each qubit q owns wire row 2*q; odd rows in between hold vertical
/// connectors for multi-qubit gates. A trailing classical wire row holds
/// measurement / conditional targets (only emitted when present).
/// * Ops are packed into columns ("layers") greedily: an op reserves the
/// contiguous qubit span it touches so connectors never collide.
/// * Single-qubit gates render as a boxed label [H] / [RX(1.5708)]; controlled
/// gates show a control marker on the control wire and a boxed target;
/// swap shows the swap marker on both wires; barriers show a dashed band.
///
/// The grid stores one glyph per cell as a std::string so multi-byte box-drawing
/// characters never break column alignment (display width == cell count).

#include "circuit/circuit.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace qplayer {

namespace {

// Per-style glyph table. Unicode uses box-drawing glyphs; Ascii stays portable.
struct Glyphs {
    std::string wire;     // horizontal qubit wire
    std::string conn;     // vertical connector (between wires)
    std::string cross;    // wire crossed by a vertical connector
    std::string ctrl;     // control marker
    std::string swap;     // swap endpoint marker
    std::string barrier;  // barrier band segment
    std::string cwire;    // classical wire
};

Glyphs glyphs_for(CircuitDrawStyle style) {
    if (style == CircuitDrawStyle::Ascii) {
        return Glyphs{"-", "|", "+", "*", "x", ":", "="};
    }
    return Glyphs{"─", "│", "┼", "■", "×", "░", "═"};
}

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Render rotation / phase parameters as "(p0, p1)". Default precision keeps the
// label compact -- the diagram is for human inspection, not round-trip fidelity
// (use to_qasm / to_json for that).
std::string format_params(const std::vector<double>& params) {
    if (params.empty()) return "";
    std::ostringstream out;
    out << '(';
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) out << ',';
        out << params[i];
    }
    out << ')';
    return out.str();
}

// Pad a label to an odd display width so its centre cell lands on the layer
// centre column -- this keeps boxed targets aligned with control connectors.
std::string make_odd(std::string s) {
    if (s.size() % 2 == 0) s.push_back(' ');
    return s;
}

// A glyph token = a sequence of single-cell strings.
using Token = std::vector<std::string>;

Token box(const std::string& label) {
    std::string l = make_odd(label);
    Token t;
    t.reserve(l.size() + 2);
    t.push_back("[");
    for (char c : l) t.emplace_back(1, c);
    t.push_back("]");
    return t;
}

Token glyph_token(const std::string& g) { return Token{g}; }

Token digits(const std::string& s) {
    std::string d = make_odd(s);
    Token t;
    t.reserve(d.size());
    for (char c : d) t.emplace_back(1, c);
    return t;
}

// Human-readable label for a single-qubit / target gate. Falls back to the
// canonical name when the type is not specially mapped.
std::string gate_label(const GateOp& g) {
    if (g.is_opaque || g.type == GateType::Opaque) {
        return g.opaque_name.empty() ? g.name : g.opaque_name;
    }
    if (g.type == GateType::Reset) return "|0>";
    std::string base;
    switch (g.type) {
        case GateType::CX:  base = "X";  break;   // controlled-target label
        case GateType::CCX: base = "X";  break;   // Toffoli target
        case GateType::CY:  base = "Y";  break;
        case GateType::CH:  base = "H";  break;
        case GateType::CRZ: base = "RZ"; break;
        case GateType::CU1: base = "U1"; break;
        case GateType::CU3: base = "U3"; break;
        default:            base = to_upper(g.name); break;
    }
    return base + format_params(g.params);
}

// One op's placement plan: which token sits on which qubit wire, the qubit span
// it occupies, whether a vertical connector links the span, an optional dashed
// barrier band, and an optional classical-wire annotation (measure / if).
struct Plan {
    std::vector<std::pair<QubitIndex, Token>> wire_tokens;  // qubit -> token
    QubitIndex lo = 0;
    QubitIndex hi = 0;
    bool       connect      = false;  // draw vertical line across [lo, hi]
    bool       is_barrier   = false;
    bool       to_classical = false;  // draw connector down to classical wire
    QubitIndex classical_from = 0;    // qubit the classical connector starts at
    Token      classical_token;       // annotation placed on the classical wire
};

void set_span(Plan& p, const std::vector<QubitIndex>& qs) {
    p.lo = *std::min_element(qs.begin(), qs.end());
    p.hi = *std::max_element(qs.begin(), qs.end());
}

// Build the placement plan for a GateOp (control/target roles by GateType).
Plan plan_gate(const GateOp& g, const Glyphs& gl) {
    Plan p;
    const auto& q = g.qubits;
    auto ctrl_tok = glyph_token(gl.ctrl);
    auto swap_tok = glyph_token(gl.swap);

    switch (g.type) {
        case GateType::CX:
        case GateType::CY:
        case GateType::CH:
        case GateType::CRZ:
        case GateType::CU1:
        case GateType::CU3:
            p.wire_tokens.push_back({q[0], ctrl_tok});
            p.wire_tokens.push_back({q[1], box(gate_label(g))});
            p.connect = true;
            break;
        case GateType::CZ:
            p.wire_tokens.push_back({q[0], ctrl_tok});
            p.wire_tokens.push_back({q[1], ctrl_tok});   // symmetric control
            p.connect = true;
            break;
        case GateType::SWAP:
            p.wire_tokens.push_back({q[0], swap_tok});
            p.wire_tokens.push_back({q[1], swap_tok});
            p.connect = true;
            break;
        case GateType::CCX:
            p.wire_tokens.push_back({q[0], ctrl_tok});
            p.wire_tokens.push_back({q[1], ctrl_tok});
            p.wire_tokens.push_back({q[2], box(gate_label(g))});
            p.connect = true;
            break;
        case GateType::CSwap:
            p.wire_tokens.push_back({q[0], ctrl_tok});
            p.wire_tokens.push_back({q[1], swap_tok});
            p.wire_tokens.push_back({q[2], swap_tok});
            p.connect = true;
            break;
        case GateType::ECR:   // symmetric-looking box on both wires
            p.wire_tokens.push_back({q[0], box("ECR")});
            p.wire_tokens.push_back({q[1], box("ECR")});
            p.connect = true;
            break;
        case GateType::ISWAP:  // Google native 2Q gate label
            p.wire_tokens.push_back({q[0], box("iSWAP")});
            p.wire_tokens.push_back({q[1], box("iSWAP")});
            p.connect = true;
            break;
        case GateType::SQRT_ISWAP:
            p.wire_tokens.push_back({q[0], box("sqrt_iSWAP")});
            p.wire_tokens.push_back({q[1], box("sqrt_iSWAP")});
            p.connect = true;
            break;
        case GateType::FSIM:
            p.wire_tokens.push_back({q[0], box("fSim")});
            p.wire_tokens.push_back({q[1], box("fSim")});
            p.connect = true;
            break;
        default: {
            // Single-qubit gate, reset, or multi-qubit opaque: box on each wire.
            Token tok = box(gate_label(g));
            for (QubitIndex qi : q) p.wire_tokens.push_back({qi, tok});
            p.connect = q.size() > 1;
            break;
        }
    }
    set_span(p, q);
    return p;
}

}  // namespace

std::string draw_circuit_text(const Circuit& c, CircuitDrawStyle style) {
    const Glyphs gl = glyphs_for(style);
    const QubitIndex n = c.num_qubits();
    if (n == 0) return "(empty circuit: 0 qubits)\n";

    // ── Plan every op + detect whether a classical wire is needed ───────────
    const auto& ops = c.ops();
    std::vector<Plan> plans;
    plans.reserve(ops.size());
    bool need_classical = false;

    for (const auto& op : ops) {
        Plan p;
        if (const auto* g = std::get_if<GateOp>(&op)) {
            p = plan_gate(*g, gl);
        } else if (const auto* m = std::get_if<MeasureOp>(&op)) {
            p.wire_tokens.push_back({m->qubit, box("M")});
            p.lo = p.hi = m->qubit;
            p.to_classical   = true;
            p.classical_from = m->qubit;
            p.classical_token = digits(std::to_string(m->cubit));
            need_classical = true;
        } else if (const auto* b = std::get_if<BarrierOp>(&op)) {
            std::vector<QubitIndex> qs = b->qubits;
            if (qs.empty()) { for (QubitIndex q = 0; q < n; ++q) qs.push_back(q); }
            for (QubitIndex q : qs) p.wire_tokens.push_back({q, glyph_token(gl.barrier)});
            set_span(p, qs);
            p.is_barrier = true;
            p.connect    = true;
        } else if (const auto* f = std::get_if<IfOp>(&op)) {
            // Render the conditioned body gate, then link it to the classical
            // wire with the compared value annotated below.
            Plan body = plan_gate(f->body, gl);
            p.wire_tokens = body.wire_tokens;
            p.lo = body.lo; p.hi = body.hi; p.connect = body.connect;
            p.to_classical   = true;
            p.classical_from = body.lo;
            p.classical_token = digits("=" + std::to_string(f->value));
            need_classical = true;
        } else {
            // IdleOp: no visual representation -- empty plan.
        }
        plans.push_back(std::move(p));
    }

    // ── Greedy layer assignment (reserve the touched qubit span) ────────────
    // Classical-touching ops reserve down to the last qubit so their vertical
    // drop to the classical wire never collides with another op's column.
    std::vector<int> next_free(n, 0);
    std::vector<int> layer_of(plans.size(), -1);
    int num_layers = 0;
    for (std::size_t i = 0; i < plans.size(); ++i) {
        const Plan& p = plans[i];
        if (p.wire_tokens.empty() && !p.to_classical) continue;  // IdleOp etc.
        QubitIndex lo = p.lo;
        QubitIndex hi = p.to_classical ? (n - 1) : p.hi;
        int layer = 0;
        for (QubitIndex q = lo; q <= hi; ++q) layer = std::max(layer, next_free[q]);
        layer_of[i] = layer;
        for (QubitIndex q = lo; q <= hi; ++q) next_free[q] = layer + 1;
        num_layers = std::max(num_layers, layer + 1);
    }

    // ── Layer widths (odd, sized to the widest token incl. classical anno) ──
    std::vector<int> width(num_layers, 1);
    for (std::size_t i = 0; i < plans.size(); ++i) {
        if (layer_of[i] < 0) continue;
        int& w = width[layer_of[i]];
        for (const auto& [q, tok] : plans[i].wire_tokens)
            w = std::max(w, static_cast<int>(tok.size()));
        if (plans[i].to_classical)
            w = std::max(w, static_cast<int>(plans[i].classical_token.size()));
    }
    for (int& w : width) if (w % 2 == 0) ++w;

    // ── Grid geometry ───────────────────────────────────────────────────────
    std::vector<std::string> labels(n);
    std::size_t gutter = 0;
    for (QubitIndex q = 0; q < n; ++q) {
        labels[q] = "q" + std::to_string(q) + ": ";
        gutter = std::max(gutter, labels[q].size());
    }
    std::string clabel = "c: ";
    if (need_classical) gutter = std::max(gutter, clabel.size());

    const int classical_row = need_classical ? static_cast<int>(2 * n) : -1;
    const int rows = need_classical ? static_cast<int>(2 * n + 1)
                                    : static_cast<int>(2 * n - 1);

    std::vector<int> col_start(num_layers, 0);
    int total_cols = static_cast<int>(gutter);
    for (int L = 0; L < num_layers; ++L) {
        col_start[L] = total_cols;
        total_cols += width[L] + 1;  // +1 inter-layer gap
    }

    // ── Initialise the glyph grid ───────────────────────────────────────────
    std::vector<std::vector<std::string>> grid(
        rows, std::vector<std::string>(total_cols, " "));

    auto write_label = [&](int row, const std::string& text) {
        for (std::size_t k = 0; k < text.size(); ++k) grid[row][k] = std::string(1, text[k]);
    };
    for (QubitIndex q = 0; q < n; ++q) {
        int r = static_cast<int>(2 * q);
        write_label(r, labels[q]);
        for (int col = static_cast<int>(gutter); col < total_cols; ++col) grid[r][col] = gl.wire;
    }
    if (need_classical) {
        write_label(classical_row, clabel);
        for (int col = static_cast<int>(gutter); col < total_cols; ++col)
            grid[classical_row][col] = gl.cwire;
    }

    auto center_at = [&](int L) { return col_start[L] + width[L] / 2; };
    auto put = [&](int row, int center, const Token& tok) {
        int start = center - static_cast<int>(tok.size()) / 2;
        for (std::size_t k = 0; k < tok.size(); ++k) grid[row][start + k] = tok[k];
    };

    // ── Place every planned op ──────────────────────────────────────────────
    for (std::size_t i = 0; i < plans.size(); ++i) {
        if (layer_of[i] < 0) continue;
        const Plan& p = plans[i];
        const int L = layer_of[i];
        const int C = center_at(L);

        std::vector<bool> on_wire(n, false);
        for (const auto& [q, tok] : p.wire_tokens) {
            put(static_cast<int>(2 * q), C, tok);
            on_wire[q] = true;
        }

        // Vertical connector across the gate's qubit span.
        if (p.connect && p.hi > p.lo) {
            for (int r = static_cast<int>(2 * p.lo) + 1; r < static_cast<int>(2 * p.hi); ++r) {
                if (r % 2 == 1) {                       // between-wire row
                    grid[r][C] = p.is_barrier ? gl.barrier : gl.conn;
                } else {                                // intermediate qubit wire
                    QubitIndex q = static_cast<QubitIndex>(r / 2);
                    if (p.is_barrier)      grid[r][C] = gl.barrier;
                    else if (!on_wire[q])  grid[r][C] = gl.cross;
                }
            }
        }

        // Drop to the classical wire (measure / conditional).
        if (p.to_classical && classical_row >= 0) {
            for (int r = static_cast<int>(2 * p.classical_from) + 1; r < classical_row; ++r) {
                if (r % 2 == 1) grid[r][C] = gl.conn;
                else            grid[r][C] = gl.cross;   // lower qubit wires crossed
            }
            put(classical_row, C, p.classical_token);
        }
    }

    // ── Join, trimming trailing whitespace per row ──────────────────────────
    std::ostringstream out;
    for (int r = 0; r < rows; ++r) {
        std::string line;
        for (const auto& cell : grid[r]) line += cell;
        std::size_t end = line.find_last_not_of(' ');
        if (end != std::string::npos) line.erase(end + 1);
        else                          line.clear();
        out << line << '\n';
    }
    return out.str();
}

}  // namespace qplayer
