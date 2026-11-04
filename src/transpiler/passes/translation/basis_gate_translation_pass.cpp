// basis_gate_translation_pass.cpp — BasisGateTranslationPass (,
//). Decomposes logical gates into the device native
// basis:
//   - 1Q non-basis gate -> ZSX Euler: from its 2x2 unitary extract (theta,phi,lam)
//     and emit RZ(lam)·SX·RZ(theta+pi)·SX·RZ(phi+pi) (trivial RZ dropped). Global
//     phase is discarded (unobservable; rz is virtual).
//   - cx -> ecr template: RZ(-pi/2)_c · RZ(-pi)_t · SX_t · RZ(-pi)_t · ECR(c,t) · X_c
//     (verified == CX up to global phase with the ecr_matrix_data convention).
// - cx -> cz template: H_t · CZ(c,t) · H_t, H decomposed via ZSX Euler.
//   - swap -> cx×3, each cx then translated to the native 2Q gate.
//   - gates already in basis_gates pass through.
// No-op when the device declares no basis_gates (nothing to translate to).

#include "transpiler/passes/translation/basis_gate_translation_pass.hpp"

#include <cmath>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "backend/full_state/gate_matrix.hpp"   // detail::gate_matrix (2x2)
#include "circuit/circuit.hpp"
#include "error.hpp"
#include "log.hpp"

namespace qplayer {

namespace {

using detail::Mat2x2;

// Build a GateOp shorthand.
GateOp mk(GateType type, std::string name,
          std::vector<QubitIndex> qubits, std::vector<double> params = {}) {
    GateOp g;
    g.type   = type;
    g.name   = std::move(name);
    g.qubits = std::move(qubits);
    g.params = std::move(params);
    return g;
}

// Reduce an angle to (-pi, pi]; treat |angle| < 1e-12 as zero.
double norm_angle(double a) { return std::remainder(a, 2.0 * M_PI); }

// Append RZ(angle) on q unless the (reduced) angle is ~0 — rz(0) == identity.
void push_rz(std::vector<GateOp>& out, QubitIndex q, double angle) {
    const double a = norm_angle(angle);
    if (std::abs(a) < 1e-12) return;
    out.push_back(mk(GateType::RZ, "rz", {q}, {a}));
}

double arg_of(double re, double im) { return std::atan2(im, re); }

// ZSX Euler decomposition of an arbitrary single-qubit gate. Extract
// U3 angles (theta,phi,lam) from the 2x2 unitary, then emit the RZ/SX sequence.
std::vector<GateOp> euler_zsx(const GateOp& g) {
    const Mat2x2 U = detail::gate_matrix(g.type, g.params);
    const QubitIndex q = g.qubits[0];

    const double a00 = std::hypot(U.u00r, U.u00i);
    const double a10 = std::hypot(U.u10r, U.u10i);
    const double theta = 2.0 * std::atan2(a10, a00);

    double phi = 0.0, lam = 0.0;
    const double eps = 1e-12;
    if (a00 > eps && a10 > eps) {
        phi = arg_of(U.u10r, U.u10i) - arg_of(U.u00r, U.u00i);
        lam = arg_of(-U.u01r, -U.u01i) - arg_of(U.u00r, U.u00i);
    } else if (a00 > eps) {
        // sin(theta/2) ~ 0: diagonal gate. lam carries the full relative phase.
        lam = arg_of(U.u11r, U.u11i) - arg_of(U.u00r, U.u00i);
    } else {
        // cos(theta/2) ~ 0: anti-diagonal gate.
        phi = arg_of(U.u10r, U.u10i) - arg_of(-U.u01r, -U.u01i);
    }

    std::vector<GateOp> out;
    push_rz(out, q, lam);
    out.push_back(mk(GateType::SX, "sx", {q}));
    push_rz(out, q, theta + M_PI);
    out.push_back(mk(GateType::SX, "sx", {q}));
    push_rz(out, q, phi + M_PI);
    return out;
}

// cx(control, target) -> ecr-based template (verified == CX up to global phase).
std::vector<GateOp> cx_to_ecr(QubitIndex c, QubitIndex t) {
    std::vector<GateOp> out;
    push_rz(out, c, -M_PI / 2.0);
    push_rz(out, t, -M_PI);
    out.push_back(mk(GateType::SX, "sx", {t}));
    push_rz(out, t, -M_PI);
    out.push_back(mk(GateType::ECR, "ecr", {c, t}));
    out.push_back(mk(GateType::X, "x", {c}));
    return out;
}

// cx(control, target) -> cz-based template.
// Identity: CX(c,t) = H_t · CZ(c,t) · H_t  (target qubit sandwiched with H).
// H is not in the CZ-native basis, so it is further decomposed via ZSX Euler.
// Requires rz + sx in the device basis (for the H ZSX decomposition).
std::vector<GateOp> cx_to_cz(QubitIndex c, QubitIndex t) {
    const GateOp h_gate = mk(GateType::H, "h", {t});
    std::vector<GateOp> h_seq = euler_zsx(h_gate);  // H_t -> rz·sx·rz·sx·rz
    std::vector<GateOp> out;
    // Pre-CZ H on target.
    out.insert(out.end(), h_seq.begin(), h_seq.end());
    // Native CZ gate (symmetric, operand order does not affect the unitary).
    out.push_back(mk(GateType::CZ, "cz", {c, t}));
    // Post-CZ H on target (H is its own inverse, so same sequence).
    out.insert(out.end(), h_seq.begin(), h_seq.end());
    return out;
}

}  // namespace

BasisGateTranslationPass::BasisGateTranslationPass(const DeviceModel& device)
    : device_(&device) {}

std::string BasisGateTranslationPass::name() const {
    return "BasisGateTranslationPass";
}

Circuit BasisGateTranslationPass::run(Circuit circuit) {
    changed_ = false;

    const std::vector<std::string>& basis = device_->basis_gates();
    // Without a declared basis the pass cannot know its target — no-op (a device
    // JSON that omits basis_gates keeps the pre-translation circuit).
    if (basis.empty()) {
        return circuit;
    }
    const std::unordered_set<std::string> basis_set(basis.begin(), basis.end());

    // Native 2Q gate: prefer the explicit field, else infer from the basis set.
    // Supported: cx (IBM Falcon/Eagle), ecr (IBM Eagle/Heron), cz (Google Willow).
    GateType nat2q      = GateType::CX;
    bool     have_nat2q = false;
    if (device_->has_two_qubit_gate()) {
        const std::string& n = device_->two_qubit_gate();
        if      (n == "cx")  { nat2q = GateType::CX;  have_nat2q = true; }
        else if (n == "ecr") { nat2q = GateType::ECR; have_nat2q = true; }
        else if (n == "cz")  { nat2q = GateType::CZ;  have_nat2q = true; }  //
    }
    if (!have_nat2q) {
        if      (basis_set.count("ecr")) { nat2q = GateType::ECR; have_nat2q = true; }
        else if (basis_set.count("cx"))  { nat2q = GateType::CX;  have_nat2q = true; }
        else if (basis_set.count("cz"))  { nat2q = GateType::CZ;  have_nat2q = true; }  //
    }

    // ZSX decomposition needs rz + sx in the basis; the ecr template also needs x.
    const bool have_zsx = basis_set.count("rz") && basis_set.count("sx");

    // Translate one cx into the native 2Q form (passthrough or ecr/cz template).
    auto translate_cx = [&](QubitIndex c, QubitIndex t) -> std::vector<GateOp> {
        if (nat2q == GateType::CX) return {mk(GateType::CX, "cx", {c, t})};
        if (nat2q == GateType::ECR) {
            if (!basis_set.count("x"))
                throw CircuitError{"BasisGateTranslationPass: ecr template needs "
                                   "'x' in the device basis"};
            return cx_to_ecr(c, t);
        }
        if (nat2q == GateType::CZ) {
            // cx -> H_t · CZ · H_t; H is ZSX-decomposed, so rz+sx must be present
            //
            if (!(basis_set.count("rz") && basis_set.count("sx")))
                throw CircuitError{"BasisGateTranslationPass: cz template needs "
                                   "rz/sx in the device basis for H (ZSX) "
                                   "decomposition"};
            return cx_to_cz(c, t);
        }
        throw CircuitError{"BasisGateTranslationPass: no native two-qubit gate to "
                           "translate cx into"};
    };

    // Translate a single GateOp into a native-basis sequence (may be {itself}).
    auto translate_gate = [&](const GateOp& g) -> std::vector<GateOp> {
        // Non-unitary / user opaque are not touched by basis translation.
        if (g.type == GateType::Reset || g.type == GateType::Opaque || g.is_opaque)
            return {g};
        // Already native.
        if (basis_set.count(g.name)) return {g};

        if (g.qubits.size() == 1) {
            if (!have_zsx)
                throw CircuitError{"BasisGateTranslationPass: device basis lacks "
                                   "rz/sx for single-qubit (ZSX) decomposition"};
            return euler_zsx(g);
        }
        if (g.type == GateType::CX) {
            return translate_cx(g.qubits[0], g.qubits[1]);
        }
        if (g.type == GateType::SWAP) {
            const QubitIndex a = g.qubits[0], b = g.qubits[1];
            std::vector<GateOp> out;
            for (auto pr : {std::pair<QubitIndex, QubitIndex>{a, b},
                            {b, a}, {a, b}}) {
                std::vector<GateOp> step = translate_cx(pr.first, pr.second);
                out.insert(out.end(), step.begin(), step.end());
            }
            return out;
        }
        // Arbitrary 2Q/3Q (cz, cy, ch, crz, cu1, cu3, ccx, cswap, non-native ecr):
        // no rule-based decomposition; KAK for arbitrary 2Q is deferred.
        throw CircuitError{"BasisGateTranslationPass: cannot decompose '" + g.name +
                           "' into the device basis (arbitrary multi-qubit gate; "
                           "KAK synthesis is)"};
    };

    std::vector<Op> out_ops;
    out_ops.reserve(circuit.ops().size());

    for (const Op& op : circuit.ops()) {
        if (const auto* g = std::get_if<GateOp>(&op)) {
            std::vector<GateOp> seq = translate_gate(*g);
            if (seq.size() != 1 || seq[0].type != g->type) changed_ = true;
            for (GateOp& out_g : seq) out_ops.push_back(std::move(out_g));
        } else if (const auto* iff = std::get_if<IfOp>(&op)) {
            // Conditionally-applied gate: decompose the body and re-wrap each
            // resulting gate in an IfOp with the same guard (semantically the
            // whole gate is applied iff creg == value).
            std::vector<GateOp> seq = translate_gate(iff->body);
            if (seq.size() != 1 || seq[0].type != iff->body.type) changed_ = true;
            for (GateOp& out_g : seq) {
                IfOp cond;
                cond.creg  = iff->creg;
                cond.value = iff->value;
                cond.body  = std::move(out_g);
                out_ops.push_back(std::move(cond));
            }
        } else {
            // MeasureOp / BarrierOp / IdleOp: no basis translation.
            out_ops.push_back(op);
        }
    }

    log::debug("BasisGateTranslationPass: translated to device basis (" +
               std::to_string(out_ops.size()) + " ops)");

    return detail_make_circuit_from_payload(
        circuit.num_qubits(),
        circuit.classical_registers(),
        circuit.opaque_definitions(),
        std::move(out_ops),
        circuit.expectation_targets(),
        circuit.metadata());
}

bool BasisGateTranslationPass::changed() const {
    return changed_;
}

}  // namespace qplayer
