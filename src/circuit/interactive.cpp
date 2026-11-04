/// @file interactive.cpp
/// @brief Real implementation of the Interactive facade (
///
///). Delegates all gate / measure / state-access calls directly to the
/// bound SimulatorBackend. Typed gate methods are thin wrappers over apply_gate().
///
/// Design decisions:
/// - TensorNetwork backend rejects initialize() with SimulatorError per the
/// feature matrix (CLAUDE.md).
/// - measure(q, cubit) accumulates 0/1 outcomes into cbits_[] and builds an
/// MSB-first bitstring matching the Batch run() path (result.cpp).
/// - probability(q, outcome) is computed from get_statevector() because the
/// backend interface exposes no per-qubit probability API. For SparseState
/// get_statevector_raw() throws; we fall through to get_statevector() (AoS)
/// which is valid for num_qubits <= MAX_DENSE_EXPORT_QUBITS.
/// - active_ guard applies before every user-visible method; it throws
/// SimulatorError with the backend's name() literal.

#include "circuit/interactive.hpp"

// Needs the complete QRegister type for the switch (owner_->...).
#include "qregister.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "backend/simulator_backend.hpp"
#include "circuit/gate_op.hpp"
#include "error.hpp"
#include "types.hpp"

namespace qplayer {

// ── Internal helpers ──────────────────────────────────────────────────────────

namespace {

// Throw SimulatorError if the Interactive session has not been initialised.
// bt is the backend name literal from backend_->name() (coding-rules).
[[noreturn]] void throw_not_active(std::string_view bt) {
    throw SimulatorError{std::string{bt},
        "Interactive not initialized; call initialize() first"};
}

// Build a 1-qubit GateOp with no parameters (h, x, y, z, s, sdg, t, tdg).
GateOp make_1q(GateType type, const char* name, QubitIndex q) {
    GateOp op;
    op.type   = type;
    op.name   = name;
    op.qubits = {q};
    return op;
}

// Build a 1-qubit GateOp with rotation parameters (rx, ry, rz, u1, u2, u3).
GateOp make_1q_params(GateType type, const char* name,
                      QubitIndex q, std::vector<double> params) {
    GateOp op;
    op.type   = type;
    op.name   = name;
    op.qubits = {q};
    op.params = std::move(params);
    return op;
}

// Build a 2-qubit GateOp with no parameters (cx, cy, cz, swap).
GateOp make_2q(GateType type, const char* name,
               QubitIndex ctrl, QubitIndex tgt) {
    GateOp op;
    op.type   = type;
    op.name   = name;
    op.qubits = {ctrl, tgt};
    return op;
}

}  // namespace

// ── Bind / lifecycle ──────────────────────────────────────────────────────────

void Interactive::bind_backend(SimulatorBackend* backend) {
    backend_ = backend;
}

// bind_owner(): called once from QRegister's constructor so initialize() can
// request the CPU switch. Never called from the run() fallback path —
// the owner does not change.
void Interactive::bind_owner(QRegister* owner) {
    owner_ = owner;
}

// initialize(): allocate |0...0> on the backend. Interactive supports CPU
// statevector backends only (FullState CPU, SparseState). TensorNetwork (tensor-
// graph model, no incremental gate apply) and FullState GPU (batch execute()
// fast-path only — no immediate-mode kernels) both reject here.
// sim_type() returns FullState for both CPU and GPU, so the GPU variant is
// identified by its backend name() literal.
// Resets accumulated classical state so a second initialize() starts clean.
void Interactive::initialize(QubitIndex num_qubits) {
    if (backend_->sim_type() == SimType::TensorNetwork) {
        throw SimulatorError{"TensorNetwork", "Interactive mode not supported"};
    }
    if (backend_->name() == std::string_view("FullStateGPU")) {
        // ProcType::Auto means the user delegated the proc choice, so picking a
        // proc that cannot run Interactive and then throwing breaks the delegation
        // contract: `QRegister qr; qr.interactive.initialize(n);` — both documented
        // defaults — failed unconditionally on any GPU host (audit #10). Switch to
        // CPU and say so. An explicit ProcType::GPU is a user decision and still
        // errors.
        if (owner_ != nullptr && owner_->proc_type_ == ProcType::Auto) {
            owner_->switch_to_cpu_backend_for_interactive();
            // backend_ was rebound by the owner; fall through on the CPU backend.
        } else {
            throw SimulatorError{"FullStateGPU",
                "Interactive mode is CPU-only; use QRegister(FullState, ProcType::CPU) "
                "or the batch run() path for GPU"};
        }
    }
    backend_->init_state(num_qubits);
    num_qubits_ = num_qubits;
    active_     = true;
    // Clear accumulated classical state.
    cbits_.assign(static_cast<std::size_t>(num_qubits), 0);
    accumulated_result_ = Result{};
}

// reset(): re-prepare |0...0> with the same qubit count (same as reinitialise).
void Interactive::reset() {
    if (!active_) throw_not_active(backend_->name());
    backend_->init_state(num_qubits_);
    cbits_.assign(static_cast<std::size_t>(num_qubits_), 0);
    accumulated_result_ = Result{};
}

// reset(q): collapse qubit q to |0> while leaving the rest untouched.
void Interactive::reset(QubitIndex q) {
    if (!active_) throw_not_active(backend_->name());
    backend_->apply_reset(q);
}

// ── Generic gate entry point ───────────────────────────────────────

Interactive& Interactive::apply_gate(const GateOp& op) {
    if (!active_) throw_not_active(backend_->name());
    backend_->apply_gate(op);
    return *this;
}

// ── Typed single-qubit gates (thin wrappers over apply_gate) ─────────────────

Interactive& Interactive::h  (QubitIndex q) { return apply_gate(make_1q(GateType::H,   "h",   q)); }
Interactive& Interactive::x  (QubitIndex q) { return apply_gate(make_1q(GateType::X,   "x",   q)); }
Interactive& Interactive::y  (QubitIndex q) { return apply_gate(make_1q(GateType::Y,   "y",   q)); }
Interactive& Interactive::z  (QubitIndex q) { return apply_gate(make_1q(GateType::Z,   "z",   q)); }
Interactive& Interactive::s  (QubitIndex q) { return apply_gate(make_1q(GateType::S,   "s",   q)); }
Interactive& Interactive::sdg(QubitIndex q) { return apply_gate(make_1q(GateType::Sdg, "sdg", q)); }
Interactive& Interactive::t  (QubitIndex q) { return apply_gate(make_1q(GateType::T,   "t",   q)); }
Interactive& Interactive::tdg(QubitIndex q) { return apply_gate(make_1q(GateType::Tdg, "tdg", q)); }

Interactive& Interactive::rx(double theta, QubitIndex q) {
    return apply_gate(make_1q_params(GateType::RX, "rx", q, {theta}));
}
Interactive& Interactive::ry(double theta, QubitIndex q) {
    return apply_gate(make_1q_params(GateType::RY, "ry", q, {theta}));
}
Interactive& Interactive::rz(double theta, QubitIndex q) {
    return apply_gate(make_1q_params(GateType::RZ, "rz", q, {theta}));
}
Interactive& Interactive::u1(double lam, QubitIndex q) {
    return apply_gate(make_1q_params(GateType::U1, "u1", q, {lam}));
}
Interactive& Interactive::u2(double phi, double lam, QubitIndex q) {
    return apply_gate(make_1q_params(GateType::U2, "u2", q, {phi, lam}));
}
Interactive& Interactive::u3(double theta, double phi, double lam, QubitIndex q) {
    return apply_gate(make_1q_params(GateType::U3, "u3", q, {theta, phi, lam}));
}

// ── Typed two-qubit gates ─────────────────────────────────────────────────────

Interactive& Interactive::cx  (QubitIndex ctrl, QubitIndex tgt) { return apply_gate(make_2q(GateType::CX,   "cx",   ctrl, tgt)); }
Interactive& Interactive::cy  (QubitIndex ctrl, QubitIndex tgt) { return apply_gate(make_2q(GateType::CY,   "cy",   ctrl, tgt)); }
Interactive& Interactive::cz  (QubitIndex ctrl, QubitIndex tgt) { return apply_gate(make_2q(GateType::CZ,   "cz",   ctrl, tgt)); }
Interactive& Interactive::swap(QubitIndex q0,   QubitIndex q1)  { return apply_gate(make_2q(GateType::SWAP, "swap", q0,   q1));  }

// Google native 2Q gates: immediate dispatch via apply_gate().
Interactive& Interactive::iswap     (QubitIndex q0, QubitIndex q1) { return apply_gate(make_2q(GateType::ISWAP,      "iswap",      q0, q1)); }
Interactive& Interactive::sqrt_iswap(QubitIndex q0, QubitIndex q1) { return apply_gate(make_2q(GateType::SQRT_ISWAP, "sqrt_iswap", q0, q1)); }
Interactive& Interactive::fsim(double theta, double phi, QubitIndex q0, QubitIndex q1) {
    GateOp op;
    op.type   = GateType::FSIM;
    op.name   = "fsim";
    op.qubits = {q0, q1};
    op.params = {theta, phi};
    return apply_gate(op);
}
// sycamore = fsim(pi/2, pi/6) convenience wrapper.
Interactive& Interactive::sycamore(QubitIndex q0, QubitIndex q1) {
    return fsim(M_PI / 2.0, M_PI / 6.0, q0, q1);
}

// ── Three-qubit gate ──────────────────────────────────────────────────────────

Interactive& Interactive::ccx(QubitIndex c0, QubitIndex c1, QubitIndex tgt) {
    if (!active_) throw_not_active(backend_->name());
    GateOp op;
    op.type   = GateType::CCX;
    op.name   = "ccx";
    op.qubits = {c0, c1, tgt};
    return apply_gate(op);
}

// ── Barrier — no-op for immediate-dispatch execution ─────────────────────────

Interactive& Interactive::barrier() {
    if (!active_) throw_not_active(backend_->name());
    return *this;
}

// ── Measurement ───────────────────────────────────────────────────────────────

// measure(q): Born-rule collapse, return 0 or 1. No cubit write.
int Interactive::measure(QubitIndex q) {
    if (!active_) throw_not_active(backend_->name());
    return backend_->apply_measure(q);
}

// measure(q, cubit): Born-rule collapse, write into cbits_[cubit], and
// rebuild accumulated_result_.counts_ with the current bitstring (1 shot).
// Bitstring ordering mirrors result.cpp: MSB-first over cubit indices
// (bs[k] = cubit[total_cubits-1-k], k in [0, total_cubits)).
void Interactive::measure(QubitIndex q, ClassicalBitIndex cubit) {
    if (!active_) throw_not_active(backend_->name());
    const int outcome = backend_->apply_measure(q);
    // Grow cbits_ if needed (cubit indices can arrive out of order).
    const std::size_t needed = static_cast<std::size_t>(cubit) + 1u;
    if (cbits_.size() < needed) {
        cbits_.resize(needed, 0);
    }
    cbits_[cubit] = outcome;
    // Rebuild counts in accumulated_result_ with current cbits_ state.
    // total_cubits = current size of cbits_.
    const ClassicalBitIndex total_c =
        static_cast<ClassicalBitIndex>(cbits_.size());
    std::string bs(total_c, '0');
    for (ClassicalBitIndex k = 0; k < total_c; ++k) {
        // MSB-first: position k in string = cubit index (total_c-1-k).
        bs[k] = static_cast<char>('0' + cbits_[total_c - 1u - k]);
    }
    // Overwrite counts with a single-shot entry. This matches the interactive
    // "one trajectory" model — counts shows the current measurement outcome.
    accumulated_result_.counts_.clear();
    accumulated_result_.counts_[bs] = 1;
    accumulated_result_.shots_ = 1;
}

// measure_all(): — index i carries qubit i's outcome, size = num_qubits_.
std::vector<int> Interactive::measure_all() {
    if (!active_) throw_not_active(backend_->name());
    std::vector<int> outcomes;
    outcomes.reserve(static_cast<std::size_t>(num_qubits_));
    for (QubitIndex i = 0; i < num_qubits_; ++i) {
        outcomes.push_back(backend_->apply_measure(i));
    }
    return outcomes;
}

// ── State access ──────────────────────────────────────────────────────────────

StateVector Interactive::get_statevector() const {
    if (!active_) throw_not_active(backend_->name());
    return backend_->get_statevector();
}

RawStateView Interactive::get_statevector_raw() const {
    if (!active_) throw_not_active(backend_->name());
    return backend_->get_statevector_raw();
}

// get_statevector_nonzero() — delegate to the backend.
// Requires an active session; throws SimulatorError if not initialised.
std::vector<std::pair<StateIndex, std::complex<double>>>
Interactive::get_statevector_nonzero() const {
    if (!active_) throw_not_active(backend_->name());
    return backend_->nonzero_amplitudes();
}

// probability(q, outcome): P(qubit q collapses to |outcome>) from statevector.
// Backend has no direct per-qubit probability API, so we sum over the full
// statevector. FullState reads the raw SoA view (no copy); SparseState falls back
// to the AoS get_statevector() because it has no SoA form.
// Basis index bit q = outcome selects contributing amplitudes.
double Interactive::probability(QubitIndex q, int outcome) const {
    if (!active_) throw_not_active(backend_->name());
    const StateIndex mask = StateIndex{1} << q;
    double prob = 0.0;

    // FullState exposes the internal SoA buffers, so read them in place. The AoS
    // get_statevector() path allocated and copied the whole 2^n vector on EVERY
    // call — 2 GiB per call at 27 qubits — even though this only needs a
    // read-only scan (audit #26). SparseState has no SoA representation and
    // throws from get_statevector_raw(), so it keeps the AoS path;
    // there the densified vector is bounded by MAX_DENSE_EXPORT_QUBITS anyway.
    if (backend_->sim_type() == SimType::FullState) {
        const RawStateView view = backend_->get_statevector_raw();
        for (StateIndex i = 0; i < view.size; ++i) {
            const bool bit_set = ((i & mask) != StateIndex{0});
            if (bit_set == (outcome != 0)) {
                const std::size_t k = static_cast<std::size_t>(i);
                prob += view.re[k] * view.re[k] + view.im[k] * view.im[k];
            }
        }
        return prob;
    }

    const StateVector sv = backend_->get_statevector();
    const StateIndex dim = static_cast<StateIndex>(sv.size());
    for (StateIndex i = 0; i < dim; ++i) {
        const bool bit_set = ((i & mask) != StateIndex{0});
        if (bit_set == (outcome != 0)) {
            const double re = sv[static_cast<std::size_t>(i)].real();
            const double im = sv[static_cast<std::size_t>(i)].imag();
            prob += re * re + im * im;
        }
    }
    return prob;
}

double Interactive::expectation_value(std::string_view pauli) const {
    if (!active_) throw_not_active(backend_->name());
    return backend_->expectation_value(pauli);
}

// ── Accumulated measurement results ──────────────────────────────────────────

const Result& Interactive::result() const {
    return accumulated_result_;
}

void Interactive::clear_result() {
    accumulated_result_ = Result{};
    cbits_.assign(cbits_.size(), 0);
}

// ── Utility ───────────────────────────────────────────────────────────────────

QubitIndex Interactive::num_qubits() const {
    return num_qubits_;
}

bool Interactive::is_active() const {
    return active_;
}

}  // namespace qplayer
