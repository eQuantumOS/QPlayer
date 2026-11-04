// merge_single_qubit_unitary_pass.cpp — Full-unitary 1Q merge pass.
// Merges runs of consecutive same-qubit 1-qubit gates (including rotation gates
// RX/RY/RZ/U1/U2/U3 and all Clifford+T types) into a single U3(theta,phi,lambda).
//
// Algorithm per qubit:
//   1. Scan ops in order; accumulate a "run" of 1Q gates in a per-qubit window.
//   2. A run ends at a boundary: 2Q+ gate, Measure, Barrier, If, Idle, Opaque,
//      Reset.
//   3. When a run ends with length >= 2, compose the 2x2 complex unitary matrices
//      (right-to-left factor order: M = M_last * ... * M_first — each gate
//      applied to a ket on the right), then decompose M into U3(theta, phi,
//      lambda) via standard ZYZ / atan2 formula, discarding the global phase.
//      The original run ops are replaced with a single U3 GateOp.
//   4. Runs of length 1 are not modified.
//
// U3 decomposition (standard, global phase ignored — safe per):
//   U3(theta, phi, lambda) =
//     [[cos(theta/2),            -e^{i*lambda}*sin(theta/2)],
//      [e^{i*phi}*sin(theta/2),   e^{i*(phi+lambda)}*cos(theta/2)]]
//
//   Given unitary M = e^{i*gamma} * U3(theta, phi, lambda) we recover:
//     theta = 2 * atan2(|M[1][0]|, |M[0][0]|)    (from the magnitude ratio)
//     phi   = atan2(Im M[1][0], Re M[1][0]) - atan2(Im M[1][1]*conj(M[0][0]), Re M[1][1]*conj(M[0][0]))
//             simplified via the U3 column phase structure:
//     gamma = atan2(Im M[0][0], Re M[0][0])       (global phase of [0][0])
//     phi   = atan2(Im M[1][0], Re M[1][0]) - gamma
//     lambda= atan2(-Im M[0][1], -Re M[0][1]) - gamma
//
//   Identity check: |M - I| < 1e-12 (Frobenius) -> emit nothing (drop the run).
//   Tolerance for sin(theta/2) near zero or one avoids gimbal-lock in phi/lambda.
//
// extension.

#include "transpiler/passes/optimization/merge_single_qubit_unitary_pass.hpp"

#include <cassert>
#include <cmath>
#include <complex>
#include <unordered_map>
#include <vector>

#include "backend/full_state/gate_matrix.hpp"
#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {

namespace {

// Returns true when the GateOp is a 1-qubit gate that can be composed as a
// unitary (covers Clifford+T, SX/SXdg, and all rotation/U gates).
// Opaque, multi-qubit, and Reset are excluded.
bool is_composable_1q(const GateOp& g) {
    if (g.is_opaque) return false;
    if (g.qubits.size() != 1) return false;
    switch (g.type) {
        // Cannot derive a simple 2x2 matrix for Reset.
        case GateType::Reset:  return false;
        // Multi-qubit gate types (shouldn't appear with qubits.size()==1, but
        // guard defensively).
        case GateType::CX:
        case GateType::CY:
        case GateType::CZ:
        case GateType::CH:
        case GateType::SWAP:
        case GateType::CRZ:
        case GateType::CU1:
        case GateType::CU3:
        case GateType::CCX:
        case GateType::CSwap:  return false;
        // All other 1Q gate types are composable.
        default: return true;
    }
}

// Returns true when op is a merge-window boundary.
bool is_boundary(const Op& op) {
    return std::holds_alternative<MeasureOp>(op) ||
           std::holds_alternative<BarrierOp>(op) ||
           std::holds_alternative<IfOp>(op)      ||
           std::holds_alternative<IdleOp>(op);
}

// 2x2 complex matrix multiply: C = A * B.
// Using the Mat2x2 (SoA double) representation from gate_matrix.hpp.
detail::Mat2x2 mat_mul(const detail::Mat2x2& A, const detail::Mat2x2& B) {
    // C[0][0] = A[0][0]*B[0][0] + A[0][1]*B[1][0]
    // C[0][1] = A[0][0]*B[0][1] + A[0][1]*B[1][1]
    // C[1][0] = A[1][0]*B[0][0] + A[1][1]*B[1][0]
    // C[1][1] = A[1][0]*B[0][1] + A[1][1]*B[1][1]
    // Each element is a complex multiply-add pair.
    detail::Mat2x2 C;
    // C[0][0]
    C.u00r = A.u00r*B.u00r - A.u00i*B.u00i + A.u01r*B.u10r - A.u01i*B.u10i;
    C.u00i = A.u00r*B.u00i + A.u00i*B.u00r + A.u01r*B.u10i + A.u01i*B.u10r;
    // C[0][1]
    C.u01r = A.u00r*B.u01r - A.u00i*B.u01i + A.u01r*B.u11r - A.u01i*B.u11i;
    C.u01i = A.u00r*B.u01i + A.u00i*B.u01r + A.u01r*B.u11i + A.u01i*B.u11r;
    // C[1][0]
    C.u10r = A.u10r*B.u00r - A.u10i*B.u00i + A.u11r*B.u10r - A.u11i*B.u10i;
    C.u10i = A.u10r*B.u00i + A.u10i*B.u00r + A.u11r*B.u10i + A.u11i*B.u10r;
    // C[1][1]
    C.u11r = A.u10r*B.u01r - A.u10i*B.u01i + A.u11r*B.u11r - A.u11i*B.u11i;
    C.u11i = A.u10r*B.u01i + A.u10i*B.u01r + A.u11r*B.u11i + A.u11i*B.u11r;
    return C;
}

constexpr double kIdentityTol = 1e-12;

// Check if M is (up to global phase) the identity.
// M ~ I iff |M[0][0]| ~ 1 and |M[0][1]| ~ 0 (and similarly for the other
// entries), or equivalently |M - e^{ig}I| ~ 0 for some g.
// We check via the Frobenius distance from e^{ig}I where g = arg(M[0][0]).
bool is_identity(const detail::Mat2x2& M) {
    // Compute global phase from M[0][0].
    const double norm00 = std::hypot(M.u00r, M.u00i);
    if (norm00 < kIdentityTol) {
        // |M[0][0]| ~ 0 means it is NOT near identity (theta would be pi).
        return false;
    }
    // e^{-ig} * M[0][0] should be ~1.
    const double cos_g = M.u00r / norm00;
    const double sin_g = M.u00i / norm00;
    // After removing phase: M' = e^{-ig}*M must be near I.
    // M'[0][0] = (M.u00r*cos_g + M.u00i*sin_g, 0) = norm00 ~ 1.
    // M'[0][1] = M.u01 rotated by -g.
    // M'[1][0] = M.u10 rotated by -g.
    // M'[1][1] = M.u11 rotated by -g.
    // Check |M'[i][j] - delta_ij|^2 summed.
    auto rotated_r = [&](double r, double i) { return r*cos_g + i*sin_g; };
    auto rotated_i = [&](double r, double i) { return -r*sin_g + i*cos_g; };

    const double d00r = rotated_r(M.u00r, M.u00i) - 1.0;
    const double d00i = rotated_i(M.u00r, M.u00i);
    const double d01r = rotated_r(M.u01r, M.u01i);
    const double d01i = rotated_i(M.u01r, M.u01i);
    const double d10r = rotated_r(M.u10r, M.u10i);
    const double d10i = rotated_i(M.u10r, M.u10i);
    const double d11r = rotated_r(M.u11r, M.u11i) - 1.0;
    const double d11i = rotated_i(M.u11r, M.u11i);

    const double frob2 = d00r*d00r + d00i*d00i +
                         d01r*d01r + d01i*d01i +
                         d10r*d10r + d10i*d10i +
                         d11r*d11r + d11i*d11i;
    return frob2 < kIdentityTol * kIdentityTol;
}

// Decompose a 2x2 unitary M into U3(theta, phi, lambda), discarding global
// phase.  Returns {theta, phi, lambda} in the vector<double> params layout
// expected by GateOp (i.e. params = {theta, phi, lambda}).
//
// U3 convention (matches gate_matrix.hpp GateType::U3):
//   U3(theta, phi, lambda) =
//     [[cos(t/2),            -e^{il}*sin(t/2)],
//      [e^{ip}*sin(t/2),     e^{i(p+l)}*cos(t/2)]]
//   where t=theta, p=phi, l=lambda.
//
// Given M (up to global phase e^{ig}):
//   M[0][0] = e^{ig} * cos(t/2)
//   M[1][0] = e^{i(g+p)} * sin(t/2)
//   theta = 2 * atan2(|M[1][0]|, |M[0][0]|)
//   global_phase g = atan2(Im M[0][0], Re M[0][0])
//   phi = atan2(Im M[1][0], Re M[1][0]) - g
//   lambda = atan2(-Im M[0][1], -Re M[0][1]) - g
//          (from M[0][1] = -e^{i(g+l)}*sin(t/2))
//
// phi and lambda are normalised to [-pi, pi] via atan2.
std::vector<double> decompose_u3(const detail::Mat2x2& M) {
    const double abs00 = std::hypot(M.u00r, M.u00i);
    const double abs10 = std::hypot(M.u10r, M.u10i);

    // theta: robust via atan2 of the two magnitudes.
    const double theta = 2.0 * std::atan2(abs10, abs00);

    // Global phase from M[0][0]; fallback to M[1][0] if abs00 is tiny.
    double global_phase;
    if (abs00 > kIdentityTol) {
        global_phase = std::atan2(M.u00i, M.u00r);
    } else {
        // theta ~ pi: M[0][0] is near zero; use M[1][0] for the phase.
        // M[1][0] = e^{i(g+p)}*sin(t/2), so g+p = atan2(Im M[1][0], Re M[1][0]).
        // We set g = 0 in this degenerate case (phi absorbs the offset).
        global_phase = 0.0;
    }

    // phi from M[1][0].
    double phi;
    if (abs10 > kIdentityTol) {
        phi = std::atan2(M.u10i, M.u10r) - global_phase;
    } else {
        phi = 0.0;  // sin(theta/2) ~ 0, phi is undefined -- set to 0.
    }

    // lambda from M[0][1] = -e^{i(g+l)}*sin(t/2).
    double lambda;
    const double abs01 = std::hypot(M.u01r, M.u01i);
    if (abs01 > kIdentityTol) {
        // -e^{i(g+l)} means the angle of M[0][1] is pi + g + lambda.
        // atan2(-M.u01i, -M.u01r) removes the leading minus.
        lambda = std::atan2(-M.u01i, -M.u01r) - global_phase;
    } else {
        lambda = 0.0;  // sin(theta/2) ~ 0, lambda is undefined -- set to 0.
    }

    // Normalise phi and lambda to [-pi, pi] by wrapping via atan2(sin,cos).
    phi    = std::atan2(std::sin(phi),    std::cos(phi));
    lambda = std::atan2(std::sin(lambda), std::cos(lambda));

    return {theta, phi, lambda};
}

// Pending window for one qubit: list of (op-vector index, GateOp*) pairs.
struct QubitRun {
    std::vector<std::size_t> indices;

    void reset() { indices.clear(); }
    std::size_t size() const { return indices.size(); }
};

// Description of a replacement: replace ops at run.indices with a single U3
// at surviving_idx, or drop all of them if is_identity.
struct Replacement {
    std::vector<std::size_t> run_indices;   // all indices in the run
    bool                     is_identity;   // entire run composes to identity
    std::vector<double>      u3_params;     // {theta, phi, lambda} if !is_identity
};

}  // anonymous namespace

std::string MergeSingleQubitUnitaryPass::name() const {
    return "MergeSingleQubitUnitaryPass";
}

Circuit MergeSingleQubitUnitaryPass::run(Circuit circuit) {
    changed_ = false;

    std::vector<Op> ops(circuit.ops().begin(), circuit.ops().end());
    const QubitIndex nq = circuit.num_qubits();

    std::vector<QubitRun> runs(static_cast<std::size_t>(nq));
    std::vector<Replacement> replacements;

    // Process a completed run for qubit q: compose matrices and record
    // replacement when run length >= 2.
    auto flush_run = [&](QubitIndex q) {
        QubitRun& run = runs[q];
        if (run.size() < 2) {
            run.reset();
            return;
        }

        // Compose matrices M = M_last * ... * M_first.
        // Gate application order: M_first acts first (on the right), so the
        // combined unitary is M = M_last * M_{last-1} * ... * M_first.
        const GateOp& g0 = std::get<GateOp>(ops[run.indices[0]]);
        detail::Mat2x2 M = detail::gate_matrix(g0.type, g0.params);

        for (std::size_t k = 1; k < run.size(); ++k) {
            const GateOp& gk = std::get<GateOp>(ops[run.indices[k]]);
            M = mat_mul(detail::gate_matrix(gk.type, gk.params), M);
        }

        Replacement r;
        r.run_indices = run.indices;

        if (is_identity(M)) {
            r.is_identity = true;
        } else {
            r.is_identity = false;
            r.u3_params   = decompose_u3(M);
        }

        replacements.push_back(std::move(r));
        run.reset();
    };

    // Scan ops and build runs.
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];

        if (is_boundary(op)) {
            for (QubitIndex q = 0; q < nq; ++q) {
                if (!runs[q].indices.empty()) flush_run(q);
            }
            continue;
        }

        if (!std::holds_alternative<GateOp>(op)) continue;
        const GateOp& g = std::get<GateOp>(op);

        if (g.qubits.size() > 1) {
            // Multi-qubit gate: flush all involved qubits.
            for (QubitIndex q : g.qubits) {
                if (q < nq && !runs[q].indices.empty()) flush_run(q);
            }
            continue;
        }

        if (g.qubits.empty()) continue;
        const QubitIndex q = g.qubits[0];
        if (q >= nq) continue;

        if (!is_composable_1q(g)) {
            // Boundary for this qubit.
            if (!runs[q].indices.empty()) flush_run(q);
            continue;
        }

        runs[q].indices.push_back(i);
    }

    // Flush remaining runs at end of circuit.
    for (QubitIndex q = 0; q < nq; ++q) {
        if (!runs[q].indices.empty()) flush_run(q);
    }

    if (replacements.empty()) {
        return circuit;
    }

    changed_ = true;

    // Build an index-to-replacement map.
    // surviving_idx = first index in the run (we rewrite that slot to U3 or
    // identity).  All other run indices are dropped.
    std::unordered_map<std::size_t, const Replacement*> survivor_map;
    std::unordered_map<std::size_t, bool>               drop_set;

    for (const auto& r : replacements) {
        // First index is the survivor (rewritten); rest are dropped.
        survivor_map[r.run_indices[0]] = &r;
        for (std::size_t k = 1; k < r.run_indices.size(); ++k) {
            drop_set[r.run_indices[k]] = true;
        }
        // If is_identity, even the first slot is dropped.
        if (r.is_identity) {
            drop_set[r.run_indices[0]] = true;
            survivor_map.erase(r.run_indices[0]);
        }
    }

    std::vector<Op> new_ops;
    new_ops.reserve(ops.size());

    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (drop_set.count(i)) continue;

        auto it = survivor_map.find(i);
        if (it != survivor_map.end()) {
            // Rewrite as U3.
            const Replacement* r = it->second;
            GateOp merged = std::get<GateOp>(ops[r->run_indices[0]]);
            merged.type   = GateType::U3;
            merged.name   = "u3";
            merged.params = r->u3_params;  // {theta, phi, lambda}
            new_ops.push_back(Op{merged});
        } else {
            new_ops.push_back(ops[i]);
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

bool MergeSingleQubitUnitaryPass::changed() const {
    return changed_;
}

}  // namespace qplayer
