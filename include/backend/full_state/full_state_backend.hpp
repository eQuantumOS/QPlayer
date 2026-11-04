/// @file full_state_backend.hpp
/// @brief FullState CPU and GPU backend declarations.
/// @ingroup backend
///
/// FullState backend declarations (CPU + GPU)
/// Two concrete classes share this header so the
/// factory can return either one without dragging CUDA headers into Public API
/// consumers. Implementation files live under src/backend/full_state/.
/// apply_fused_program() added for runtime Gate Fusion seam.

#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "backend/simulator_backend.hpp"
#include "backend/full_state/gate_fusion.hpp"

namespace qplayer {

/// Owns the 64-byte-aligned SoA real and imaginary amplitude arrays.
/// @ingroup backend
///
/// CPU-resident SoA state buffer shared between the backend and any live Result
/// (mirrors GpuStateBuffers in full_state_gpu.cpp). Owns the aligned
/// re/im arrays via AlignedDeleter unique_ptrs so lifetime is ref-counted.
/// Non-copyable: each array is managed by exactly one owner chain.
struct CpuStateBuffers {
    /// Frees aligned_alloc storage so the buffers can live in a unique_ptr.
    /// @ingroup backend
    ///
    /// Forward-declare so the unique_ptr type is complete below. AlignedDeleter
    /// is defined as a public member of FullStateCpuBackend in this header.
    struct AlignedDeleter {
        void operator()(double* p) const noexcept;
    };
    std::unique_ptr<double[], AlignedDeleter> re;
    std::unique_ptr<double[], AlignedDeleter> im;
    StateIndex dim = 0;

    CpuStateBuffers() = default;
    CpuStateBuffers(const CpuStateBuffers&)            = delete;
    CpuStateBuffers& operator=(const CpuStateBuffers&) = delete;
};

/// Dense statevector backend on CPU with SIMD kernels, OpenMP parallelism and runtime gate fusion.
/// @ingroup backend
///
/// FullState CPU. SoA state vector (re / im double arrays).
/// SIMD runtime dispatcher picks AVX2 vs AVX-512 once at process start
/// Cache blocking on 1Q + 2Q gates.
class FullStateCpuBackend : public SimulatorBackend {
public:
    FullStateCpuBackend();
    ~FullStateCpuBackend() override;

    void   prepare      (const Circuit& circuit) override;
    Result execute      (const Circuit& circuit, std::uint32_t nshots,
                         const NoiseModel* noise = nullptr) override;
    void   init_state   (QubitIndex num_qubits) override;
    void   apply_gate   (const GateOp& op)      override;
    int    apply_measure(QubitIndex qubit)      override;
    void   apply_reset  (QubitIndex qubit)      override;

    StateVector  get_statevector()                              override;
    RawStateView get_statevector_raw()                          override;
    double       expectation_value(std::string_view pauli)      override;

    // Enumerate nonzero-amplitude states without densifying.
    // Scans re_/im_ in index order; returns empty vector before init_state().
    std::vector<std::pair<StateIndex, std::complex<double>>>
            nonzero_amplitudes() const override;
    // Single amplitude and RDM —
    // simulator_backend.hpp for the shared contract.
    std::complex<double> amplitude(StateIndex basis) const override;
    std::vector<std::complex<double>>
            reduced_density_matrix(std::vector<QubitIndex> qubits) const override;


    // Active qubit count (0 before init_state). Used by dump_statevector()
    // to format the bitstring width without densifying.
    QubitIndex num_qubits() const override;

    SimType          sim_type()       const override;
    bool             supports_noise() const override;
    bool             supports_gpu()   const override;
    std::string_view name()           const override;

    // Host peak model: max no-swap dense qubit count.
    std::uint32_t max_qubits(detail::WorkingSet ws) const noexcept override;

    // Seed the internal PRNG for reproducible measurement sampling.
    // Called by QRegister::set_seed() via the backend interface.
    void set_seed(std::uint64_t seed) override;

    // Store the runtime gate-fusion config. When enabled, execute()'s
    // fast path fuses the circuit via detail::fuse_circuit().
    void set_fusion(bool enable, std::uint32_t max_width) override;

    // Enable or disable L2-tiled cache blocking on 1Q/2Q gate kernels.
    // Default: false (OFF) since Phase 7-D — measured blocking
    // as net-negative on modern server CPUs (q21 +53%, q27 no gain). Opt in via
    // set_cache_blocking(true) or env QPLAYER_CACHE_BLOCKING=1.
    void set_cache_blocking(bool enable) override;

    // Per-site noise interventions and draw ledger for the trajectory path.
    void set_noise_interventions(const std::vector<std::uint32_t>& sites,
                                 bool ledger) override;

    // Current cache-blocking state. Exists because the blocked and unblocked
    // kernels are numerically identical, so the default-OFF invariant
    // is not observable through results alone — a test must read the flag
    // directly to assert it (testing gap #4).
    bool cache_blocking_enabled() const { return cache_blocking_enabled_; }

    /// Public alias for the aligned-array deleter used by this backend's buffers.
    ///
    /// std::free wrapper for 64-byte-aligned double arrays. Re-exposed here as a
    /// public typedef alias pointing to CpuStateBuffers' inner struct so existing
    /// cpp call sites (alloc_aligned return type) still compile unmodified
    /// (— new/delete direct use forbidden).
    using AlignedDeleter = CpuStateBuffers::AlignedDeleter;

    // NaN/Inf + norm validation helper.
    // Throws SimulatorError when |psi|^2 is not 1 within 1e-6 or any amplitude
    // is non-finite.  op_index is embedded in the error message for diagnosis.
    // Called by execute(); exposed public so unit tests can directly
    // verify the guard without going through the full execute() pipeline.
    void verify_state_finite_and_normalized(std::size_t op_index);

    // Runtime Gate Fusion seam for testing.
    // Traverses a fused program produced by detail::fuse_circuit() and applies
    // each FusedGate via detail::apply_fused_gate(). Boundary ops
    // (Measure/Barrier/If/Idle) are no-ops at this stage; the full execute()
    // pipeline handles measurement sampling and classical control.
    // Requires init_state() to have been called first.
    void apply_fused_program(const std::vector<qplayer::detail::FusedOp>& prog);

private:

    // Shared owner of the CPU-resident SoA buffers (mirrors
    // state_owner_ in FullStateGpuBackend). A Result from execute()
    // holds a copy so the host state survives the backend's next init_state()
    // for deferred sampling / lazy AoS materialisation. Null before init_state().
    std::shared_ptr<CpuStateBuffers> state_owner_;

    // Cached raw pointers into state_owner_->re/im. Valid while
    // state_owner_ is non-null; re_/im_ are updated by init_state() to point
    // into the (possibly new) buffer allocated that call.
    double* re_ = nullptr;
    double* im_ = nullptr;

    QubitIndex    num_qubits_   = 0;
    StateIndex    dim_          = 0;   // 2^num_qubits_; 0 before init_state()
    // Set-ness is tracked separately from the value: 0 is a valid master seed
    // Using "master_seed_ != 0" as the unset sentinel made
    // set_seed(0) fall back to random_device while current_seed() still
    // reported 0 — a reproducibility hole that lied about itself.
    std::uint64_t master_seed_  = 0;   // PRNG master seed (any value, 0 included)
    bool          has_seed_     = false;  // true once set_seed() has been called
    std::vector<std::uint32_t> noise_sites_;          // sorted
    bool                       noise_ledger_ = false;

    // Runtime gate fusion config (set_fusion). Default matches the
    // QRegister::run() defaults (enable_fusion=true, fusion_max_width=4).
    bool          fusion_enabled_   = true;
    std::uint32_t fusion_max_width_ = 4;

    // Per-gate NaN/norm validation, opt-in via env QPLAYER_VERIFY_STATE=1
    // execute() always validates once at the end; the per-gate mode
    // costs an extra O(2^n) reduction per gate and exists for bisecting which
    // gate diverged. Read at construction like QPLAYER_CACHE_BLOCKING.
    bool          verify_state_per_gate_ = false;

    // L2-tiled cache blocking. Default OFF since Phase 7-D
    // — measured net-negative on modern server CPUs. Opt
    // set_cache_blocking(true) or env QPLAYER_CACHE_BLOCKING=1.
    // Only affects 1Q/2Q gate kernels.
    bool cache_blocking_enabled_ = false;

    // Op-variant dispatcher. Called by execute() for each Op in the circuit
    // Routing to apply_gate / apply_measure / apply_idle or special
    // handling per Op type. Wired into execute() at /.
    void apply_op(const Op& op);

    // Phase 5 dispatch slot for IdleOp. No state change in Phase 5; Amplitude
    // Damping + Dephasing Kraus channels are activated in Phase 8.
    void apply_idle(const IdleOp& op);

    // Internal measure with caller-supplied RNG — used by execute() per-shot
    // loop so measurements within a shot share the same RNG stream and produce
    // reproducible, independent outcomes when master_seed_ is set.
    int  apply_measure_rng(QubitIndex qubit, std::mt19937_64& rng);

    // Internal reset with caller-supplied RNG (delegates to apply_measure_rng).
    void apply_reset_rng(QubitIndex qubit, std::mt19937_64& rng);
};

/// Dense statevector backend keeping the state GPU-resident; batch execute() only.
/// @ingroup backend
///
/// FullState GPU. CUDA-backed twin of the CPU class. GPU-resident state vector
/// + cuBLAS GEMV + CUDA Streams. Constructor is
/// noexcept fall-through-friendly: when CUDA is missing the factory builds a
/// FullStateCpuBackend instead.
class FullStateGpuBackend : public SimulatorBackend {
public:
    FullStateGpuBackend();
    ~FullStateGpuBackend() override;

    void   prepare      (const Circuit& circuit) override;
    Result execute      (const Circuit& circuit, std::uint32_t nshots,
                         const NoiseModel* noise = nullptr) override;
    void   init_state   (QubitIndex num_qubits) override;
    void   apply_gate   (const GateOp& op)      override;
    int    apply_measure(QubitIndex qubit)      override;
    void   apply_reset  (QubitIndex qubit)      override;

    StateVector  get_statevector()                              override;
    RawStateView get_statevector_raw()                          override;
    double       expectation_value(std::string_view pauli)      override;

    // Enumerate nonzero-amplitude states without densifying.
    // Delegates to get_statevector() (GPU is dense, <=30 qubits, so D2H is fine).
    std::vector<std::pair<StateIndex, std::complex<double>>>
            nonzero_amplitudes() const override;
    // Single amplitude and RDM —
    // simulator_backend.hpp for the shared contract.
    std::complex<double> amplitude(StateIndex basis) const override;
    std::vector<std::complex<double>>
            reduced_density_matrix(std::vector<QubitIndex> qubits) const override;


    // Active qubit count (0 before init_state). Used by dump_statevector()
    // to format the bitstring width without densifying.
    QubitIndex num_qubits() const override;

    SimType          sim_type()       const override;
    bool             supports_noise() const override;
    bool             supports_gpu()   const override;
    std::string_view name()           const override;

    // VRAM ∧ host dual-budget model: min(VRAM limit, host limit).
    std::uint32_t max_qubits(detail::WorkingSet ws) const noexcept override;

    // Seed the internal PRNG for reproducible measurement sampling.
    void set_seed(std::uint64_t seed) override;

    // Runtime Gate Fusion on the GPU fast path (superseding
    // 's "unsupported" state). Widths are clamped to kMaxGpuFusionWidth;
    // the noisy trajectory path stays unfused.
    void set_fusion(bool enable, std::uint32_t max_width) override;

    // Widest fused window the GPU kernel accepts. A thread holds 2^k amplitudes
    // in registers, so at k=5 (64 doubles) they spill to local memory and
    // occupancy collapses: measured q26_ising 32.2 ms at width 4 against 56.2 ms
    // at width 5, and every benchmark circuit was slower at 5.
    // The CPU cap (kMaxFusionWidth = 8) is a different contract — its scratch
    // lives on the stack, where 2^8 amplitudes cost nothing.
    static constexpr std::uint32_t kMaxGpuFusionWidth = 4;

private:
    // Warn about an over-cap fusion width at most once per backend instance:
    // set_fusion() runs on every run() call, so an unguarded warning would
    // flood the log for a single mis-set knob.
    bool fusion_width_warned_ = false;

    // Apply one fused program element sequence on the device. Dense
    // windows go to the fused GEMV kernel, passed-through GateOps to apply_gate.
    void apply_fused_program_gpu(const std::vector<detail::FusedOp>& prog);

    // Scratch device buffers holding every window's qubit list and matrix for
    // one program, grown on demand and reused across run() calls. One buffer for
    // the whole program, not one per window: a per-window upload would need a
    // stream synchronise each time and serialise the pipeline.
    unsigned int* d_fused_qubits_        = nullptr;
    double*       d_fused_matrix_        = nullptr;
    std::size_t   fused_matrix_capacity_ = 0;   // in doubles

    // Op-variant dispatcher mirroring the CPU backend. Wired
    // the GPU execute() pipeline at +; registers the routing skeleton
    // so the IdleOp branch has a home before Phase 8 activates Kraus channels.
    void apply_op(const Op& op);

    // dispatch slot for IdleOp — no state change yet. Amplitude Damping +
    // Dephasing Kraus channels (GPU path) are activated in Phase 8 via the
    // same formula used by the CPU backend.
    void apply_idle(const IdleOp& op);

    // GPU-resident SoA state vector (device pointers). Raw
    // double* keeps this header free of cuda_runtime.h; allocation lives in the
    // cu launchers. Null before init_state(). Lifetime is owned by state_owner_
    // — these are cached pointers into that shared buffer.
    double*    d_re_       = nullptr;
    double*    d_im_       = nullptr;
    QubitIndex num_qubits_ = 0;
    StateIndex dim_        = 0;   // 2^num_qubits_; 0 before init_state()

    // Shared owner of the GPU-resident SoA buffers (Phase 7-C). A
    // Result from execute() holds a copy so the device state survives the
    // backend's next init_state() for deferred sampling / lazy D2H. Ownership is
    // shared (refcounted cudaFree), not transferred — the backend keeps using
    // d_re_/d_im_ for get_statevector() after run(). Null before init_state().
    std::shared_ptr<void> state_owner_;

    // PRNG master seed for reproducible sampling. 0 means random.
    // Mirrors the CPU backend's master_seed_ / has_seed_ so set_seed() works
    // uniformly (— 0 is a valid seed, set-ness is a separate flag).
    std::uint64_t master_seed_ = 0;
    bool          has_seed_    = false;

    // Runtime gate fusion config. Defaults match the
    // QRegister::run() defaults; max_width is clamped to kMaxGpuFusionWidth.
    bool          fusion_enabled_   = true;
    std::uint32_t fusion_max_width_ = 4;

    // CUDA stream handle stored as unsigned long long to match the extern "C"
    // launcher signatures (which use unsigned long long to avoid cuda_runtime.h
    // in the header). cudaStream_t is a pointer; its integer representation fits
    // in 64 bits on all supported 64-bit platforms. 0 = no stream yet.
    unsigned long long stream_handle_ = 0;

    // Host-side SoA cache for get_statevector_raw().
    // Populated on demand via a single D2H copy; invalidated conceptually
    // at the next apply_gate / init_state call (contract: callers must not
    // retain the view across state-mutating calls). std::vector manages
    // heap lifetime without new/delete.
    std::vector<double> raw_cache_re_;
    std::vector<double> raw_cache_im_;
};

}  // namespace qplayer
