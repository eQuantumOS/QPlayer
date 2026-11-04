/// @file qregister.hpp
/// @brief Sole user-facing entry point.
/// @ingroup api
///
/// QRegister is both:
///   1. The Batch-mode driver: parse() / transpile() / run().
///   2. The Interactive-mode host: the `interactive` member is an Interactive
/// instance whose private constructor is friended to QRegister.
///
/// This is the only public header that includes circuit/interactive.hpp.
/// CLAUDE.md forbids user code from including interactive.hpp directly.

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

/// The single public entry point: owns a simulator backend and drives both
/// Batch execution (parse -> transpile -> run) and Interactive dispatch.
class QRegister {
public:
    // ── Interactive surface (immediate dispatch) ─────────
    /// Immediate-dispatch surface.
    ///
    /// A public *member* (not pointer / reference) so the natural Python /
    /// C++ call site `qr.interactive.h(0)` mirrors the design doc verbatim.
    /// The Interactive private constructor + friend QRegister keeps users
    /// from constructing it standalone.
    Interactive interactive;

    // ── Construction ───────────────────────────────────────────────
    /// Constructs a register bound to a freshly built backend.
    ///
    /// ProcType::Auto -> GPU when CUDA is detected, CPU fallback otherwise
    /// The constructor calls make_backend() then binds
    /// the backend pointer into `interactive` via Interactive::bind_backend().
    /// @param sim_type Simulator family to instantiate.
    /// @param proc     Processor selection; Auto resolves at construction.
    explicit QRegister(SimType  sim_type = SimType::FullState,
                       ProcType proc     = ProcType::Auto);
    /// Releases the backend and any state it still owns.
    ~QRegister();

    /// @name Non-copyable
    /// Owns a unique_ptr<SimulatorBackend>. Move support could be added later
    /// but is unnecessary for the single-entry-point pattern.
    /// @{

    /// Copy construction is deleted.
    QRegister(const QRegister&)            = delete;
    /// Copy assignment is deleted.
    QRegister& operator=(const QRegister&) = delete;
    /// @}

    // ── Device model ingestion (optional) ──────────────────────────
    /// Loads a device model (coupling map, gate durations, error rates) from JSON.
    ///
    /// Side-effect persistence: stored as a member; subsequent transpile()
    /// calls fall back to this DeviceModel when device_json is empty
    /// (truth table).
    /// @param json_path Filesystem path to the device JSON.
    void load_device_model(std::string_view json_path);

    // ── Input path 1: OpenQASM v2 parser ───────────────────────────
    /// Parses OpenQASM v2 source text into a Circuit IR.
    /// @param qasm_text OpenQASM v2 source.
    /// @return The built circuit.
    /// @throws QASMParseError on lexical, syntactic, or semantic failure.
    Circuit parse     (std::string_view qasm_text);
    /// Parses an OpenQASM v2 file into a Circuit IR.
    /// @param file_path Path to the .qasm file.
    /// @return The built circuit.
    /// @throws QASMParseError on lexical, syntactic, or semantic failure.
    Circuit parse_file(std::string_view file_path);

    // ── Compile (single entry point partial supersede) ─
    /// Runs the pass pipeline over a circuit — the single compile entry point.
    ///
    /// @param circuit            Circuit to compile (taken by value, returned transformed).
    /// @param optimization_level 0 (validator only) | 1 (default F-OPT) |
    /// 2 / 3 (TBD — Phase 4 entry gate).
    /// @param device_json        Empty / invalid -> skip layout / routing / scheduling
    ///                           stages and fall back to the member DeviceModel (if any).
    ///                           Both paths missing -> skip device-dependent stages entirely.
    /// @return The compiled circuit.
    Circuit transpile(Circuit          circuit,
                      int              optimization_level = 1,
                      std::string_view device_json        = "");

    // ── Run (Batch execution) ────────────────────────────
    /// Executes a circuit in Batch mode and samples measurement outcomes.
    ///
    /// Calling run() invalidates the `interactive` active state.
    /// @param circuit          Circuit to execute.
    /// @param nshots           Number of measurement shots.
    /// @param enable_fusion Drives the runtime Gate Fusion stage.
    ///                         SparseState ignores this flag (no fusion).
    /// @param fusion_max_width Maximum fused window width. FullState GPU honours it on
    ///                         the noiseless fast path but caps at 4 and clamps above
    /// that; FullState CPU caps at 8.
    /// @return Execution result holding counts, and the final state when the
    ///         backend took a fast path that preserves one.
    Result run(const Circuit&    circuit,
               std::uint32_t     nshots           = 1024,
               bool              enable_fusion    = true,
               std::uint32_t     fusion_max_width = 4);

    /// Last completed Result (from either run() or interactive.result()).
    /// @return Reference to the retained result.
    const Result& result() const;

    // ── State access ─────────────────
    /// Dense AoS copy of the current state vector. Per-backend behaviour: see
    /// comment block.
    /// @return Amplitudes indexed by basis state.
    StateVector  get_statevector()                              const;
    /// Borrowed SoA view of the current state vector — no AoS conversion cost.
    /// @return View valid until the next operation mutates the backend state.
    RawStateView get_statevector_raw()                          const;
    /// Expectation value of a Pauli string over the current state.
    ///
    /// @param pauli Pauli string; defaults to "" -> all-Z over every qubit
    ///              (<Z...Z>). The backend builds the all-Z string from its
    /// num_qubits.
    /// @return The expectation value in [-1, 1].
    double       expectation_value(std::string_view pauli = "")  const;
    /// Weighted Pauli-sum expectation Sum(c_i * <P_i>).
    ///
    /// Post-hoc on the current backend state; each term delegates to the
    /// pauli overload (which validates length==num_qubits at this point).
    /// An empty Observable returns 0.0. The Estimator facade wraps run()+this.
    /// @param obs Weighted Pauli-sum observable to evaluate.
    /// @return The weighted sum of per-term expectation values.
    double       expectation_value(const Observable& obs)        const;

    /// Enumerates nonzero-amplitude basis states as (index, amplitude) pairs in
    /// ascending StateIndex order without densifying.
    ///
    /// SparseState works up to 128 qubits; FullState scans its dense array;
    /// TensorNetwork throws SimulatorError. Returns an empty vector before
    /// init_state() is called.
    /// @return (basis index, amplitude) pairs in ascending index order.
    /// Amplitude of a single basis state.
    ///
    /// Same index space as get_statevector() and get_statevector_nonzero(): bit
    /// q is qubit q. On TensorNetwork this sweeps the MPS chain in O(n*chi^2) and
    /// works past MAX_DENSE_EXPORT_QUBITS — for a TN circuit wider than 25 qubits
    /// it is the only way to inspect the state at all.
    /// @param basis Basis-state index; must be < 2^num_qubits.
    /// @return The complex amplitude.
    /// @throws SimulatorError basis out of range, or no state prepared.
    std::complex<double> amplitude(StateIndex basis) const;

    /// Reduced density matrix over a qubit subset.
    ///
    /// TensorNetwork only. Dense backends throw with a message pointing at
    /// get_statevector(), from which a partial trace is a short user-side loop
    ///
    /// @param qubits Subset to keep; duplicates rejected, size capped.
    /// @return Row-major 2^k x 2^k matrix; bit i (LSB) is the i-th smallest qubit.
    /// @throws SimulatorError unsupported backend, bad qubit list, or subset too large.
    std::vector<std::complex<double>>
        reduced_density_matrix(std::vector<QubitIndex> qubits) const;

    std::vector<std::pair<StateIndex, std::complex<double>>>
            get_statevector_nonzero() const;

    /// Human-readable statevector dump in the oracle .data text format.
    ///
    /// Emits one line per NONZERO basis state — `[P=%.6f] [%+.8f, %+.8f] |\<bits\>>`
    /// where P = |amp|^2 and `\<bits\>` is MSB->LSB (q[n-1]..q[0]) — followed by a
    /// TRAILING header line "===== dump <count> states =====".
    ///
    /// @warning This is NOT the acceptance baseline layout (audit #8: the doc used
    /// to claim it mirrored tests/data/qasm/expected/*.data, and was wrong on three
    /// counts). Those .data files are header-FIRST, list ALL 2^n states including
    /// zeros, and use %+.12e. run_acceptance.cpp's parse_data_file() skips until it
    /// sees the '=' header, so feeding it a dump — whose header comes last — parses
    /// zero amplitudes. The formats diverged deliberately: dump_statevector() was
    /// rebuilt on nonzero_amplitudes() so SparseState circuits beyond
    /// MAX_DENSE_EXPORT_QUBITS=25 can be dumped without densifying.
    ///
    /// Extends the state-access family; the no-arg overload -> stdout.
    /// @param os Stream to write the dump to.
    void dump_statevector(std::ostream& os) const;
    /// Writes the same dump as dump_statevector(std::ostream&) to stdout.
    void dump_statevector()                 const;

    // ── Circuit diagram ────────────────────────
    /// Renders a text circuit diagram.
    ///
    /// Convenience delegate: QRegister does not retain a circuit (run() takes it
    /// by argument), so the circuit is passed in and forwarded to Circuit::draw.
    /// @param circuit Circuit to render.
    /// @param style   Character set used for the diagram.
    /// @return The rendered diagram.
    std::string   draw(const Circuit& circuit,
                       CircuitDrawStyle style = CircuitDrawStyle::Unicode) const;

    // ── Utilities ──────────────────────────────────────────────────
    /// Replaces the whole noise model.
    /// @param model Noise model to install, discarding any previous one.
    void          set_noise_model (const NoiseModel& model);

    /// Read-only noise model accessor.
    ///
    /// Read-only and does NOT arm the noisy execution path. That split matters:
    /// a single mutable accessor would let an inspection-only call route an empty
    /// model through the Monte Carlo path (tens to hundreds of times slower than
    /// the fast path).
    /// @return Reference to the installed model; inspection does not arm noise.
    const NoiseModel& noise_model()         const;

    /// Mutable noise model accessor — arms the noisy execution path on call
    /// because merely holding the reference is taken as intent
    /// apply noise.
    ///
    /// load_device_model() clears previously registered channels ( idempotent
    /// reload) and set_noise_model() replaces the whole model, so there was no way
    /// to put manual Kraus channels on top of a device model through QRegister even
    /// though NoiseModel itself supports it (audit #39). Order matters:
    /// @code
    ///     load_device_model(path);
    ///     mutable_noise_model().add_kraus(...);   // AFTER the device load
    /// @endcode
    /// Reversing the two loses the manual channels to load_device()'s clear.
    /// @return Mutable reference to the installed model; the call itself arms the
    ///         noisy execution path.
    NoiseModel&       mutable_noise_model();

    /// Sets the OpenMP thread count used by CPU backends.
    /// @param n Thread count to request from OpenMP.
    /// @name TensorNetwork noise route and truncation knobs
    /// All three are TensorNetwork-only: on another backend the call warns once
    /// and does nothing, because the backend virtuals default to no-ops
    /// (the set_fusion / set_cache_blocking pattern). Precedence is
    /// defaults -> QPLAYER_* env -> these setters, last one winning (D).
    ///@{

    /// Selects the noise route: "mc" (default, sampled trajectories) or
    /// "mpo" (density matrix, exact in the channel). Shares its vocabulary with
    /// QPLAYER_TN_NOISE_PATH so one concept does not carry two spellings.
    ///
    /// An env typo warns and keeps MC (E), but a bad argument here is a
    /// bug in the calling code and running different physics silently is this
    /// switch's worst failure mode — so this one throws.
    /// @param path Route name, "mc" or "mpo".
    /// @throws CircuitError The string is neither "mc" nor "mpo".
    void          set_tn_noise_path(std::string_view path);

    /// Caps the MPS bond dimension and the SVD cutoff for the sampled route.
    /// @param max_bond Bond dimension ceiling (the resource axis B).
    /// @param cutoff   Relative discarded-tail bound (the accuracy axis).
    /// @throws CircuitError max_bond is 0 or above kMpsMaxBondCeiling, or cutoff
    ///   is negative — raised by the backend, so only on a TensorNetwork register.
    void          set_mps_truncation(std::uint32_t max_bond, double cutoff);

    /// Same two axes for the density-matrix route, kept separate because all
    /// five quantities differ from the MPS ones (A).
    /// @param max_bond Bond dimension ceiling for rho.
    /// @param cutoff   Relative Frobenius-tail bound, normalised by ||rho||_F.
    /// @throws CircuitError max_bond is 0 or above kMpoMaxBondCeiling, or cutoff
    ///   is negative — raised by the backend, so only on a TensorNetwork register.
    void          set_mpo_truncation(std::uint32_t max_bond, double cutoff);

    ///@}

    /// @name Noise-trajectory counterfactual interventions
    ///
    /// A site is one uniform a noise trajectory consumes because of the
    /// NoiseModel, numbered per shot in canonical execution order. An intervened
    /// site still consumes its draw and skips only the channel, so every later
    /// draw is unchanged. Kept here and handed to the executing backend on every
    /// run(), like the seed; Interactive mode is out of scope.
    ///@{

    /// Replaces the intervention list for subsequent run() calls.
    /// @param sites Site numbers to replace by the identity; sorted and
    ///   de-duplicated on entry. An empty list means no intervention.
    /// @throws SimulatorError at run() when the list is non-empty and no noise
    ///   model is set, when the register is SparseState, when a site is beyond
    ///   the draws the run made, or on the TensorNetwork MPO route.
    void set_noise_interventions(std::vector<std::uint32_t> sites);

    /// The intervention list run() will apply.
    /// @return Sorted, duplicate-free site numbers.
    const std::vector<std::uint32_t>& noise_interventions() const noexcept;

    /// Turns the per-shot draw ledger (Result::noise_draw_ledger) on or off.
    /// @param on Record every site's uniform, branch and op; default off, and
    ///   off costs nothing.
    void set_noise_draw_ledger(bool on);

    ///@}

    void          set_omp_threads (int n);
    /// True when a usable CUDA device is present.
    /// @return true when a CUDA device is visible to this process.
    static bool   gpu_available();
    /// Simulator family this register was constructed with.
    /// @return The SimType passed to the constructor.
    SimType       sim_type() const;

    /// Largest qubit count this register's backend accepts.
    ///
    /// The bound's SOURCE differs by backend, which is the whole content of the
    /// answer: FullState is limited by memory — min(/proc/meminfo MemAvailable,
    /// cgroup limit) x 0.8 or QPLAYER_MEMORY_BUDGET_GB, peak working set with
    /// Execute = 2x — while SparseState and TensorNetwork are limited by
    /// structure and report 128, the width StateIndex can index
    /// Neither structural answer promises a given circuit fits:
    /// sparse cost follows the non-zero count and TN cost follows the bond
    /// dimension, and both are decided by the circuit rather than by its width.
    /// @return Largest qubit count the backend accepts.
    std::uint32_t max_qubits() const noexcept;

    // ── Seed control ───────────────────────────
    /// Sets the PRNG master seed.
    ///
    /// The QPLAYER_SEED env var overrides set_seed() and emits a warn() once
    /// (table).
    /// @param seed Master seed for every RNG stream this register drives.
    void          set_seed    (std::uint64_t seed) noexcept;
    /// Currently active master seed.
    /// @return The seed in effect, including one supplied via QPLAYER_SEED.
    std::uint64_t current_seed() const               noexcept;

    // Intentionally absent (superseded):
    //   - optimize() / compile()              -> transpile() with level argument
    //   - register_opaque / has_opaque        -> Circuit::define_opaque()
    //   - unregister_opaque                   -> rebuild Circuit instead

private:
    // Interactive::initialize() calls switch_to_cpu_backend_for_interactive()
    // when Auto resolved to GPU. Only QRegister owns the backend, so
    // the swap has to live here.
    friend class Interactive;

    // Rebuilds backend_ as FullState CPU, rebinds `interactive`, and re-applies
    // the master seed. Permanent, mirroring the run() fallback so later
    // state access observes one backend.
    void switch_to_cpu_backend_for_interactive();

    std::unique_ptr<SimulatorBackend> backend_;
    SimType                           sim_type_  = SimType::FullState;
    ProcType                          proc_type_ = ProcType::Auto;

    // Cached device + noise models (transpile fallback path).
    std::unique_ptr<DeviceModel>      device_model_;
    NoiseModel                        noise_model_;
    bool                              has_noise_model_ = false;

    // Noise-site interventions and draw ledger, pushed per run().
    std::vector<std::uint32_t>        noise_interventions_;
    bool                              noise_draw_ledger_ = false;

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
