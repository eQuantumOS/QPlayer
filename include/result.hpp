/// @file result.hpp
/// @brief Public Result container returned by QRegister::run() and referenced
/// by interactive.result() (+).
/// @ingroup api
///
/// has_error / error_message are kept trivially false / "" — the current
/// partial-failure model throws immediately and never populates
/// these fields. They remain in the API to give the Python binding a stable
/// shape and to preserve room for a future opt-in continuation mode.

#pragma once

#include <complex>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "types.hpp"
#include "noise/noise_intervention.hpp"

namespace qplayer {

/// AoS state vector view used by Result.statevector() and by the AoS path of
/// get_statevector(). SoA buffers are exposed only via RawStateView
/// in include/types.hpp.
using StateVector = std::vector<std::complex<double>>;

/// Execution result returned by QRegister::run() — measurement counts, final
/// state, timing, and MPS truncation errors.
class Result {
public:
    /// Constructs an empty result; only backends and QRegister populate one.
    Result() = default;

    /// Measurement counts keyed by bit-string, MSB->LSB in QASM creg layout
    ///
    ///
    /// Lazy sampling: on the FullState CPU fast path the nshots draw
    /// is deferred to this first call (it fills a mutable cache from the
    /// attached final state). Hence this accessor is not noexcept. Not
    /// thread-safe on the first call: a single Result must be sampled from one
    /// thread.
    /// @return Map from bit-string (e.g. "11") to occurrence count; the values
    ///         sum to shots().
    const std::map<std::string, std::uint64_t>& counts() const;

    /// Number of shots this result was produced from.
    /// @return The nshots value passed to QRegister::run().
    std::uint64_t shots() const noexcept;

    /// Whether a final state vector is attached.
    ///
    /// True on the fast path; false on the per-shot path, where
    /// each shot collapses the state and no single final state exists.
    /// @return true when statevector() will succeed.
    bool has_statevector() const noexcept;

    /// Final state vector in AoS layout.
    /// @return Reference to the attached amplitudes, index i being basis state i.
    /// @throws CircuitError No state is attached — see has_statevector().
    const StateVector& statevector() const;

    /// Whether an expectation value was recorded for a Pauli string.
    /// @param pauli Pauli string in big-endian order (pauli[0] is qubit n-1).
    /// @return true when expectation() will succeed for this key.
    bool has_expectation(const std::string& pauli) const;

    /// Expectation value recorded for a Pauli string.
    ///
    /// On probabilistic paths this is the nshot average
    /// (1/N) * sum_j <psi_j|P|psi_j> that defines, not the value
    /// of the last shot.
    /// @param pauli Pauli string in big-endian order (pauli[0] is qubit n-1).
    /// @return The recorded expectation value in [-1, 1] for a single Pauli term.
    /// @throws CircuitError No expectation was recorded for this key.
    double expectation(const std::string& pauli) const;

    /// All recorded expectation values.
    /// @return Map from Pauli string to expectation value; empty when the
    ///         executing backend does not accumulate them.
    const std::map<std::string, double>& all_expectations() const noexcept;

    /// Whether an error message is attached.
    ///
    /// Kept for ABI stability: under the current partial-failure model
    /// failures throw immediately, so this is always false. A future
    /// opt-in continuation mode may flip it on.
    /// @return Always false in this build.
    bool has_error() const noexcept;

    /// Error message attached to a partially failed run.
    /// @return Always the empty string in this build — see has_error().
    const std::string& error_message() const noexcept;

    /// Whether SVD truncation errors were recorded.
    /// @return true for TensorNetwork runs that truncated; false for FullState
    ///         and SparseState, which never truncate.
    bool has_truncation_errors() const noexcept;

    /// Per-step SVD truncation error history.
    /// @return One entry per 2Q truncation STEP, in circuit order — 2Q gates and,
    ///         on the noise path, 2Q Kraus channels too, because a channel-induced
    /// truncation spends the same accuracy budget (note).
    ///         1Q channels never truncate. Under nshots trajectories the list
    ///         reported is the WORST trajectory's (largest accumulated error,
    ///         lowest index breaking ties), which is the number that answers
    ///         "can I trust this run".
    ///         order; empty for FullState and SparseState backends.
    const std::vector<double>& truncation_errors() const noexcept;

    /// @name TN trajectory diagnostics
    ///
    /// Indexed by SHOT, length nshots — unlike truncation_errors(), which is the
    /// worst trajectory's per-STEP history. The two vectors have different
    /// lengths and their elements must not be paired.
    /// Filled by the two paths that hold one MPS chain PER SHOT: the noise
    /// Monte-Carlo trajectories, and the noiseless per-shot path taken when
    /// Circuit::requires_per_shot() (mid-circuit measurement, reset, IfOp).
    /// Empty everywhere else, and for a reason rather than an omission: on the
    /// noiseless fast path a SINGLE chain serves every shot, and on the MPO path
    /// a single density matrix does, so a per-shot vector there would report a
    /// history that does not exist. Also empty on FullState and SparseState.
    ///@{

    /// Whether per-shot trajectory diagnostics were recorded.
    /// @return true when this run held one MPS chain per shot; false on the
    ///         noiseless single-chain fast path, the MPO path, and other
    ///         backends. Check it before indexing the two vectors.
    bool has_trajectory_diagnostics() const noexcept;

    /// Per-shot PEAK bond dimension — the largest the chain reached at any point
    /// in that shot, not its value at the end.
    /// @return One entry per shot in shot order (thread-count independent,
    ///); empty when has_trajectory_diagnostics() is false.
    ///
    /// The distinction is not pedantry: on rotated surface code d=3/5/7 the peak
    /// is 8/32/64 while the end-of-run value is 4/16/32, because a SWAP round
    /// trip and syndrome extraction both raise a bond and then lower it again.
    /// This accessor answers "how close did the run come to the cap", so it
    /// reports the peak; the end-of-run value stays backend-internal.
    const std::vector<std::uint32_t>& bond_dimensions() const noexcept;

    /// Per-shot |norm^2 - 1| before the renormalisation.
    /// @return One entry per shot in shot order; empty when
    ///         has_trajectory_diagnostics() is false. Nonzero means a truncation
    ///         cost accuracy — truncation_errors() says how much.
    const std::vector<double>& norm_drifts() const noexcept;

    /// Per-shot CUMULATIVE truncation weight, the plain sum of that shot's step
    /// errors (L1).
    /// @return One entry per shot in shot order; empty when
    ///         has_trajectory_diagnostics() is false.
    ///
    /// Not the L2 form. sqrt(sum of squares) is the budget quantity;
    /// the distance between a truncated trajectory and the exact one accumulates
    /// like this sum, and it is that distance which bounds how far an ensemble
    /// observable can be off. Note the indices differ from truncation_errors(),
    /// which is one worst trajectory's per-STEP history — do not pair them up.
    const std::vector<double>& truncation_totals() const noexcept;

    ///@}

    /// Per-shot noise draw ledger.
    /// @return Indexed by shot; each element lists that shot's noise sites in
    ///         canonical execution order. Filled only when
    ///         QRegister::set_noise_draw_ledger(true) was set and a noise
    ///         trajectory path ran (FullState CPU, TensorNetwork Monte Carlo);
    /// empty otherwise, like the diagnostics.
    const std::vector<std::vector<NoiseDrawEvent>>& noise_draw_ledger() const noexcept;

    /// Echo of the noise interventions applied to this run.
    /// @return Sorted site numbers; empty when the run had no intervention.
    const std::vector<std::uint32_t>& noise_interventions() const noexcept;

private:
    /// Lazy sampling snapshot. Populated by a backend fast path instead
    /// of drawing nshots eagerly; counts() draws on first call. measure_map holds
    /// (qubit, cubit) pairs. Two modes (개정):
    ///   - dense  (FullState CPU/GPU): sparse_probs empty -> draw from statevector_.
    ///   - sparse (SparseState): sparse_probs holds normalised (basis_index, prob)
    ///     pairs -> draw from the sparse CDF without densifying (works > 25 qubits).
    struct LazySampling {
        /// Deferred shot count to draw on the first counts() call.
        std::uint32_t                                            nshots = 0;
        /// RNG seed reproducing the eager path's stream.
        std::uint64_t                                            seed   = 0;
        /// Total classical bits, fixing the width of each counts() key.
        ClassicalBitIndex                                       total_cubits = 0;
        /// (qubit, cubit) pairs mapping measured qubits to classical bits.
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>>    measure_map;
        /// Normalised (basis index, probability) pairs for the sparse draw mode;
        /// empty in dense mode, where the draw reads the state vector instead.
        std::vector<std::pair<StateIndex, double>>               sparse_probs;
    };

    /// Draw the deferred nshots samples into counts_ from statevector_.
    /// Reproduces the eager path's RNG stream / inverse-CDF for an identical seed.
    void perform_lazy_sampling() const;

    /// GPU-resident sampling handle (Phase 7-C). When the FullState GPU
    /// fast path defers sampling, it shares ownership of the device SoA state via
    /// gpu_owner_ (kept alive past the backend's next run()), with gpu_re_/gpu_im_
    /// raw device pointers valid while gpu_owner_ is non-null. counts() then draws
    /// on-device (no 2GiB D2H + AoS materialize) and statevector() D2Hs lazily.
    bool                  has_gpu_resident() const noexcept { return gpu_owner_ != nullptr; }
    void                  materialize_gpu_statevector() const;  // lazy D2H + AoS

    /// CPU-resident sampling handle (mirrors GPU above). When the
    /// FullState CPU fast path defers sampling (P2), it shares ownership of the
    /// host SoA buffers via cpu_owner_ so counts() reads SoA directly and
    /// statevector() AoS-materialises lazily — avoids the 2 GiB eager copy.
    bool has_cpu_resident() const noexcept { return cpu_owner_ != nullptr; }
    void materialize_cpu_statevector() const;  // lazy SoA -> AoS conversion

    mutable std::map<std::string, std::uint64_t> counts_;
    std::uint64_t                        shots_ = 0;
    mutable std::optional<StateVector>   statevector_;
    std::shared_ptr<void>                gpu_owner_;
    const double*                        gpu_re_  = nullptr;
    const double*                        gpu_im_  = nullptr;
    StateIndex                           gpu_dim_ = 0;
    /// CPU-resident SoA handle. cpu_owner_ keeps the CpuStateBuffers
    /// alive; cpu_re_/cpu_im_ are cached pointers into that buffer.
    std::shared_ptr<void>                cpu_owner_;
    const double*                        cpu_re_  = nullptr;
    const double*                        cpu_im_  = nullptr;
    StateIndex                           cpu_dim_ = 0;
    std::map<std::string, double>        expectations_;
    std::string                          error_message_;
    std::vector<double>                  truncation_errors_;
    /// Per-shot TN diagnostics; both empty on non-MPS backends.
    std::vector<std::uint32_t>           bond_dimensions_;
    std::vector<double>                  norm_drifts_;
    std::vector<double>                  truncation_totals_;
    /// Noise intervention echo and draw ledger.
    std::vector<std::uint32_t>                noise_interventions_;
    std::vector<std::vector<NoiseDrawEvent>>  noise_draw_ledger_;
    std::optional<LazySampling>          lazy_;
    mutable bool                         sampled_ = false;

    friend class QRegister;
    friend class Interactive;
    friend class SimulatorBackend;
};

}  // namespace qplayer
