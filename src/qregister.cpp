/// @file qregister.cpp
/// @brief Phase 0-S implementation of the sole user-facing entry
///
/// point.
///
/// Phase 0-S responsibilities:
/// 1. Construct the backend via make_backend() and bind it to the Interactive
/// member. Backend constructors are trivial (no resource allocation), so
/// this path stays exception-free even though every prepare/execute call
/// later throws NotImplementedError.
/// 2. Provide trivial seed accessors. std::random_device seeds
/// master_seed_ on construction; set_seed() overrides it explicitly.
/// 3. Forward every Phase-3+ API to NotImplementedError with the appropriate
/// phase tag so callers and Python bindings see a uniform stub message.
/// 4. sim_type()/result()/gpu_available() are real because tests can inspect
/// them right away.

#include "qregister.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <omp.h>

#include "backend/memory_check.hpp"
#include "backend/simulator_backend.hpp"
#include "error.hpp"
#include "log.hpp"
#include "parser/parser.hpp"
#include "transpiler/transpiler.hpp"

namespace qplayer {

namespace {

// Parses QPLAYER_SEED into a uint64 master seed. Returns true and
// fills `out` on success; on a malformed value warns once and returns false so
// the caller falls back to std::random_device.
bool seed_from_env(std::uint64_t& out) {
    const char* raw = std::getenv("QPLAYER_SEED");
    if (raw == nullptr) {
        return false;
    }
    try {
        out = std::stoull(raw);
        return true;
    } catch (...) {
        log::warn(std::string("QPLAYER_SEED=") + raw +
                  " is not a valid uint64, ignored");
        return false;
    }
}

}  // namespace

QRegister::QRegister(SimType sim_type, ProcType proc)
    : backend_(make_backend(sim_type, proc)),
      sim_type_(sim_type),
      proc_type_(proc) {
    // Seed bootstrap. Priority: QPLAYER_SEED env > random_device.
    // When the env var locks the seed, later set_seed() calls are ignored.
    std::uint64_t env_seed = 0;
    if (seed_from_env(env_seed)) {
        master_seed_     = env_seed;
        env_seed_locked_ = true;
        log::info("master seed = " + std::to_string(master_seed_) +
                  " (source: QPLAYER_SEED env)");
    } else {
        // Non-deterministic entropy when no explicit seed is supplied; still
        // recoverable via current_seed() for bug reports.
        std::random_device rd;
        master_seed_ = (static_cast<std::uint64_t>(rd()) << 32) |
                        static_cast<std::uint64_t>(rd());
        log::info("master seed = " + std::to_string(master_seed_) +
                  " (source: random_device)");
    }

    // Interactive's private constructor is friended to QRegister so
    // the member is default-constructed via the in-class initialiser, then
    // we bind the backend pointer here. Interactive keeps a non-owning
    // pointer — the unique_ptr above owns the storage.
    interactive.bind_backend(backend_.get());
    interactive.bind_owner(this);

    // Seed the backend with the resolved master seed so the interactive path is
    // reproducible from construction even when set_seed() is never called (the
    // master seed is still recoverable via current_seed()). run() re-applies it
    // before execute(); this just covers the run()-less interactive flow.
    // /.
    backend_->set_seed(master_seed_);
}

QRegister::~QRegister() = default;

// ── Device model ingestion ───────
// Loads the device JSON, stores it as a member for transpile() fallback
// then ingests T1/T2 relaxation into the NoiseModel so
// ALAPSchedulePass can build per-qubit Kraus channels.
void QRegister::load_device_model(std::string_view json_path) {
    device_model_ = std::make_unique<DeviceModel>(
        DeviceModel::from_json_file(std::string(json_path)));
    // Build Amplitude Damping + Dephasing channels from T1/T2 coherence times.
    // load_device() clears any prior synthesised channels first, so each
    // call replaces — never accumulates — the device noise (intent).
    noise_model_.load_device(*device_model_);
    has_noise_model_ = true;
}

// ── Input path 1: OpenQASM v2 parser ───────────────────────────
// Delegates to the Phase 3 Parser facade (Lexer -> AstParser ->
// SemanticValidator -> CircuitBuilder). QASMParseError propagates to the caller.
Circuit QRegister::parse(std::string_view qasm_text) {
    Parser parser;
    return parser.parse(qasm_text);
}

Circuit QRegister::parse_file(std::string_view file_path) {
    Parser parser;
    return parser.parse_file(std::string(file_path));
}

// ── Compile (single entry point) ───────────────────────────────
// Delegates to Transpiler, which builds a PassManager dynamically based on
// optimization_level and device_json.
// device_model_ / noise_model_ members are forwarded as optional fallbacks;
// nullptr when not set (load_device_model / set_noise_model not yet called).
Circuit QRegister::transpile(Circuit          circuit,
                             int              optimization_level,
                             std::string_view device_json) {
    Transpiler t;
    return t.transpile(std::move(circuit),
                       optimization_level,
                       device_json,
                       device_model_.get(),
                       has_noise_model_ ? &noise_model_ : nullptr);
}

// ── Run (Batch execution) ──────────────────────────────────────
// Propagates the master seed and runtime fusion config
// the backend, then runs the / pipeline. The returned
// Result is cached so result() can re-expose it. NoiseModel is forwarded only
// when set; FullState ignores it until Phase 8.
Result QRegister::run(const Circuit& circuit,
                      std::uint32_t  nshots,
                      bool           enable_fusion,
                      std::uint32_t  fusion_max_width) {
    const NoiseModel* noise = has_noise_model_ ? &noise_model_ : nullptr;

    // Dense-space noise handoff (Consequences). When transpile()
    // truncated the circuit onto a dense [0,k) qubit range, its operands are
    // dense but noise_model_ is keyed by physical qubit. Project the model into
    // dense space so each qubit's device T1/T2 / gate error / readout binds to
    // the right physical qubit. No-op for an identity (untruncated) mapping.
    // projected_noise outlives execute() (and the CPU-fallback lambda's `noise`
    // capture) because it lives in this function scope.
    NoiseModel projected_noise;
    if (noise != nullptr && circuit.metadata().qubit_truncation.has_value()) {
        projected_noise = noise->project_to_dense(*circuit.metadata().qubit_truncation);
        noise = &projected_noise;
    }

    backend_->set_seed(master_seed_);
    backend_->set_fusion(enable_fusion, fusion_max_width);

    // Interventions address noise draws, so they need a noise model and a backend
    // that runs noise trajectories. FullState GPU needs no check:
    // it hands every noisy circuit to the CPU backend below.
    if (!noise_interventions_.empty()) {
        if (noise == nullptr) {
            throw SimulatorError{std::string(backend_->name()),
                "noise interventions need a noise model: without one a trajectory "
                "makes no noise draw to intervene on"};
        }
        if (sim_type_ == SimType::SparseState) {
            throw SimulatorError{"SparseState",
                "noise interventions need a noise trajectory backend "
                "(FullState CPU or TensorNetwork)"};
        }
    }
    backend_->set_noise_interventions(noise_interventions_, noise_draw_ledger_);

    // Runtime GPU->CPU fallback. The GPU execute() handles only the
    // noiseless end-measure fast path; circuits it rejects (IfOp/noise/Reset/
    // Opaque -> NotImplementedError) and VRAM OOM (GPUError) are retried on a
    // freshly built CPU backend. The switch is permanent so subsequent state
    // access (get_statevector / expectation_value) observes the same backend
    // and never reads a stale GPU state.
    const bool is_gpu = backend_->name() == std::string_view("FullStateGPU");
    auto fallback_to_cpu_and_run = [&]() -> Result {
        log::warn("FullState GPU backend cannot run this circuit "
                  "(unsupported op or insufficient VRAM); switching to CPU "
                  "backend");
        backend_ = make_backend(SimType::FullState, ProcType::CPU);
        interactive.bind_backend(backend_.get());
        backend_->set_seed(master_seed_);
        backend_->set_fusion(enable_fusion, fusion_max_width);
        backend_->set_noise_interventions(noise_interventions_, noise_draw_ledger_);
        return backend_->execute(circuit, nshots, noise);
    };

    try {
        last_result_ = backend_->execute(circuit, nshots, noise);
    } catch (const NotImplementedError&) {
        if (!is_gpu) throw;          // CPU NotImplemented is a real error
        last_result_ = fallback_to_cpu_and_run();
    } catch (const GPUError&) {
        if (!is_gpu) throw;
        last_result_ = fallback_to_cpu_and_run();
    }

    // fill pre-registered expectation targets into the Result.
    //
    // Division of labour: on the stochastic paths ( trajectory,
    // per-shot) each shot ends in a different state, so <P> is the nshot average
    // and only the backend can accumulate it — it does so inside its shot loop
    // and hands back a populated dict. On the fast path a single final
    // state exists, so one evaluation here suffices. A non-empty dict is the
    // signal that the backend already did the work.
    const auto& targets = circuit.expectation_targets();
    if (!targets.empty() && last_result_.expectations_.empty()) {
        if (!circuit.requires_per_shot() && !has_noise_model_) {
            for (const auto& pauli : targets) {
                // QRegister is a friend of Result — fill the dict.
                last_result_.expectations_[pauli] = backend_->expectation_value(pauli);
            }
        } else {
            // Reached only by a backend whose stochastic path does not yet
            // accumulate (SparseState / TensorNetwork). Reporting the last
            // shot's value here would silently return a different quantity
            // than the documented nshot average, so the dict
            // is left empty and Result::expectation() throws.
            log::warn(std::string("expectation targets registered but backend '") +
                      std::string(backend_->name()) +
                      "' does not accumulate them on the per-shot/noisy path; "
                      "Result.expectation() will report them as unavailable");
        }
    }
    return last_result_;
}

const Result& QRegister::result() const {
    return last_result_;
}

// ── State access ──────────────────
// Thin delegation to the active backend. Per-backend support + error policy
// (e.g. SparseState raw always throws SimulatorError, >MAX_DENSE_EXPORT_QUBITS
// AoS throws) lives in the backend (table).
StateVector QRegister::get_statevector() const {
    return backend_->get_statevector();
}

RawStateView QRegister::get_statevector_raw() const {
    return backend_->get_statevector_raw();
}

double QRegister::expectation_value(std::string_view pauli) const {
    return backend_->expectation_value(pauli);
}

// Weighted Pauli-sum expectation <H> = Sum(c_i * <P_i>).
// Each term reuses the validated pauli overload; an empty Observable is 0.0.
double QRegister::expectation_value(const Observable& obs) const {
    double total = 0.0;
    for (const auto& [coeff, pauli] : obs.terms()) {
        total += coeff * backend_->expectation_value(pauli);
    }
    return total;
}

// get_statevector_nonzero() — delegate to the backend. Thin facade;
// all per-backend semantics (no densify on SparseState, throw on TN) live in
// the backend implementation (simulator_backend.hpp).
std::vector<std::pair<StateIndex, std::complex<double>>>
QRegister::get_statevector_nonzero() const {
    return backend_->nonzero_amplitudes();
}

// Single amplitude. Pure delegation: the index convention is
// the backend's, and routing it through here would only add a second place for
// that convention to drift.
std::complex<double> QRegister::amplitude(StateIndex basis) const {
    return backend_->amplitude(basis);
}

// Reduced density matrix. TensorNetwork only; the other
// backends throw with a pointer at get_statevector().
std::vector<std::complex<double>>
QRegister::reduced_density_matrix(std::vector<QubitIndex> qubits) const {
    return backend_->reduced_density_matrix(std::move(qubits));
}

// Dump the current statevector in the oracle.data text format.
// Per line:
//   [P=%.6f] [%+.8f, %+.8f] |<bitstring>>
// with P = re*re + im*im and the bitstring MSB->LSB (q[n-1]..q[0]), so the
// acceptance runner's parse_data_file() reads the dump back unchanged. The
// %.8f precision (1e-8 absolute) clears the per-amplitude 1e-7 tolerance.
// Uses nonzero_amplitudes() + num_qubits() instead of get_statevector() to
// avoid densifying — SparseState now works beyond MAX_DENSE_EXPORT_QUBITS=25.
// Header trails the listing (same order as before; non-breaking).
void QRegister::dump_statevector(std::ostream& os) const {
    const auto entries    = backend_->nonzero_amplitudes();
    const QubitIndex nq   = backend_->num_qubits();
    const unsigned num_qubits = static_cast<unsigned>(nq);

    std::string bits(num_qubits, '0');
    char        line[96];

    for (const auto& [idx, amp] : entries) {
        const double re = amp.real();
        const double im = amp.imag();
        const double p  = re * re + im * im;

        // MSB->LSB: column k from the left maps to qubit (num_qubits-1-k).
        // idx is StateIndex (__uint128_t) — shift using StateIndex{1} so the
        // operation is 128-bit and defined for all qubit counts up to 128.
        for (unsigned k = 0; k < num_qubits; ++k) {
            const unsigned shift = num_qubits - 1u - k;
            bits[k] = ((idx >> shift) & StateIndex{1}) ? '1' : '0';
        }

        std::snprintf(line, sizeof(line),
                      "[P=%.6f] [%+.8f, %+.8f] |", p, re, im);
        os << line << bits << ">\n";
    }

    // Header trails the listing and reports the count of emitted (nonzero) states.
    os << "============== dump " << entries.size() << " states =============\n";
}

// Convenience overload -> stdout (state-access family).
void QRegister::dump_statevector() const {
    dump_statevector(std::cout);
}

// Convenience delegate -- QRegister does not own a circuit.
std::string QRegister::draw(const Circuit& circuit, CircuitDrawStyle style) const {
    return circuit.draw(style);
}

// ── Utilities ──────────────────────────────────────────────────
void QRegister::set_noise_model(const NoiseModel& model) {
    noise_model_     = model;
    has_noise_model_ = true;
}

// noise_model() — read-only view. Deliberately does not touch has_noise_model_ so
// verification harnesses can inspect the model without changing execution
// routing.
const NoiseModel& QRegister::noise_model() const {
    return noise_model_;
}

// mutable_noise_model() — mutable access for registering manual channels on top
// of a device model. Arms the noisy path on call (conservative: we do not track
// whether the caller actually registered anything through the reference).
NoiseModel& QRegister::mutable_noise_model() {
    has_noise_model_ = true;
    return noise_model_;
}

// The thread count the OpenMP runtime started with (OMP_NUM_THREADS or its own
// default). Captured on first use, which is read before this call changes
// anything, so it survives later set_omp_threads(n) calls. omp_get_max_threads()
// alone cannot serve as "the default": after set_omp_threads(8) it returns 8, so
// a subsequent restore would pin 8 forever instead of the original value.
static int initial_omp_max_threads() {
    static const int initial = omp_get_max_threads();
    return initial;
}

// Sets the OpenMP thread count for the backend's parallel kernels.
// n <= 0 restores the runtime's original default. The previous implementation
// clamped to 1, which serialised every kernel — the exact opposite of the
// documented contract and a silent ~100x slowdown on this class of machine
// (audit #11). Passing a non-positive value to omp_set_num_threads() is
// undefined, so the default is resolved explicitly rather than forwarded.
// TensorNetwork noise route and truncation knobs. All three forward
// to backend virtuals whose default implementations are no-ops, so the facade's
// job is to name the route, reject a route it does not know, and say out loud
// when the current backend cannot honour the call. Range validation stays with
// the backend config types — duplicating the bounds
// here would give two places to change when a ceiling moves.
namespace {

// The backend's own literal, not a new vocabulary: SimulatorError already
// identifies backends by these strings.
const char* sim_type_literal(SimType sim) {
    switch (sim) {
        case SimType::FullState:      return "FullState";
        case SimType::SparseState:    return "SparseState";
        case SimType::TensorNetwork:  return "TensorNetwork";
    }
    return "unknown";
}

// Warned per call rather than once per process: a sweep over backends should
// hear it for each knob it set on the wrong one.
void warn_knob_ignored(const char* what, SimType sim) {
    log::warn(std::string(what) + " applies to the TensorNetwork backend only; "
              "this QRegister holds " + sim_type_literal(sim) +
              " and the call had no effect");
}

}  // namespace

void QRegister::set_tn_noise_path(std::string_view path) {
    // Validated before the backend check so a typo is caught even on a backend
    // that would have ignored the value — otherwise the bug hides until someone
    // runs the same script on TensorNetwork.
    bool use_mpo = false;
    if (path == "mc") {
        use_mpo = false;
    } else if (path == "mpo") {
        use_mpo = true;
    } else {
        throw CircuitError{"set_tn_noise_path: unknown route '" + std::string(path) +
                           "' (expected \"mc\" or \"mpo\")"};
    }
    if (sim_type_ != SimType::TensorNetwork) {
        warn_knob_ignored("set_tn_noise_path", sim_type_);
        return;
    }
    backend_->set_tn_use_mpo(use_mpo);
}

void QRegister::set_noise_interventions(std::vector<std::uint32_t> sites) {
    std::sort(sites.begin(), sites.end());
    sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
    noise_interventions_ = std::move(sites);
}

const std::vector<std::uint32_t>& QRegister::noise_interventions() const noexcept {
    return noise_interventions_;
}

void QRegister::set_noise_draw_ledger(bool on) {
    noise_draw_ledger_ = on;
}

void QRegister::set_mps_truncation(std::uint32_t max_bond, double cutoff) {
    if (sim_type_ != SimType::TensorNetwork) {
        warn_knob_ignored("set_mps_truncation", sim_type_);
        return;
    }
    backend_->set_mps_truncation(max_bond, cutoff);
}

void QRegister::set_mpo_truncation(std::uint32_t max_bond, double cutoff) {
    if (sim_type_ != SimType::TensorNetwork) {
        warn_knob_ignored("set_mpo_truncation", sim_type_);
        return;
    }
    backend_->set_mpo_truncation(max_bond, cutoff);
}

void QRegister::set_omp_threads(int n) {
    const int fallback = initial_omp_max_threads();  // read before we change it
    const int threads  = (n > 0) ? n : fallback;
    omp_set_num_threads(threads);
    log::info("OpenMP threads set to " + std::to_string(threads) +
              (n > 0 ? "" : " (OpenMP default restored)"));
}

// Reports the same verdict resolve_proc() acts on: GPU build + at least one
// visible CUDA device. The Phase 0-S placeholder returned a
// hardcoded false long after the probe existed in factory.cpp, so this public
// query answered the opposite of what the constructor had actually chosen —
// ProcType::Auto selected GPU while gpu_available() said there was none
// (audit #6 Context).
// Permanent swap to a CPU FullState backend for Interactive. Mirrors
// the run() fallback: rebuild, rebind, re-apply the seed, so every later
// state access observes one backend and never a stale GPU state.
void QRegister::switch_to_cpu_backend_for_interactive() {
    log::warn("Interactive mode is CPU-only; switching the auto-selected GPU "
              "backend to FullState CPU. Construct with "
              "ProcType::CPU to select it explicitly.");
    backend_ = make_backend(SimType::FullState, ProcType::CPU);
    interactive.bind_backend(backend_.get());
    backend_->set_seed(master_seed_);
}

bool QRegister::gpu_available() {
    return qplayer::detail::gpu_runtime_available();
}

SimType QRegister::sim_type() const {
    return sim_type_;
}

// max_qubits() — largest no-swap qubit count, delegated to the bound backend
// Uses the Execute working set (2x base) since running a circuit
// is the binding constraint; get_statevector() only adds 1x and is looser.
// Delegation means the answer tracks the actual backend instance: FullState CPU
// reports the host peak budget, FullState GPU reports min(VRAM, host), and a GPU
// request that fell back to CPU naturally yields the CPU value.
// Sparse and TN report their structural ceiling (128) instead: neither holds
// 2^n amplitudes, so the dense budget the formula computes describes nothing
// they do.
std::uint32_t QRegister::max_qubits() const noexcept {
    return backend_->max_qubits(qplayer::detail::WorkingSet::Execute);
}

// ── Seed control ────────────────────────────────────
// QPLAYER_SEED takes precedence over set_seed(); when locked, set_seed() is
// ignored and warns exactly once (priority table).
void QRegister::set_seed(std::uint64_t seed) noexcept {
    if (env_seed_locked_) {
        if (!set_seed_warned_) {
            log::warn("QPLAYER_SEED env is set; set_seed() ignored");
            set_seed_warned_ = true;
        }
        return;
    }
    master_seed_ = seed;
    // Propagate to the backend immediately so the interactive path (which never
    // calls run()) draws reproducible measurement RNG from master_seed_. run()
    // re-applies this before execute(), so batch behaviour is unchanged
    // (idempotent). /.
    backend_->set_seed(master_seed_);
    log::info("master seed = " + std::to_string(master_seed_) +
              " (source: user)");
}

std::uint64_t QRegister::current_seed() const noexcept {
    return master_seed_;
}

}  // namespace qplayer
