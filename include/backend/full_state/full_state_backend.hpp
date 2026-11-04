// full_state_backend.hpp — FullState backend declarations (CPU + GPU)
// Two concrete classes share this header so the
// factory can return either one without dragging CUDA headers into Public API
// consumers. Implementation files live under src/backend/full_state/.
// apply_fused_program() added for runtime Gate Fusion seam.

#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "backend/simulator_backend.hpp"
#include "backend/full_state/gate_fusion.hpp"

namespace qplayer {

// CPU-resident SoA state buffer shared between the backend and any live Result
// (mirrors GpuStateBuffers in full_state_gpu.cpp). Owns the aligned
// re/im arrays via AlignedDeleter unique_ptrs so lifetime is ref-counted.
// Non-copyable: each array is managed by exactly one owner chain.
struct CpuStateBuffers {
    // Forward-declare so the unique_ptr type is complete below. AlignedDeleter
    // is defined as a public member of FullStateCpuBackend in this header.
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

// FullState CPU. SoA state vector (re / im double arrays).
// SIMD runtime dispatcher picks AVX2 vs AVX-512 once at process start
// Cache blocking on 1Q + 2Q gates.
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

    // AlignedDeleter: std::free wrapper for 64-byte-aligned double arrays.
    // Re-exposed here as a public typedef alias pointing to CpuStateBuffers'
    // inner struct so existing .cpp call sites (alloc_aligned return type) still
    // compile unmodified (— new/delete direct use forbidden).
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
    std::uint64_t master_seed_  = 0;   // PRNG master seed; 0 = use random_device

    // Runtime gate fusion config (set_fusion). Default matches the
    // QRegister::run() defaults (enable_fusion=true, fusion_max_width=4).
    bool          fusion_enabled_   = true;
    std::uint32_t fusion_max_width_ = 4;

    // L2-tiled cache blocking. Default OFF since Phase 7-D
    // — measured net-negative on modern server CPUs. Opt in via
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

// FullState GPU. CUDA-backed twin of the CPU class. GPU-resident state vector
// + cuBLAS GEMV + CUDA Streams. Constructor is
// noexcept fall-through-friendly: when CUDA is missing the factory builds a
// FullStateCpuBackend instead.
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

private:
    // Op-variant dispatcher mirroring the CPU backend. Wired into
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
    // Mirrors the CPU backend's master_seed_ so set_seed() works uniformly.
    std::uint64_t master_seed_ = 0;

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
