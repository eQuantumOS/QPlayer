/// @file diagonal_phase_pass.cpp
/// @brief DiagonalPhasePass (Phase 4).
///
/// Collapses runs of same-qubit diagonal gates into a single gate by accumulating
/// their phases. Diagonal gates are of the form diag(1, e^{iφ}) on the |1> basis:
/// Z:    φ = π
/// S:    φ = π/2
/// Sdg:  φ = -π/2
/// T:    φ = π/4
/// Tdg:  φ = -π/4
/// RZ(θ): φ = θ  (note: RZ(θ) = diag(e^{-iθ/2}, e^{iθ/2}); relative phase = θ)
/// U1(λ): φ = λ  (U1(λ) = diag(1, e^{iλ}))
///
/// For RZ: the global phase e^{-iθ/2} is ignored; only the relative phase θ is
/// accumulated. This is consistent with the "global phase ignored" convention
/// throughout the F-OPT passes.
///
/// Net-phase → gate mapping (mod 2π):
/// 0                 → identity (op removed)
/// π/4  (45°)        → T
/// -π/4 (-45°)        → Tdg
/// π/2  (90°)        → S
/// -π/2 (-90°)        → Sdg
/// π   (180°)        → Z
/// other             → RZ(net_phase)
///
/// Tolerance for equality checks: 1e-10 radians.
/// Level 2/3 only — not auto-registered at Level 0/1.
///

#include "transpiler/passes/optimization/diagonal_phase_pass.hpp"

#include <cmath>
#include <vector>
#include <unordered_map>

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

constexpr double kPi       = 3.14159265358979323846;
constexpr double kTol      = 1e-10;    // phase equality tolerance (radians)
constexpr double kTwoPi    = 2.0 * kPi;

// Returns true when |a - b| <= tol (mod 2π).
bool phase_near(double a, double b, double tol = kTol) {
    double diff = std::fmod(std::abs(a - b), kTwoPi);
    if (diff > kPi) diff = kTwoPi - diff;
    return diff <= tol;
}

// Returns the relative phase φ carried by a diagonal gate, or nullopt when the
// gate is not a diagonal gate eligible for accumulation.
std::optional<double> diagonal_phase(const GateOp& g) {
    if (g.is_opaque) return std::nullopt;
    switch (g.type) {
        case GateType::Z:   return kPi;
        case GateType::S:   return kPi / 2.0;
        case GateType::Sdg: return -kPi / 2.0;
        case GateType::T:   return kPi / 4.0;
        case GateType::Tdg: return -kPi / 4.0;
        case GateType::RZ:
            // RZ(θ) contributes relative phase θ (global phase e^{-iθ/2} ignored).
            if (g.params.size() == 1) return g.params[0];
            return std::nullopt;
        case GateType::U1:
            // U1(λ) = diag(1, e^{iλ}) — relative phase is λ.
            if (g.params.size() == 1) return g.params[0];
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

// Returns true when op is a boundary (terminates the accumulation window).
bool is_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

// Maps a net accumulated phase (mod 2π, normalised to (-π, π]) to the best
// single-gate representation. Returns {type, params}.
// type == GateType::I means identity — the op should be removed.
struct PhaseGate {
    GateType           type;
    std::vector<double> params;
    const char*        name;
};

PhaseGate phase_to_gate(double phi) {
    // Normalise to (-π, π].
    phi = std::fmod(phi, kTwoPi);
    if (phi >  kPi) phi -= kTwoPi;
    if (phi <= -kPi) phi += kTwoPi;

    if (phase_near(phi, 0.0))        return {GateType::I,   {}, "id"};
    if (phase_near(phi,  kPi/4.0))   return {GateType::T,   {}, "t"};
    if (phase_near(phi, -kPi/4.0))   return {GateType::Tdg, {}, "tdg"};
    if (phase_near(phi,  kPi/2.0))   return {GateType::S,   {}, "s"};
    if (phase_near(phi, -kPi/2.0))   return {GateType::Sdg, {}, "sdg"};
    if (phase_near(phi,  kPi) ||
        phase_near(phi, -kPi))       return {GateType::Z,   {}, "z"};

    // General case: RZ(φ).
    return {GateType::RZ, {phi}, "rz"};
}

}  // anonymous namespace

std::string DiagonalPhasePass::name() const {
    return "DiagonalPhasePass";
}

// Algorithm:
// For each qubit maintain a run of consecutive diagonal-gate op indices. When
// a non-diagonal gate or boundary is encountered, flush the accumulated run:
//   - sum the phases
//   - convert to a single gate via phase_to_gate()
//   - if the merged gate differs from a single original op (or if there are
//     multiple originals), emit the single merged gate replacing all originals
// Flush results are collected as (surviving_index, new_gate) plus a set of
// indices to drop.
Circuit DiagonalPhasePass::run(Circuit circuit) {
    changed_ = false;

    const auto& old_ops = circuit.ops();
    const QubitIndex nq = circuit.num_qubits();

    // Per-qubit accumulation state.
    struct QubitAcc {
        std::vector<std::size_t> indices;  // positions in old_ops
        double                   sum_phase = 0.0;
    };
    std::vector<QubitAcc> acc(static_cast<std::size_t>(nq));

    struct MergeResult {
        std::size_t            first_idx;   // position of the surviving (replacement) op
        std::vector<std::size_t> drop_idx;  // positions to remove
        PhaseGate              gate;
        QubitIndex             qubit;
    };
    std::vector<MergeResult> results;

    // Flush the accumulator for qubit q; record a MergeResult if a simplification
    // occurred (more than one op in the run, or a single op that maps differently).
    auto flush = [&](QubitIndex q) {
        QubitAcc& a = acc[q];
        if (a.indices.empty()) return;

        if (a.indices.size() == 1) {
            // Single op — no merge benefit unless the phase reduces to identity
            // (e.g. RZ(0)). Check this.
            PhaseGate pg = phase_to_gate(a.sum_phase);
            const GateOp& orig = std::get<GateOp>(old_ops[a.indices[0]]);
            bool changed_single = false;
            if (pg.type == GateType::I) {
                // Collapsed to identity — remove.
                changed_single = true;
            } else if (pg.type != orig.type) {
                // Phase reduces to a different gate type.
                changed_single = true;
            } else if (pg.type == GateType::RZ || pg.type == GateType::U1) {
                // Same type but check if params differ by more than tolerance.
                if (orig.params.empty() || !phase_near(orig.params[0], a.sum_phase)) {
                    changed_single = true;
                }
            }
            if (changed_single) {
                MergeResult r;
                r.first_idx = a.indices[0];
                r.gate      = pg;
                r.qubit     = q;
                results.push_back(r);
            }
        } else {
            // Multiple ops — always merge.
            PhaseGate pg = phase_to_gate(a.sum_phase);
            MergeResult r;
            r.first_idx = a.indices[0];
            r.drop_idx  = std::vector<std::size_t>(a.indices.begin() + 1, a.indices.end());
            r.gate      = pg;
            r.qubit     = q;
            results.push_back(r);
        }

        a.indices.clear();
        a.sum_phase = 0.0;
    };

    // Pass 1: accumulate.
    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        const Op& op = old_ops[i];

        if (is_boundary(op)) {
            for (QubitIndex q = 0; q < nq; ++q) flush(q);
            continue;
        }

        if (!std::holds_alternative<GateOp>(op)) continue;

        const GateOp& g = std::get<GateOp>(op);

        // Multi-qubit gate: flush all its qubits.
        if (g.qubits.size() > 1) {
            for (QubitIndex q : g.qubits) {
                if (q < nq) flush(q);
            }
            continue;
        }

        if (g.qubits.empty()) continue;
        QubitIndex q = g.qubits[0];
        if (q >= nq) continue;

        auto phi = diagonal_phase(g);
        if (!phi.has_value()) {
            // Non-diagonal gate: flush this qubit's accumulator.
            flush(q);
            continue;
        }

        // Accumulate.
        acc[q].indices.push_back(i);
        acc[q].sum_phase += *phi;
    }

    // Flush remaining accumulators.
    for (QubitIndex q = 0; q < nq; ++q) flush(q);

    if (results.empty()) {
        return circuit;  // nothing to simplify
    }

    changed_ = true;

    // Build drop set and update map.
    std::unordered_map<std::size_t, bool>       drop_set;
    std::unordered_map<std::size_t, MergeResult*> update_map;
    for (auto& r : results) {
        if (r.gate.type == GateType::I) {
            // Identity — also drop the first index.
            drop_set[r.first_idx] = true;
        } else {
            update_map[r.first_idx] = &r;
        }
        for (std::size_t d : r.drop_idx) {
            drop_set[d] = true;
        }
    }

    std::vector<Op> new_ops;
    new_ops.reserve(old_ops.size());

    for (std::size_t i = 0; i < old_ops.size(); ++i) {
        if (drop_set.count(i)) continue;

        if (update_map.count(i)) {
            // Emit the merged gate.
            const MergeResult& r = *update_map[i];
            GateOp merged = std::get<GateOp>(old_ops[i]);  // copy for field reuse
            merged.type   = r.gate.type;
            merged.name   = r.gate.name;
            merged.params = r.gate.params;
            new_ops.push_back(Op{merged});
        } else {
            new_ops.push_back(old_ops[i]);
        }
    }

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(new_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool DiagonalPhasePass::changed() const {
    return changed_;
}

}  // namespace qplayer
