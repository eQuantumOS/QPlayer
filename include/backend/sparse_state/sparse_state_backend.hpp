/// @file sparse_state_backend.hpp
/// @brief SparseState backend declaration.
/// @ingroup backend
///
/// SparseState backend declaration
/// unordered_map<StateIndex, complex<double>> storage.
/// No noise, no GPU, no Gate Fusion. Interactive supported.
/// Default truncation threshold: kSparseDefaultThreshold.
/// : gate application, measurement collapse, execute(), set_seed().

#pragma once

#include <complex>
#include <cstdint>
#include <random>
#include <unordered_map>

#include "backend/full_state/gate_matrix.hpp"  // shared Mat2x2 + gate_matrix()
#include "backend/sparse_state/partitioned_store.hpp"  // PersistentAmplitudeStore, StateIndexHash
#include "backend/simulator_backend.hpp"

namespace qplayer {

// Default amplitude truncation threshold.
// |amp| < threshold removes floating-point round-off noise (~1e-16) while keeping
// any physically meaningful amplitude. Lossy truncation is opt-in via
// QPLAYER_SPARSE_THRESHOLD env override. Initialised from the shared
// kNonzeroAmplitudeThreshold so SparseState truncation and FullState's
// nonzero_amplitudes() filter use one single source of truth (no value drift).
constexpr double kSparseDefaultThreshold = kNonzeroAmplitudeThreshold;

// Structural qubit ceiling. StateIndex is __uint128_t,
// so basis-state indices span 2^128. Gate kernels shift by qubit index in
// [0, num_qubits-1]; at n=128 the max shift is 127 (< 128, well-defined), but
// n=129 would need a shift of 128 = undefined behaviour on __uint128_t. So 128
// is the largest safe qubit count; init_state()/prepare() reject anything above.
// This is independent of memory feasibility and of the get_statevector()
// densify cap MAX_DENSE_EXPORT_QUBITS=25.
constexpr QubitIndex kMaxSparseQubits = 128;

// Runtime memory guard cadence. apply_gate() re-checks the live
// non-zero count against the budget every kMemCheckGateInterval gates rather
// than every gate, trading detection latency for near-zero overhead (the check
// is a map.size() multiply, no syscall). 256 keeps the amortised cost negligible
// while bounding overshoot to at most this many gates of growth.
constexpr std::uint64_t kMemCheckGateInterval = 256;

// StateIndexHash moved to partitioned_store.hpp (Phase 7-H) so both the
// shard maps and any standalone map share one definition.

/// Hash-map amplitude backend for circuits with few non-zero amplitudes; supports Interactive, excludes noise, GPU and fusion.
/// @ingroup backend
class SparseStateBackend : public SimulatorBackend {
public:
    SparseStateBackend();
    ~SparseStateBackend() override;

    void   prepare      (const Circuit& circuit) override;
    Result execute      (const Circuit& circuit, std::uint32_t nshots,
                         const NoiseModel* noise = nullptr) override;

    // init_state() initialises |0...0> with num_qubits qubits.
    // TN backend throws SimulatorError here; SparseState stores the ground state.
    void   init_state   (QubitIndex num_qubits) override;
    void   apply_gate   (const GateOp& op)      override;
    int    apply_measure(QubitIndex qubit)      override;
    void   apply_reset  (QubitIndex qubit)      override;

    // get_statevector() densifies the sparse map into an AoS vector.
    // Only allowed when num_qubits <= MAX_DENSE_EXPORT_QUBITS.
    // get_statevector_raw() always throws SimulatorError — SparseState has no
    // SoA buffer.
    StateVector  get_statevector()                              override;
    RawStateView get_statevector_raw()                          override;

    // expectation_value() walks the stored non-zero entries only — O(nnz), no
    // MAX_DENSE_EXPORT_QUBITS cap, so it works at every qubit count up to 128
    // (policy table). Semantics match the
    // FullState backend: empty string = all-Z, Qiskit qubit order.
    double       expectation_value(std::string_view pauli)      override;

    // Enumerate nonzero-amplitude states without densifying.
    // Returns sorted_entries() directly — no densify, works up to 128 qubits.
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

    // set_seed() overrides the master PRNG seed for reproducible sampling.
    void set_seed(std::uint64_t seed) override;

    // set_feasibility_check() toggles the pre-flight feasibility analysis
    // Default OFF (env QPLAYER_SPARSE_FEASIBILITY also toggles). When
    // OFF, prepare()/execute() skip the prediction and rely on the runtime
    // guard + bad_alloc backstop. The Public API run()/execute() signatures are
    // unchanged — this is an additive opt-in (mirrors set_cache_blocking).
    void set_feasibility_check(bool enable) override;

    SimType          sim_type()       const override;
    bool             supports_noise() const override;
    bool             supports_gpu()   const override;
    std::string_view name()           const override;

    // Sparse storage is unbounded; report the dense-equivalent host bound as a
    // densification guidance value (policy).
    std::uint32_t max_qubits(detail::WorkingSet ws) const noexcept override;

private:
    // Amplitude store: persistent prime-modulo partitioned store, kept across
    // gates and mutated in place (Phase 8-E). Only non-zero entries
    // (above threshold_) are stored. configure(n) sizes the
    // shards once at init_state(); set_ground() seeds |0...0>. Reductions go
    // through sorted_entries() for reproducibility (0919).
    PersistentAmplitudeStore amplitudes_;

    QubitIndex num_qubits_ = 0;

    // Master PRNG seed for reproducible sampling. 0 = random device.
    // Set-ness is tracked separately from the value: 0 is a valid master seed
    // Using "master_seed_ != 0" as the unset sentinel made
    // set_seed(0) fall back to random_device while current_seed() still
    // reported 0 — a reproducibility hole that lied about itself.
    std::uint64_t master_seed_ = 0;
    bool          has_seed_    = false;

    // Truncation threshold parsed once from QPLAYER_SPARSE_THRESHOLD env.
    // Defaults to kSparseDefaultThreshold when env is absent, non-numeric, or
    // negative.
    double threshold_ = kSparseDefaultThreshold;

    // pre-flight feasibility check toggle. Default OFF; set
    // QPLAYER_SPARSE_FEASIBILITY at construction and by set_feasibility_check().
    // When false, prepare()/execute() skip the predicted-count rejection.
    bool feasibility_check_enabled_ = false;

    // Runtime memory guard state. budget_bytes_ is captured
    // detail::available_memory_bytes() at init_state() time (the single chokepoint
    // covering batch, per-shot, and Interactive paths). gate_counter_ paces the
    // periodic check in apply_gate(); both reset per fresh state. budget_bytes_
    // is UINT64_MAX (kNoLimit) when the budget is unreadable, disabling the guard.
    std::uint64_t budget_bytes_ = 0;
    std::uint64_t gate_counter_ = 0;

    // Diagnostics for the baseline measurement. When
    // QPLAYER_SPARSE_DIAG is set, execute() tracks the peak non-zero amplitude
    // count over the evolution and emits one log::info line at completion. This
    // is the only way to observe S beyond MAX_DENSE_EXPORT_QUBITS, since
    // get_statevector() densifies and cannot run past 25 qubits. Off by default:
    // diag_enabled_ gates the per-op note_peak() so there is no cost when unset.
    bool        diag_enabled_ = false;
    std::size_t peak_nonzero_ = 0;

    // Updates peak_nonzero_ with the current amplitudes_.size() when diagnostics
    // are enabled. The logical S (non-zero count). With the persistent
    // in-place store there is no per-gate rebuild/swap, so transient RSS tracks
    // peak_nonzero_ * per-entry bytes closely (the /0923 4× swap
    // transient is gone; kSparseEntryBytes re-measured).
    void note_peak();

    // Prunes |amp| < threshold_ from amplitudes_ after a gate update.
    // Called by gate application paths; provided here for completeness.
    // Does NOT insert: callers must call set_amplitude() or insert_if_above_threshold().
    void prune_below_threshold();

    // Inserts or updates amplitudes_[idx] = amp when |amp| >= threshold_.
    // Erases the entry when |amp| < threshold_ (numerical hygiene).
    void set_amplitude(StateIndex idx, std::complex<double> amp);

    // Core Born-rule collapse kernel with caller-supplied RNG.
    // Returns 0 or 1 and collapses + renormalises the surviving subspace.
    int  apply_measure_rng(QubitIndex qubit, std::mt19937_64& rng);

    // Project qubit to |0> using a caller-supplied RNG (apply_measure_rng + X).
    void apply_reset_rng  (QubitIndex qubit, std::mt19937_64& rng);
};

}  // namespace qplayer
