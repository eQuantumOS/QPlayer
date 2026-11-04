// result.hpp — Public Result container returned by QRegister::run() and
// referenced by interactive.result().
// has_error / error_message are kept trivially false / "" — the current
// partial-failure model throws immediately and never populates
// these fields. They remain in the API to give the Python binding a stable
// shape and to preserve room for a future opt-in continuation mode.

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

namespace qplayer {

// AoS state vector view used by Result.statevector() and by the AoS path of
// get_statevector(). SoA buffers are exposed only via RawStateView
// in include/types.hpp.
using StateVector = std::vector<std::complex<double>>;

class Result {
public:
    Result() = default;

    // ── Measurement counts ─────────────────────
    // Key: bit-string ordered MSB->LSB matching QASM creg layout
    //      (e.g. {"00": 512, "11": 512} for a Bell pair).
    // Lazy sampling: on the FullState CPU fast path the nshots draw
    // is deferred to the first counts() call (it fills a mutable cache from the
    // attached final state). Hence counts() is not noexcept. Not thread-safe on
    // the first call: a single Result must be sampled from one thread.
    const std::map<std::string, std::uint64_t>& counts() const;
    std::uint64_t                               shots()  const noexcept;

    // ── State vector ───────────────────────────
    // has_statevector() depends on the fast path vs the per-shot
    // path. statevector() throws CircuitError when unavailable.
    bool               has_statevector() const noexcept;
    const StateVector& statevector()     const;

    // ── Expectation values ───────────────────────────────
    // pending: dict-fill policy (user pre-registers vs auto-compute).
    bool                                  has_expectation(const std::string& pauli) const;
    double                                expectation    (const std::string& pauli) const;
    const std::map<std::string, double>&  all_expectations() const noexcept;

    // ── Error info (kept for ABI; always trivial under) ───
    // Continuation mode (M4+) may flip these on; the current build always
    // returns false / "" because failures throw immediately.
    bool               has_error()     const noexcept;
    const std::string& error_message() const noexcept;

    // ── SVD truncation history ─────────────────
    // One entry per 2Q gate that triggered SVD truncation in the TN backend.
    // Empty for FullState / SparseState backends.
    bool                       has_truncation_errors() const noexcept;
    const std::vector<double>& truncation_errors()     const noexcept;

private:
    // Lazy sampling snapshot. Populated by a backend fast path instead
    // of drawing nshots eagerly; counts() draws on first call. measure_map holds
    // (qubit, cubit) pairs. Two modes:
    //   - dense  (FullState CPU/GPU): sparse_probs empty -> draw from statevector_.
    //   - sparse (SparseState): sparse_probs holds normalised (basis_index, prob)
    //     pairs -> draw from the sparse CDF without densifying (works > 25 qubits).
    struct LazySampling {
        std::uint32_t                                            nshots = 0;
        std::uint64_t                                            seed   = 0;
        ClassicalBitIndex                                       total_cubits = 0;
        std::vector<std::pair<QubitIndex, ClassicalBitIndex>>    measure_map;
        std::vector<std::pair<StateIndex, double>>               sparse_probs;
    };

    // Draw the deferred nshots samples into counts_ from statevector_.
    // Reproduces the eager path's RNG stream / inverse-CDF for an identical seed.
    void perform_lazy_sampling() const;

    // GPU-resident sampling handle (Phase 7-C). When the FullState GPU
    // fast path defers sampling, it shares ownership of the device SoA state via
    // gpu_owner_ (kept alive past the backend's next run()), with gpu_re_/gpu_im_
    // raw device pointers valid while gpu_owner_ is non-null. counts() then draws
    // on-device (no 2GiB D2H + AoS materialize) and statevector() D2Hs lazily.
    bool                  has_gpu_resident() const noexcept { return gpu_owner_ != nullptr; }
    void                  materialize_gpu_statevector() const;  // lazy D2H + AoS

    // CPU-resident sampling handle (mirrors GPU above). When the
    // FullState CPU fast path defers sampling (P2), it shares ownership of the
    // host SoA buffers via cpu_owner_ so counts() reads SoA directly and
    // statevector() AoS-materialises lazily — avoids the 2 GiB eager copy.
    bool has_cpu_resident() const noexcept { return cpu_owner_ != nullptr; }
    void materialize_cpu_statevector() const;  // lazy SoA -> AoS conversion

    mutable std::map<std::string, std::uint64_t> counts_;
    std::uint64_t                        shots_ = 0;
    mutable std::optional<StateVector>   statevector_;
    std::shared_ptr<void>                gpu_owner_;
    const double*                        gpu_re_  = nullptr;
    const double*                        gpu_im_  = nullptr;
    StateIndex                           gpu_dim_ = 0;
    // CPU-resident SoA handle. cpu_owner_ keeps the CpuStateBuffers
    // alive; cpu_re_/cpu_im_ are cached pointers into that buffer.
    std::shared_ptr<void>                cpu_owner_;
    const double*                        cpu_re_  = nullptr;
    const double*                        cpu_im_  = nullptr;
    StateIndex                           cpu_dim_ = 0;
    std::map<std::string, double>        expectations_;
    std::string                          error_message_;
    std::vector<double>                  truncation_errors_;
    std::optional<LazySampling>          lazy_;
    mutable bool                         sampled_ = false;

    friend class QRegister;
    friend class Interactive;
    friend class SimulatorBackend;
};

}  // namespace qplayer
