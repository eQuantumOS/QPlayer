// qregister.hpp — Sole user-facing entry point.
//
// QRegister is both:
//   1. The Batch-mode driver: parse() / transpile() / run().
//   2. The Interactive-mode host: the `interactive` member is an Interactive
// instance whose private constructor is friended to QRegister.
//
// This is the only public header that includes circuit/interactive.hpp.

#pragma once

#include <complex>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "types.hpp"
#include "result.hpp"
#include "log.hpp"
#include "observable.hpp"
#include "circuit/circuit.hpp"
#include "circuit/interactive.hpp"
#include "noise/noise_model.hpp"
#include "noise/device_model.hpp"
#include "backend/simulator_backend.hpp"     // SimType + ProcType + SimulatorBackend
#include "backend/memory_check.hpp"
#include "parser/parser.hpp"                 // pulled so include/qregister.hpp acts as the umbrella

namespace qplayer {

class QRegister {
public:
    // ── Interactive surface (immediate dispatch) ─────────
    // Public *member* (not pointer / reference) so the natural Python /
    // C++ call site `qr.interactive.h(0)` mirrors the design doc verbatim.
    // The Interactive private constructor + friend QRegister keeps users
    // from constructing it standalone.
    Interactive interactive;

    // ── Construction ───────────────────────────────────────────────
    // ProcType::Auto -> GPU when CUDA is detected, CPU fallback otherwise
    // The constructor calls make_backend() then binds
    // the backend pointer into `interactive` via Interactive::bind_backend().
    explicit QRegister(SimType  sim_type = SimType::FullState,
                       ProcType proc     = ProcType::Auto);
    ~QRegister();

    // Non-copyable — owns a unique_ptr<SimulatorBackend>. Move support could
    // be added later but is unnecessary for the single-entry-point pattern.
    QRegister(const QRegister&)            = delete;
    QRegister& operator=(const QRegister&) = delete;

    // ── Device model ingestion (optional) ──────────────────────────
    // Side-effect persistence: stored as a member; subsequent transpile()
    // calls fall back to this DeviceModel when device_json is empty
    // (truth table).
    void load_device_model(std::string_view json_path);

    // ── Input path 1: OpenQASM v2 parser ───────────────────────────
    Circuit parse     (std::string_view qasm_text);
    Circuit parse_file(std::string_view file_path);

    // ── Compile (single entry point partial supersede) ─
    // optimization_level: 0 (validator only) | 1 (default) |
    // 2 / 3 (TBD — Phase 4 entry gate).
    // device_json: empty / invalid -> skip layout / routing / scheduling
    //   stages and fall back to the member DeviceModel (if any).
    //   Both paths missing -> skip device-dependent stages entirely.
    Circuit transpile(Circuit          circuit,
                      int              optimization_level = 1,
                      std::string_view device_json        = "");

    // ── Run (Batch execution) ────────────────────────────
    // enable_fusion / fusion_max_width drive the runtime Gate Fusion stage
    // SparseState ignores both flags (no fusion).
    // Calling run() invalidates the `interactive` active state.
    Result run(const Circuit&    circuit,
               std::uint32_t     nshots           = 1024,
               bool              enable_fusion    = true,
               std::uint32_t     fusion_max_width = 4);

    // Last completed Result (from either run() or interactive.result()).
    const Result& result() const;

    // ── State access ─────────────────
    // Per-backend behaviour: see comment block.
    StateVector  get_statevector()                              const;
    RawStateView get_statevector_raw()                          const;
    // pauli defaults to "" -> all-Z over every qubit (<Z...Z>); the backend
    // builds the all-Z string from its num_qubits.
    double       expectation_value(std::string_view pauli = "")  const;
    // Weighted Pauli-sum expectation Sum(c_i * <P_i>).
    // Post-hoc on the current backend state; each term delegates to the
    // pauli overload (which validates length==num_qubits at this point).
    // An empty Observable returns 0.0. The Estimator facade wraps run()+this.
    double       expectation_value(const Observable& obs)        const;

    // Enumerate nonzero-amplitude basis states as (index, amplitude) pairs in
    // ascending StateIndex order without densifying. SparseState works
    // up to 128 qubits; FullState scans its dense array; TensorNetwork throws
    // SimulatorError. Returns an empty vector before init_state() is called.
    std::vector<std::pair<StateIndex, std::complex<double>>>
            get_statevector_nonzero() const;

    // Human-readable statevector dump in the oracle .data text format: a header
    // line plus one line per basis state — "[P=%.6f] [%+.8e, %+.8e] |<bits>>"
    // where P = |amp|^2 and <bits> is MSB->LSB (q[n-1]..q[0]). Mirrors
    // tests/data/qasm/expected/*.data so a dump can seed an acceptance baseline.
    // Extends the state-access family; the no-arg overload -> stdout.
    void dump_statevector(std::ostream& os) const;
    void dump_statevector()                 const;

    // ── Circuit diagram ────────────────────────
    // Convenience delegate: QRegister does not retain a circuit (run() takes it
    // by argument), so the circuit is passed in and forwarded to Circuit::draw.
    std::string   draw(const Circuit& circuit,
                       CircuitDrawStyle style = CircuitDrawStyle::Unicode) const;

    // ── Utilities ──────────────────────────────────────────────────
    void          set_noise_model (const NoiseModel& model);
    void          set_omp_threads (int n);
    static bool   gpu_available();
    SimType       sim_type() const;

    // Largest qubit count a FullState dense run fits in the no-swap memory
    // budget (peak working set, Execute = 2x). Derived from
    // min(/proc/meminfo MemAvailable, cgroup limit) x 0.8 or
    // QPLAYER_MEMORY_BUDGET_GB. Sparse/TN backends are not bounded by 2^n, so
    // for them this is a dense-equivalent lower-bound advisory only.
    std::uint32_t max_qubits() const noexcept;

    // ── Seed control ───────────────────────────
    // QPLAYER_SEED env overrides set_seed() and emits a warn() once
    // (table).
    void          set_seed    (std::uint64_t seed) noexcept;
    std::uint64_t current_seed() const               noexcept;

    // Intentionally absent (superseded by):
    //   - optimize() / compile()              -> transpile() with level argument
    //   - register_opaque / has_opaque        -> Circuit::define_opaque()
    //   - unregister_opaque                   -> rebuild Circuit instead

private:
    std::unique_ptr<SimulatorBackend> backend_;
    SimType                           sim_type_  = SimType::FullState;
    ProcType                          proc_type_ = ProcType::Auto;

    // Cached device + noise models (transpile fallback path).
    std::unique_ptr<DeviceModel>      device_model_;
    NoiseModel                        noise_model_;
    bool                              has_noise_model_ = false;

    // PRNG master seed. std::random_device fills the initial
    // value on construction; set_seed() / QPLAYER_SEED override it.
    std::uint64_t                     master_seed_ = 0;

    // QPLAYER_SEED env lock: when the env var supplied a valid
    // seed at construction, set_seed() is ignored and warns exactly once.
    bool                              env_seed_locked_ = false;
    bool                              set_seed_warned_ = false;

    Result                            last_result_;
};

}  // namespace qplayer
