/// @file simulator_backend.hpp
/// @brief Abstract simulator interface.
/// @ingroup backend
///
/// All four backends (FullState CPU / FullState GPU / SparseState / TensorNetwork)
/// derive from this class. The QRegister facade owns a unique_ptr<SimulatorBackend>
/// produced by make_backend().

#pragma once

#include <complex>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "types.hpp"
#include "backend/memory_check.hpp"   // detail::WorkingSet for max_qubits()
#include "circuit/gate_op.hpp"
#include "noise/noise_model.hpp"
#include "noise/device_model.hpp"
#include "result.hpp"

namespace qplayer {

/// Mirrors the user-facing enum in qregister.hpp so the factory + backend
/// metadata can self-identify without dragging the QRegister header here.
/// SSOT: :247 + backend factory pattern.
/// GPU dispatch is driven by ProcType, not a separate SimType value -- see
/// make_backend() in factory.cpp.
enum class SimType {
    FullState,
    SparseState,
    TensorNetwork,
};

/// Processor the backend runs on; Auto resolves at construction.
/// @ingroup backend
enum class ProcType {
    CPU,
    GPU,
    Auto,
};

class Circuit;  // Forward declaration — full definition pulled by .cpp files.

/// Shared lower bound on |amp| for a basis state to count as "nonzero" in
/// nonzero_amplitudes(). Unifies the filter across backends: FullState
/// scans its dense array for |amp| >= this value (dropping ~1e-17 roundoff dust),
/// and SparseState's default truncation threshold is initialised
/// it, so the same circuit yields an identical nonzero set on every backend.
constexpr double kNonzeroAmplitudeThreshold = 1e-12;

/// Abstract backend interface every simulator implements.
/// @ingroup backend
class SimulatorBackend {
public:
    virtual ~SimulatorBackend() = default;

    /// ── Batch mode (QRegister::run path) ───────────────────────────
    /// prepare(): runtime fusion + memory pre-check.
    /// execute(): one-shot pipeline that consumes the Circuit and emits a
    ///   Result containing counts + optional statevector + truncation_errors.
    virtual void   prepare(const Circuit& circuit) = 0;
    virtual Result execute(const Circuit&     circuit,
                           std::uint32_t      nshots,
                           const NoiseModel*  noise = nullptr) = 0;

    /// ── Interactive mode (QRegister::interactive path) ───
    /// Supported by FullState (CPU / GPU) and SparseState. TensorNetwork
    /// throws SimulatorError on init_state() per the table.
    virtual void init_state  (QubitIndex num_qubits) = 0;
    virtual void apply_gate  (const GateOp& op)      = 0;
    virtual int  apply_measure(QubitIndex qubit)     = 0;
    virtual void apply_reset (QubitIndex qubit)      = 0;

    /// ── State access (Batch + Interactive) ─
    /// Per-backend support matrix:
    ///   FullState CPU/GPU : full support
    ///   SparseState       : get_statevector when n <= MAX_DENSE_EXPORT_QUBITS;
    ///                       get_statevector_raw always throws SimulatorError
    ///   TensorNetwork     : get_statevector when n <= MAX_DENSE_EXPORT_QUBITS;
    ///                       expectation_value is partial-contraction efficient
    virtual StateVector  get_statevector()                              = 0;
    virtual RawStateView get_statevector_raw()                          = 0;
    /// An empty pauli defaults to all-Z over every qubit (<Z...Z>);
    /// backends build the "ZZ..Z" string from their own num_qubits.
    virtual double       expectation_value(std::string_view pauli)      = 0;

    /// Enumerate only nonzero-amplitude basis states as (index, amplitude) pairs,
    /// ascending by StateIndex (reproducible). No densify — SparseState works up to
    /// 128 qubits; FullState scans its dense array; TensorNetwork throws.
    /// "Nonzero" means |amp| >= kNonzeroAmplitudeThreshold on every backend, so the
    /// nonzero set matches across backends and FP dust is excluded.
    /// Returns an empty vector before init_state() is called.
    virtual std::vector<std::pair<StateIndex, std::complex<double>>>
            nonzero_amplitudes() const = 0;

    /// Current active qubit count (0 before init_state). Used by dump_statevector()
    /// to format the bitstring width without densifying.
    virtual QubitIndex num_qubits() const = 0;

    /// ── Metadata ──────────────────────────────────────────────────
    virtual SimType          sim_type()       const = 0;
    virtual bool             supports_noise() const = 0;
    virtual bool             supports_gpu()   const = 0;
    /// Short backend identifier literal used by SimulatorError messages
    /// Returns one of "FullState", "FullStateGPU",
    /// "SparseState", "TensorNetwork".
    virtual std::string_view name()           const = 0;

    /// ── Backend-specific qubit ceiling ──────────────────────
    /// Largest no-swap qubit count for this backend under the given working set.
    /// FullState CPU uses the host peak model; FullState GPU returns
    /// min(VRAM limit, host pinned/AoS limit); SparseState / TensorNetwork report
    /// the dense-equivalent host bound as a guidance value. QRegister::max_qubits()
    /// delegates here so the answer follows the actual backend instance (a GPU
    /// request that fell back to CPU naturally reports the CPU value).
    virtual std::uint32_t max_qubits(detail::WorkingSet ws) const noexcept = 0;

    /// ── Seed propagation ─────────────────────────
    /// Default no-op so SparseState / TN stubs are unaffected. FullState CPU
    /// overrides to seed the internal mt19937_64 for reproducible sampling.
    virtual void set_seed(std::uint64_t /*seed*/) {}

    /// ── Runtime gate fusion config ─────
    /// QRegister::run() forwards its enable_fusion / fusion_max_width arguments
    /// here before execute(). Default no-op; SparseState ignores fusion, and
    /// FullState CPU stores the config for the fused execution path.
    virtual void set_fusion(bool /*enable*/, std::uint32_t /*max_width*/) {}

    /// ── Cache blocking toggle (benchmark) ──────────
    /// Enable or disable L2-tiled cache blocking on 1Q/2Q gate kernels.
    /// Default on for FullState CPU.
    /// Other backends ignore this call (GPU uses coalesced access instead,
    /// SparseState/TN have no cache blocking). FullState CPU overrides this.
    virtual void set_cache_blocking(bool /*enable*/) {}

    /// ── Single-amplitude and RDM queries ──
    /// Amplitude of one basis state, in the same index space as
    /// get_statevector() — bit q is qubit q. Every backend answers this in
    /// constant-to-logarithmic time (dense subscript / hash lookup / MPS sweep),
    /// which is why it sits on the shared interface rather than on TN alone.
    /// throws: SimulatorError — basis out of range, or no state prepared.
    virtual std::complex<double> amplitude(StateIndex basis) const = 0;

    /// Reduced density matrix over a qubit subset, row-major 2^k x 2^k with bit
    /// i (LSB) = the i-th smallest requested qubit. TensorNetwork only;
    /// a dense backend's user can partial-trace get_statevector() directly, so
    /// duplicating that here would only risk a convention drift.
    /// throws: SimulatorError — unsupported backend, bad qubit list, or subset
    ///   larger than the backend's cap.
    virtual std::vector<std::complex<double>>
        reduced_density_matrix(std::vector<QubitIndex> qubits) const = 0;

    /// ── MPS truncation policy ──────────────────
    /// Overrides the constexpr defaults and QPLAYER_MPS_* env values for this
    /// backend instance. Only TensorNetwork overrides it; the other backends
    /// have no bond dimension to cap. Mirrors set_fusion/set_cache_blocking so
    /// QRegister::run() keeps its signature (no per-backend argument leaks into
    /// the shared Batch API or the Python bindings).
    /// throws: CircuitError — max_bond is 0 or above kMpsMaxBondCeiling, or cutoff is negative.
    virtual void set_mps_truncation(std::uint32_t /*max_bond*/, double /*cutoff*/) {}

    /// MPO truncation knobs — deliberately separate from the MPS
    /// ones: the physical dimension, the norm, and the meaning of the error all
    /// differ, so one number cannot carry both settings.
    /// throws: CircuitError — max_bond is 0 or above kMpoMaxBondCeiling, or cutoff is negative.
    virtual void set_mpo_truncation(std::uint32_t /*max_bond*/, double /*cutoff*/) {}

    /// Selects the noise route: false = Monte Carlo trajectories (default),
    /// true = the MPO density matrix. Only TensorNetwork overrides this; run()'s
    /// signature stays unchanged. Also read
    /// QPLAYER_TN_NOISE_PATH, which this call overrides.
    virtual void set_tn_use_mpo(bool /*use_mpo*/) {}

    /// ── SparseState feasibility pre-check toggle ────────
    /// Enable/disable the pre-flight feasibility analysis (predicted non-zero
    /// count -> reject before running). Default OFF: only SparseState overrides
    /// this; other backends ignore it. Also toggled via QPLAYER_SPARSE_FEASIBILITY.
    /// The runtime guard and bad_alloc backstop stay on regardless.
    virtual void set_feasibility_check(bool /*enable*/) {}

    /// Per-site noise interventions and the draw ledger for the next execute()
    /// QRegister::run() passes them every run, like the seed.
    /// Only the backends with a noise trajectory loop (FullState CPU,
    /// TensorNetwork) override this; the default ignores it, which is safe
    /// because QRegister rejects interventions on SparseState and FullState GPU
    /// hands every noisy circuit to the CPU backend.
    /// `sites` is sorted and duplicate-free.
    virtual void set_noise_interventions(const std::vector<std::uint32_t>& /*sites*/,
                                         bool /*ledger*/) {}

protected:
    /// ── Result population helpers ──────────────────────────
    /// Derived backends call these to fill Result private fields. The friend
    /// declaration on Result allows SimulatorBackend (and its subclasses via
    /// protected access) to reach the private members (result.hpp).
    static void result_set_counts(Result& r,
                                  std::map<std::string, std::uint64_t> counts);
    static void result_set_shots (Result& r, std::uint64_t shots);
    static void result_set_statevector(Result& r, StateVector sv);
    /// nshot-averaged expectation targets (per-shot path). Only
    /// the stochastic paths call this — on the fast path a single final
    /// state exists, so QRegister::run() fills the dict from expectation_value()
    /// instead. A non-empty dict is how run() tells the two apart.
    static void result_set_expectations(Result& r,
                                        std::map<std::string, double> exp);
    /// Per-2Q-gate SVD truncation errors. TensorNetwork is
    /// the only caller; every other backend leaves the vector empty, which is
    /// what Result::has_truncation_errors() reports on.
    static void result_set_truncation_errors(Result& r, std::vector<double> errors);
    /// Per-shot TN trajectory diagnostics. TensorNetwork's
    /// MPS paths are the only callers; the three vectors must be the same length
    /// (one entry per shot) because Result reports them as one record.
    static void result_set_trajectory_diagnostics(Result& r,
                                                  std::vector<std::uint32_t> bonds,
                                                  std::vector<double> drifts,
                                                  std::vector<double> totals);
    /// Noise intervention echo and per-shot draw ledger. Called by the
    /// noise trajectory loops of FullState CPU and TensorNetwork.
    static void result_set_noise_draws(Result& r,
                                       std::vector<std::uint32_t> interventions,
                                       std::vector<std::vector<NoiseDrawEvent>> ledger);
    /// Deferred sampling snapshot. The FullState CPU/GPU fast path
    /// calls this (dense mode: counts() draws from the attached statevector_)
    /// instead of result_set_counts(); Result::counts() draws on first use.
    static void result_set_lazy_sampling(
        Result& r,
        std::uint32_t nshots,
        std::uint64_t seed,
        ClassicalBitIndex total_cubits,
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map);
    /// Sparse-mode deferred sampling. The SparseState fast path
    /// passes normalised (basis_index, prob) pairs; counts() draws from the sparse
    /// CDF without densifying (works beyond MAX_DENSE_EXPORT_QUBITS).
    static void result_set_lazy_sampling_sparse(
        Result& r,
        std::uint32_t nshots,
        std::uint64_t seed,
        ClassicalBitIndex total_cubits,
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map,
        std::vector<std::pair<StateIndex, double>> sparse_probs);
    /// GPU-resident sampling handle (Phase 7-C). The FullState GPU fast
    /// path shares ownership of the device SoA state with the Result so counts()
    /// samples on-device (no D2H) and statevector() can D2H lazily. `owner` keeps
    /// the device buffers alive past the next run()/init_state(); d_re/d_im are
    /// raw device pointers valid while owner is non-null.
    static void result_set_gpu_resident(
        Result& r,
        std::shared_ptr<void> owner,
        const double* d_re,
        const double* d_im,
        StateIndex dim);

    /// CPU-resident sampling handle (mirrors result_set_gpu_resident).
    /// The FullState CPU fast path shares ownership of the host SoA buffers with
    /// the Result so counts() draws from SoA directly (no AoS eager copy) and
    /// statevector() AoS-materialises lazily. `owner` keeps the CpuStateBuffers
    /// alive past the backend's next init_state() (P3 reuse safety).
    static void result_set_cpu_resident(
        Result& r,
        std::shared_ptr<void> owner,
        const double* re,
        const double* im,
        StateIndex dim);
};

/// Single source of truth for "is a GPU usable right now": QPLAYER_BUILD_GPU was
/// enabled at build time AND cudaGetDeviceCount() sees at least one device
/// resolve_proc(Auto) inside factory.cpp and the public
/// QRegister::gpu_available() both report this, so the query and the actual
/// backend choice can never disagree — QRegister::gpu_available() used to be a
/// hardcoded `return false` while resolve_proc happily selected GPU (audit #6).
namespace detail {
bool gpu_runtime_available();
}  // namespace detail

/// Backend factory. GPU request + missing CUDA -> automatic CPU fallback with
/// a qplayer::log::warn() banner.
/// Adding a new SimType requires extending the switch inside factory.cpp
///
///
/// There is no device_model parameter: it was declared and documented but the
/// definition discarded it unnamed, so passing a DeviceModel here silently did
/// nothing (audit #36). Device data reaches the backends through
/// QRegister::load_device_model() -> NoiseModel and
/// transpile(device_json), never through construction.
std::unique_ptr<SimulatorBackend> make_backend(
    SimType  sim_type,
    ProcType proc = ProcType::Auto);

}  // namespace qplayer
