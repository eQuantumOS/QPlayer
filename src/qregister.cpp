// qregister.cpp — Phase 0-S implementation of the sole user-facing entry
// point.
//
// Phase 0-S responsibilities:
//   1. Construct the backend via make_backend() and bind it to the Interactive
//      member. Backend constructors are trivial (no resource allocation), so
//      this path stays exception-free even though every prepare/execute call
//      later throws NotImplementedError.
// 2. Provide trivial seed accessors. std::random_device seeds
//      master_seed_ on construction; set_seed() overrides it explicitly.
//   3. Forward every Phase-3+ API to NotImplementedError with the appropriate
//      phase tag so callers and Python bindings see a uniform stub message.
//   4. sim_type()/result()/gpu_available() are real because tests can inspect
//      them right away.

#include "qregister.hpp"

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
    : backend_(make_backend(sim_type, proc, /*device_model=*/nullptr)),
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
// IdleNoiseInserterPass can build per-qubit Kraus channels.
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
// Propagates the master seed and runtime fusion config to
// the backend, then runs the / pipeline. The returned
// Result is cached so result() can re-expose it. NoiseModel is forwarded only
// when set; FullState ignores it until Phase 8.
Result QRegister::run(const Circuit& circuit,
                      std::uint32_t  nshots,
                      bool           enable_fusion,
                      std::uint32_t  fusion_max_width) {
    const NoiseModel* noise = has_noise_model_ ? &noise_model_ : nullptr;

    // Dense-space noise handoff. When transpile()
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

    // accumulate pre-registered expectation targets into the
    // Result. Fast path only in Phase 5 — after execute() the backend
    // state is |ψ_final⟩, so backend_->expectation_value() is well-defined.
    // Per-shot / noisy accumulation (nshot-average) is wired in Phase 8.
    const auto& targets = circuit.expectation_targets();
    if (!targets.empty()) {
        if (!circuit.has_classical_control() && !has_noise_model_) {
            for (const auto& pauli : targets) {
                // QRegister is a friend of Result — fill the dict.
                last_result_.expectations_[pauli] = backend_->expectation_value(pauli);
            }
        } else {
            log::warn("expectation targets registered but accumulation on the "
                      "per-shot/noisy path is deferred to Phase 8; skipped");
        }
    }
    return last_result_;
}

const Result& QRegister::result() const {
    return last_result_;
}

// ── State access ──────────────────
// Thin delegation to the active backend. Per-backend support + error policy
// (e.g. SparseState raw always throws SimulatorError, >MAX_STATEVECTOR_QUBITS
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

// Dump the current statevector in the oracle.data text format.
// Per line:
//   [P=%.6f] [%+.8f, %+.8f] |<bitstring>>
// with P = re*re + im*im and the bitstring MSB->LSB (q[n-1]..q[0]), so the
// acceptance runner's parse_data_file() reads the dump back unchanged. The
// %.8f precision (1e-8 absolute) clears the per-amplitude 1e-7 tolerance.
// Uses nonzero_amplitudes() + num_qubits() instead of get_statevector() to
// avoid densifying — SparseState now works beyond MAX_STATEVECTOR_QUBITS=25.
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

// Sets the OpenMP thread count for the backend's parallel kernels.
// n <= 0 restores OpenMP's default (omp_set_num_threads ignores non-positive on
// some runtimes, so we clamp to >=1 and log the effective value).
void QRegister::set_omp_threads(int n) {
    const int threads = (n > 0) ? n : 1;
    omp_set_num_threads(threads);
    log::info("OpenMP threads set to " + std::to_string(threads));
}

bool QRegister::gpu_available() {
    // Phase 0-S: no CUDA runtime probe yet (Phase 6 wires it up via
    // cudaGetDeviceCount). The conservative default is false so call sites
    // that gate on gpu_available() pick the CPU path during early bring-up.
    return false;
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
// Sparse/TN return a dense-equivalent advisory (they hold far more sparsely).
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
