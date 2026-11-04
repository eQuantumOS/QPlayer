/// @file tn_backend.hpp
/// @brief TensorNetwork backend declaration.
/// @ingroup backend
///
/// TensorNetwork backend declaration.
/// MPS / general TN storage. Interactive mode is NOT supported — init_state /
/// apply_gate / apply_measure / apply_reset throw SimulatorError at runtime
/// (backend matrix). Batch-only via execute().

#pragma once

#include "backend/simulator_backend.hpp"
#include "backend/tensor_network/topology.hpp"

#include <complex>
#include <cstddef>
#include <map>
#include <string>
#include <memory>
#include <optional>
#include <cstdint>
#include <vector>

namespace qplayer {

// ── MPS truncation policy constants ────────────────────────
//
// The two knobs are applied together: kept = min(max_bond, #singular values
// whose discarded tail stays within cutoff). Either one alone leaves the other
// axis unbounded — a bond cap bounds resources but not error, a cutoff bounds
// error but lets chi grow to 2^(n/2). Env override (QPLAYER_MPS_*) and the
// set_mps_truncation() virtual land with the SVD path.

/// Default bond dimension cap: 2^8, exact for up to 8 EPR pairs across any cut (B).
constexpr std::uint32_t kMpsDefaultMaxBond = 256;

/// Largest bond dimension a user may request — beyond this one SVD costs minutes per gate (B).
constexpr std::uint32_t kMpsMaxBondCeiling = 4096;

/// Default SVD cutoff on the amplitude-scale step error — the lossless hygiene floor shared with SparseState (B).
constexpr double kMpsDefaultCutoff = 1e-12;

/// When the MPS keeps the mixed-canonical gauge through its two-site splits.
/// Auto arms it at the first split that actually discards weight: a lossless run
/// pays nothing and stays bit-identical to None, a lossy one is split optimally
/// from its first loss on.
enum class MpsGauge { None, Auto, Canonical };

/// Default for MpsTruncationConfig::gauge.
constexpr MpsGauge kMpsDefaultGauge = MpsGauge::Auto;

/// Accumulated sqrt(sum of squared step errors) above which the backend warns once and keeps running (C).
constexpr double kMpsErrorBudgetWarn = 0.1;

/// Structural qubit ceiling — StateIndex spans 2^128, and a shift of 128 would be UB (types.hpp; mirrors kMaxSparseQubits).
constexpr QubitIndex kMaxTensorNetworkQubits = 128;

/// Largest qubit subset an RDM may cover: the matrix holds 4^k complex entries,
/// so k=12 is already 16.7M entries (268 MB) and k=13 doubles that twice over.
constexpr QubitIndex kMaxRdmSites = 12;

namespace detail {

/// One leg of a tensor: a physical qubit leg (dim 2) or a bond shared with a neighbour (dim chi).
/// @ingroup backend
struct TensorIndex {
    /// Physical legs survive every contraction; bond legs are what contraction consumes.
    enum class Kind { Physical, Bond };

    Kind kind = Kind::Physical;

    /// Physical legs are always 2; bond legs carry the bond's current chi.
    std::uint32_t dim = 2;

    /// Qubit number for Physical, bond ordinal for Bond — identity that survives chi changes.
    std::uint32_t id = 0;
};

/// A dense tensor node: its index list plus row-major data, placed on CPU or GPU.
/// @ingroup backend
struct TensorNode {
    /// Placement is chosen per tensor size so small tensors stay off the PCIe path.
    enum class Device { CPU, GPU };

    std::vector<TensorIndex> indices;

    /// Row-major over `indices` — last index varies fastest.
    std::vector<std::complex<double>> data;

    Device device = Device::CPU;

    /// Product of all index dims: the length `data` must have. A node with no indices is a scalar (1).
    std::size_t element_count() const noexcept;
};

/// A shared index between two nodes — node_a's leg `index_a` is the same leg as node_b's `index_b`.
/// @ingroup backend
struct Bond {
    std::uint32_t node_a = 0;
    std::uint32_t index_a = 0;
    std::uint32_t node_b = 0;
    std::uint32_t index_b = 0;
};

/// Tensor network storage: an ordered node list plus the bonds contracting them.
///
/// An MPS is a TensorGraph whose nodes form a chain, so one type serves both the
/// 1D and the general topology instead of duplicating storage.
/// @ingroup backend
class TensorGraph {
public:
    /// Appends a node and returns its id; ids are stable because nodes are never removed.
    /// throws: SimulatorError — data length does not match the product of the index dims.
    std::uint32_t add_node(TensorNode node);

    /// Contracts two legs into a bond after checking both are Bond-kind, unbonded, and equal in dim.
    /// A dim mismatch is silent corruption once contraction runs, so it is rejected at wiring time.
    /// throws: SimulatorError — node/index out of range, non-bond leg, dim mismatch, or leg already bonded.
    void connect(std::uint32_t node_a, std::uint32_t index_a,
                 std::uint32_t node_b, std::uint32_t index_b);

    /// @return Number of nodes in the graph.
    std::size_t node_count() const noexcept { return nodes_.size(); }

    /// @return Number of bonds wired between nodes.
    std::size_t bond_count() const noexcept { return bonds_.size(); }

    /// Read access to one node.
    /// throws: SimulatorError — index out of range.
    const TensorNode& node(std::uint32_t id) const;

    /// Mutable access for gate application — the index list must stay consistent with existing bonds.
    /// throws: SimulatorError — index out of range.
    TensorNode& node(std::uint32_t id);

    /// @return All nodes in insertion order.
    const std::vector<TensorNode>& nodes() const noexcept { return nodes_; }

    /// @return All bonds in creation order.
    const std::vector<Bond>& bonds() const noexcept { return bonds_; }

    /// Largest chi over every bond leg — 1 for a product state, and what the cap bounds.
    std::uint32_t max_bond_dimension() const noexcept;

    /// True when the graph is a well-formed MPS chain: uniform [bond, physical, bond] sites,
    /// physical ids 0..n-1 in order, dim-1 boundary bonds, and exactly n-1 neighbour bonds.
    /// This is the structural invariant every MPS routine assumes, so it is checkable, not assumed.
    bool is_mps_chain() const noexcept;

    /// Builds |0...0> as an MPS chain with every bond at chi = 1.
    /// Boundary bonds are kept explicit at dim 1 so every site is a uniform 3-index
    /// tensor — the alternative (2-index end sites) forces a special case into every
    /// later routine.
    /// throws: SimulatorError — num_qubits is 0 or exceeds kMaxTensorNetworkQubits.
    static TensorGraph mps_zero_state(QubitIndex num_qubits);

private:
    std::vector<TensorNode> nodes_;
    std::vector<Bond>       bonds_;
};

class MpsState;  // mps.hpp — held by pointer so the backend header stays free of it

}  // namespace detail

/// MPS and general tensor-network backend; batch execute() only, Interactive entry points throw SimulatorError.
/// @ingroup backend
class TensorNetworkBackend : public SimulatorBackend {
public:
    /// @param gpu_svd offload the two-site SVD to cuSolver. Set
    ///        an explicit ProcType::GPU request; ProcType::Auto leaves it false.
    ///        Ignored on a CPU-only build or a host with no device -- the
    ///        offload declines by itself rather than failing the run.
    explicit TensorNetworkBackend(bool gpu_svd = false);
    ~TensorNetworkBackend() override;

    void   prepare(const Circuit& circuit) override;
    Result execute(const Circuit& circuit, std::uint32_t nshots,
                   const NoiseModel* noise = nullptr) override;

    // Interactive surface — always throws SimulatorError. Kept overridden so
    // QRegister::interactive can dispatch uniformly and trigger the error
    // at the first user call.
    void init_state   (QubitIndex num_qubits) override;
    void apply_gate   (const GateOp& op)      override;
    int  apply_measure(QubitIndex qubit)      override;
    void apply_reset  (QubitIndex qubit)      override;

    StateVector  get_statevector()                              override;
    RawStateView get_statevector_raw()                          override;
    double       expectation_value(std::string_view pauli)      override;

    // TensorNetwork does not support nonzero_amplitudes() — throws SimulatorError
    // num_qubits() reports the width prepare() built the chain for
    // (0 before prepare), since TN has no Interactive session to track.
    std::vector<std::pair<StateIndex, std::complex<double>>>
            nonzero_amplitudes() const override;
    // Single amplitude and RDM —
    // simulator_backend.hpp for the shared contract.
    std::complex<double> amplitude(StateIndex basis) const override;
    std::vector<std::complex<double>>
            reduced_density_matrix(std::vector<QubitIndex> qubits) const override;

    QubitIndex num_qubits() const override;

    SimType          sim_type()       const override;
    bool             supports_noise() const override;
    bool             supports_gpu()   const override;
    std::string_view name()           const override;

    // The structural ceiling, kMaxTensorNetworkQubits. An MPS holds
    // n * chi^2 amplitudes, not 2^n, so the host budget behind the dense formula
    // has no meaning here — reporting it said 34 on a host where this backend
    // runs 97-qubit surface-code circuits.
    //
    // A ceiling is not a guarantee: what actually limits TN is chi, and chi is
    // decided by the circuit. This function does not see a circuit, so it cannot
    // say more; a run that saturates the bond cap loses accuracy at any width
    // (warns shows the norm drift). Feasibility is caught
    // after the fact, because a circuit-dependent estimate is not
    // available for TN the way it is for SparseState.
    std::uint32_t max_qubits(detail::WorkingSet ws) const noexcept override;

    // Reproducible sampling: QRegister::run() seeds every backend
    // before execute(), and the seed is captured into the deferred draw.
    void set_seed(std::uint64_t seed) override;

    // MPS truncation knobs — highest precedence over env/defaults.
    void set_mps_truncation(std::uint32_t max_bond, double cutoff) override;

    // NonLinear site layout. Resolved from QPLAYER_TN_LAYOUT
    // construction; this setter exists for tests, not for QRegister.
    void set_nonlinear_layout(detail::NonLinearLayout layout) noexcept { nonlinear_layout_ = layout; }

    // Canonical-gauge splits. Resolved from QPLAYER_MPS_GAUGE
    // prepare(); this setter overrides it for tests, not for QRegister.
    void set_mps_gauge(MpsGauge gauge) noexcept { forced_gauge_ = gauge; }

    // MPO truncation knobs + route selection.
    void set_mpo_truncation(std::uint32_t max_bond, double cutoff) override;
    void set_tn_use_mpo(bool use_mpo) override;
    // Per-site noise interventions and draw ledger for the MC trajectory path.
    void set_noise_interventions(const std::vector<std::uint32_t>& sites,
                                 bool ledger) override;

    // Bond dimension the last MPO run reached — the quantity says
    // damping is supposed to bound, exposed so a test can measure it.
    std::uint32_t last_mpo_bond_dimension() const noexcept { return last_mpo_bond_; }

    // Largest chi any trajectory of the last Monte Carlo run reached.
    // truncation_errors() reports the accuracy axis; this is the resource axis,
    // and the two answer different questions — a chain sitting AT max_bond means
    // the cap decided the error, so a small reported error is not evidence the
    // run was easy. Above the dense cap there is no FullState reference to catch
    // that, which is exactly where the number is needed.
    std::uint32_t last_trajectory_bond_dimension() const noexcept { return last_traj_bond_; }

    // Worst |1 - |psi|^2| any trajectory of the last run showed BEFORE it was
    // renormalised. Nonzero only when truncation lost weight, so it
    // reads as "how far the local rescale drifted from the global norm".
    double last_trajectory_norm_drift() const noexcept { return last_traj_norm_drift_; }

    // Interaction topology prepare() derived, for tests and diagnostics.
    const detail::TopologyAnalysis& topology() const noexcept { return topology_; }

    // Adjacent SWAPs the MPS spent on distant operands; 0 when the chain order
    // matched the circuit's interaction path.
    std::size_t swap_steps() const noexcept;

    // Bit-vector overload of amplitude(), kept because the site-layout mapping
    // needs per-qubit access. `bitstring[q]` is qubit q's value, so it reads in
    // the same direction as a Circuit, not as a counts() key.
    // throws: SimulatorError — no circuit prepared, or length != num_qubits.
    std::complex<double> amplitude(const std::vector<int>& bitstring) const;


private:
    /// Monte Carlo trajectory path (—). Each shot replays
    /// the RAW op list on its own private MpsState with its own RNG: gate fusion
    /// is skipped deliberately, because a fused matrix has no gate name and
    /// NoiseModel::channels_for_gate() is keyed by name.
    /// Reset runs here in place: every QEC round reuses its ancilla,
    /// so without it a surface-code circuit has no noise-capable backend at all.
    /// throws: SimulatorError — an op family the noisy path does not support
    ///   (opaque / 3Q+, including as an IfOp body), or a trajectory whose norm
    /// diverged.
    Result execute_trajectories(const Circuit& circuit, std::uint32_t nshots,
                                const NoiseModel& noise);

    /// Per-shot path for circuits that reuse a measured qubit.
    ///
    /// Routed by Circuit::has_classical_control(), the same predicate the
    /// statevector backends use, so a
    /// mid-circuit circuit cannot end up on one collapse convention here and
    /// another there. Each shot evolves its own chain and collapses at every
    /// MeasureOp, which is why the "evolve once, sample many" saving of the fast
    /// path is unavailable — the shots no longer share a final state.
    /// Reset routes here too, even with no measurement reuse: it
    /// is a collapse, so evolving it once and drawing nshots would report a
    /// single branch nshots times.
    /// IfOp branches on THIS shot's classical register, which is the
    /// reason a conditional circuit cannot use the fast path at all: the
    /// register has no value until this shot's measurements have run.
    /// throws: SimulatorError — opaque or 3Q+ gates (including as an IfOp body),
    /// or a shot whose norm diverged.
    Result execute_per_shot(const Circuit& circuit, std::uint32_t nshots);

    /// Native MPS sampling for the noiseless path above the dense export cap
    /// Builds the sampler once and draws `nshots`
    /// shots from the chain, so no statevector is materialised at any width.
    /// throws: SimulatorError — the chain is malformed or has diverged.
    std::map<std::string, std::uint64_t> sample_counts_native(
        const detail::MpsState& state,
        std::uint32_t nshots,
        ClassicalBitIndex total_cubits,
        const std::vector<std::pair<QubitIndex, ClassicalBitIndex>>& measure_map) const;

    /// MPO density-matrix path. One exact evolution
    /// of rho rather than nshots sampled trajectories: channels are applied in
    /// full, so counts come from rho's diagonal and <P> is Tr(rho P) with no
    /// sampling error at all.
    /// throws: SimulatorError — an op family the noisy path does not support, or
    /// a chain whose trace has diverged.
    Result execute_mpo(const Circuit& circuit, std::uint32_t nshots, const NoiseModel& noise);

    /// Refuses state access when there is no single state to return — before
    /// prepare(), or after a Monte Carlo run whose trajectories each had their own.
    /// throws: SimulatorError — always, when either condition holds.
    void require_single_state(const char* what) const;

    /// Site-ordered amplitudes -> qubit-ordered, undoing the layout permutation.
    std::vector<std::complex<double>> to_qubit_order(
        std::vector<std::complex<double>> site_ordered) const;

    std::unique_ptr<detail::MpsState> state_;
    // Site layout chosen from the coupling graph: site i carries qubit
    // topology_.chain_order[i], and site_of_qubit_ is its inverse. Identity when
    // the topology is non-linear or already in order.
    detail::TopologyAnalysis          topology_;
    std::vector<QubitIndex>           site_of_qubit_;
    QubitIndex                        num_qubits_ = 0;
    std::uint64_t                     seed_       = 0;
    // nullopt until set_mps_truncation() is called; env/defaults apply otherwise.
    std::optional<std::uint32_t>      forced_max_bond_;
    std::optional<double>             forced_cutoff_;
    std::optional<MpsGauge>           forced_gauge_;
    bool                              warned_non_linear_ = false;
    // Set by execute_trajectories(): after a noisy run there is no single final
    // state — each trajectory evolved its own private chain — so the state-access
    // family must refuse rather than hand back the prepared |0...0>. FullState
    // fails loudly here too (its member buffers are never init'd on that path);
    // returning a plausible-looking wrong state would be the worse failure.
    bool                              trajectory_state_unavailable_ = false;
    // route. Resolved from QPLAYER_TN_NOISE_PATH at construction and
    // overridden by set_tn_use_mpo(); only consulted when a NoiseModel is given.
    bool                              use_mpo_       = false;
    std::vector<std::uint32_t>        noise_sites_;          // sorted
    bool                              noise_ledger_  = false;
    // identity unless QPLAYER_TN_LAYOUT=minla asked otherwise.
    detail::NonLinearLayout           nonlinear_layout_ = detail::NonLinearLayout::Identity;
    /// Explicit ProcType::GPU was requested, so the two-site SVD may be
    /// offloaded. Resolved once at construction because the factory
    /// is the only place that knows the requested processor.
    bool                              gpu_svd_ = false;
    std::uint32_t                     last_mpo_bond_ = 0;
    std::uint32_t                     last_traj_bond_ = 0;
    double                            last_traj_norm_drift_ = 0.0;
    std::optional<std::uint32_t>      forced_mpo_max_bond_;
    std::optional<double>             forced_mpo_cutoff_;
};

}  // namespace qplayer
