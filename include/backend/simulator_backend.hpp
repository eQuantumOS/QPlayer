// simulator_backend.hpp — Abstract simulator interface.
// All four backends (FullState CPU / FullState GPU / SparseState / TensorNetwork)
// derive from this class. The QRegister facade owns a unique_ptr<SimulatorBackend>
// produced by make_backend().

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

// Mirrors the user-facing enum in qregister.hpp so the factory + backend
// metadata can self-identify without dragging the QRegister header here.
// SSOT: :247 + backend factory pattern.
// GPU dispatch is driven by ProcType, not a separate SimType value -- see
// make_backend() in factory.cpp.
enum class SimType {
    FullState,
    SparseState,
    TensorNetwork,
};

enum class ProcType {
    CPU,
    GPU,
    Auto,
};

class Circuit;  // Forward declaration — full definition pulled by .cpp files.

// Shared lower bound on |amp| for a basis state to count as "nonzero" in
// nonzero_amplitudes(). Unifies the filter across backends: FullState
// scans its dense array for |amp| >= this value (dropping ~1e-17 roundoff dust),
// and SparseState's default truncation threshold is initialised from
// it, so the same circuit yields an identical nonzero set on every backend.
constexpr double kNonzeroAmplitudeThreshold = 1e-12;

class SimulatorBackend {
public:
    virtual ~SimulatorBackend() = default;

    // ── Batch mode (QRegister::run path) ───────────────────────────
    // prepare(): runtime fusion + memory pre-check.
    // execute(): one-shot pipeline that consumes the Circuit and emits a
    //   Result containing counts + optional statevector + truncation_errors.
    virtual void   prepare(const Circuit& circuit) = 0;
    virtual Result execute(const Circuit&     circuit,
                           std::uint32_t      nshots,
                           const NoiseModel*  noise = nullptr) = 0;

    // ── Interactive mode (QRegister::interactive path) ───
    // Supported by FullState (CPU / GPU) and SparseState. TensorNetwork
    // throws SimulatorError on init_state() per the table in.
    virtual void init_state  (QubitIndex num_qubits) = 0;
    virtual void apply_gate  (const GateOp& op)      = 0;
    virtual int  apply_measure(QubitIndex qubit)     = 0;
    virtual void apply_reset (QubitIndex qubit)      = 0;

    // ── State access (Batch + Interactive) ─
    // Per-backend support matrix:
    //   FullState CPU/GPU : full support
    //   SparseState       : get_statevector when n <= MAX_STATEVECTOR_QUBITS;
    //                       get_statevector_raw always throws SimulatorError
    //   TensorNetwork     : get_statevector when n <= MAX_STATEVECTOR_QUBITS;
    //                       expectation_value is partial-contraction efficient
    virtual StateVector  get_statevector()                              = 0;
    virtual RawStateView get_statevector_raw()                          = 0;
    // An empty pauli defaults to all-Z over every qubit (<Z...Z>);
    // backends build the "ZZ..Z" string from their own num_qubits.
    virtual double       expectation_value(std::string_view pauli)      = 0;

    // Enumerate only nonzero-amplitude basis states as (index, amplitude) pairs,
    // ascending by StateIndex (reproducible). No densify — SparseState works up to
    // 128 qubits; FullState scans its dense array; TensorNetwork throws.
    // "Nonzero" means |amp| >= kNonzeroAmplitudeThreshold on every backend, so the
    // nonzero set matches across backends and FP dust is excluded.
    // Returns an empty vector before init_state() is called.
    virtual std::vector<std::pair<StateIndex, std::complex<double>>>
            nonzero_amplitudes() const = 0;

    // Current active qubit count (0 before init_state). Used by dump_statevector()
    // to format the bitstring width without densifying.
    virtual QubitIndex num_qubits() const = 0;

    // ── Metadata ──────────────────────────────────────────────────
    virtual SimType          sim_type()       const = 0;
    virtual bool             supports_noise() const = 0;
    virtual bool             supports_gpu()   const = 0;
    // Short backend identifier literal used by SimulatorError messages
    // Returns one of "FullState", "FullStateGPU",
    // "SparseState", "TensorNetwork".
    virtual std::string_view name()           const = 0;

    // ── Backend-specific qubit ceiling ──────────────────────
    // Largest no-swap qubit count for this backend under the given working set.
    // FullState CPU uses the host peak model; FullState GPU returns
    // min(VRAM limit, host pinned/AoS limit); SparseState / TensorNetwork report
    // the dense-equivalent host bound as a guidance value. QRegister::max_qubits()
    // delegates here so the answer follows the actual backend instance (a GPU
    // request that fell back to CPU naturally reports the CPU value).
    virtual std::uint32_t max_qubits(detail::WorkingSet ws) const noexcept = 0;

    // ── Seed propagation ─────────────────────────
    // Default no-op so SparseState / TN stubs are unaffected. FullState CPU
    // overrides to seed the internal mt19937_64 for reproducible sampling.
    virtual void set_seed(std::uint64_t /*seed*/) {}

    // ── Runtime gate fusion config ─────
    // QRegister::run() forwards its enable_fusion / fusion_max_width arguments
    // here before execute(). Default no-op; SparseState ignores fusion, and
    // FullState CPU stores the config for the fused execution path.
    virtual void set_fusion(bool /*enable*/, std::uint32_t /*max_width*/) {}

    // ── Cache blocking toggle (benchmark) ──────────
    // Enable or disable L2-tiled cache blocking on 1Q/2Q gate kernels.
    // Default on for FullState CPU.
    // Other backends ignore this call (GPU uses coalesced access instead,
    // SparseState/TN have no cache blocking). FullState CPU overrides this.
    virtual void set_cache_blocking(bool /*enable*/) {}

    // ── SparseState feasibility pre-check toggle ────────
    // Enable/disable the pre-flight feasibility analysis (predicted non-zero
    // count -> reject before running). Default OFF: only SparseState overrides
    // this; other backends ignore it. Also toggled via QPLAYER_SPARSE_FEASIBILITY.
    // The runtime guard and bad_alloc backstop stay on regardless.
    virtual void set_feasibility_check(bool /*enable*/) {}

protected:
    // ── Result population helpers ──────────────────────────
    // Derived backends call these to fill Result private fields. The friend
    // declaration on Result allows SimulatorBackend (and its subclasses via
    // protected access) to reach the private members (result.hpp).
    static void result_set_counts(Result& r,
                                  std::map<std::string, std::uint64_t> counts);
    static void result_set_shots (Result& r, std::uint64_t shots);
    static void result_set_statevector(Result& r, StateVector sv);
    // Deferred sampling snapshot. The FullState CPU/GPU fast path
    // calls this (dense mode: counts() draws from the attached statevector_)
    // instead of result_set_counts(); Result::counts() draws on first use.
    static void result_set_lazy_sampling(
        Result& r,
        std::uint32_t nshots,
        std::uint64_t seed,
        ClassicalBitIndex total_cubits,
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map);
    // Sparse-mode deferred sampling (개정). The SparseState fast path
    // passes normalised (basis_index, prob) pairs; counts() draws from the sparse
    // CDF without densifying (works beyond MAX_STATEVECTOR_QUBITS).
    static void result_set_lazy_sampling_sparse(
        Result& r,
        std::uint32_t nshots,
        std::uint64_t seed,
        ClassicalBitIndex total_cubits,
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>> measure_map,
        std::vector<std::pair<StateIndex, double>> sparse_probs);
    // GPU-resident sampling handle (Phase 7-C). The FullState GPU fast
    // path shares ownership of the device SoA state with the Result so counts()
    // samples on-device (no D2H) and statevector() can D2H lazily. `owner` keeps
    // the device buffers alive past the next run()/init_state(); d_re/d_im are
    // raw device pointers valid while owner is non-null.
    static void result_set_gpu_resident(
        Result& r,
        std::shared_ptr<void> owner,
        const double* d_re,
        const double* d_im,
        StateIndex dim);

    // CPU-resident sampling handle (mirrors result_set_gpu_resident).
    // The FullState CPU fast path shares ownership of the host SoA buffers with
    // the Result so counts() draws from SoA directly (no AoS eager copy) and
    // statevector() AoS-materialises lazily. `owner` keeps the CpuStateBuffers
    // alive past the backend's next init_state() (P3 reuse safety).
    static void result_set_cpu_resident(
        Result& r,
        std::shared_ptr<void> owner,
        const double* re,
        const double* im,
        StateIndex dim);
};

// Backend factory. GPU request + missing CUDA -> automatic CPU fallback with
// a qplayer::log::warn() banner.
// Adding a new SimType requires extending the switch inside factory.cpp
//
std::unique_ptr<SimulatorBackend> make_backend(
    SimType            sim_type,
    ProcType           proc        = ProcType::Auto,
    const DeviceModel* device_model = nullptr);

}  // namespace qplayer
