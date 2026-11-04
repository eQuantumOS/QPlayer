/// @file noise_model.hpp
/// @brief Gate-name-keyed Kraus channel registry.
/// @ingroup noise
///
/// Holds depolarising / damping / readout error channels and converts T1/T2
/// device data into Amplitude Damping + Dephasing Kraus channels via
/// load_device(). NoiseModel does not own a NoiseChannel
/// hierarchy — every entry collapses to a KrausChannel for uniform backend
/// dispatch.

#pragma once

#include <complex>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "types.hpp"
#include "noise/device_model.hpp"

namespace qplayer {

/// Single arbitrary-dimension complex matrix used as a Kraus operator or an
/// opaque unitary. The same alias is reused by OpaqueDefinition.body
/// (+) — kept here so circuit/gate_op.hpp can simply
/// include noise/noise_model.hpp instead of duplicating the typedef.
using UnitaryMatrix = std::vector<std::vector<std::complex<double>>>;

/// Channel discriminator for diagnostics / logging. Backends never dispatch on
/// this — they always apply the Kraus operator list. The tag is preserved so
/// JSON dumps and log messages can identify the channel origin.
enum class ChannelType {
    Identity,
    Depolarizing,
    AmplitudeDamping,
    PhaseDamping,
    BitFlip,
    PhaseFlip,
    Readout,
    Custom,        // User-supplied via add_kraus().
};

/// Kraus channel: list of Kraus operators K_i with ΣK_i^† K_i = I.
/// target_qubit is the *logical* operand of the channel; for two-qubit channels
/// the operators encode the joint action (operand list lives on the GateOp).
/// +.
struct KrausChannel {
    /// Kraus operators K_i of the channel, in application order.
    std::vector<UnitaryMatrix> operators;
    /// Logical operand the channel acts on; local bit 0 for two-qubit channels.
    QubitIndex                 target_qubit  = 0;
    /// Second operand for two-qubit (4x4-operator) channels (e.g. 2Q depolarizing,
    /// /). Unused for single-qubit channels. Convention: target_qubit
    /// is local bit 0, target_qubit2 is local bit 1 (matches the dense-2Q kernels).
    QubitIndex                 target_qubit2 = 0;
    /// Which channel family this is, for diagnostics and backend dispatch.
    ChannelType                type          = ChannelType::Custom;
};

/// Apply readout confusion to a true measurement outcome.
/// Given a uniform draw u in [0,1), return the (possibly flipped) reported bit.
/// p01 = P(report 0 | true 1), p10 = P(report 1 | true 0). Shared by the CPU and
/// GPU Monte Carlo paths so both report identical statistics (parity).
/// @param true_outcome The measured bit before confusion, 0 or 1.
/// @param p01          P(report 0 | true 1).
/// @param p10          P(report 1 | true 0).
/// @param u            Uniform draw in [0, 1) supplied by the caller's RNG.
/// @return The bit to report, 0 or 1.
inline int apply_readout_error(int true_outcome, double p01, double p10, double u) {
    if (true_outcome == 1) return (u < p01) ? 0 : 1;
    return (u < p10) ? 1 : 0;
}

/// Gate-name-keyed registry of Kraus channels applied during noisy execution
/// Every entry collapses to a KrausChannel for uniform dispatch.
class NoiseModel {
public:
    /// Constructs an empty model that registers no channels.
    NoiseModel();

    /// @name Built-in channel registration (..)
    ///
    /// Every function in this group shares one contract: `gate_name` matches
    /// GateOp.name (e.g. "h", "cx"); an empty `qubits` filter applies the channel
    /// to every operand of the named gate (the F-NS global rule), while a
    /// non-empty one restricts it to those operands; the magnitude argument is
    /// validated immediately, so an out-of-range value throws CircuitError before
    /// the model is mutated; and each returns `*this` so registrations chain.
    /// @{

    /// Registers a depolarizing channel of probability @p p.
    NoiseModel& add_depolarizing(const std::string&      gate_name,
                                 double                  p,
                                 std::vector<QubitIndex> qubits = {});
    /// Registers amplitude damping (T1 relaxation) of strength @p gamma.
    NoiseModel& add_amplitude_damping(const std::string&      gate_name,
                                      double                  gamma,
                                      std::vector<QubitIndex> qubits = {});
    /// Registers phase damping (pure dephasing) of strength @p lam.
    NoiseModel& add_phase_damping(const std::string&      gate_name,
                                  double                  lam,
                                  std::vector<QubitIndex> qubits = {});
    /// Registers a bit-flip (Pauli X) channel of probability @p p.
    NoiseModel& add_bit_flip(const std::string&      gate_name,
                             double                  p,
                             std::vector<QubitIndex> qubits = {});
    /// Registers a phase-flip (Pauli Z) channel of probability @p p.
    NoiseModel& add_phase_flip(const std::string&      gate_name,
                               double                  p,
                               std::vector<QubitIndex> qubits = {});
    /// @}

    /// Registers readout confusion for one qubit.
    ///
    /// Keyed by qubit rather than by gate, because confusion is applied to the
    /// reported classical bit at measurement time.
    /// @param qubit            Physical qubit the confusion applies to.
    /// @param p_meas_0_given_1 P(report 0 | true 1).
    /// @param p_meas_1_given_0 P(report 1 | true 0).
    /// @return *this, so registrations chain.
    /// @throws CircuitError Either probability lies outside [0, 1].
    NoiseModel& add_readout_error(QubitIndex qubit,
                                  double     p_meas_0_given_1,
                                  double     p_meas_1_given_0);

    /// Registers a caller-supplied Kraus channel.
    /// @param gate_name Gate name to attach the channel to.
    /// @param channel   Operators to apply; validated as trace-preserving
    ///                  (sum of K^dagger K within 1e-10 of the identity).
    /// @param qubits    Operand filter; empty applies to every operand.
    /// @return *this, so registrations chain.
    /// @throws CircuitError The operators are not trace-preserving.
    NoiseModel& add_kraus(const std::string&      gate_name,
                          const KrausChannel&     channel,
                          std::vector<QubitIndex> qubits = {});

    /// Synthesises channels from a hardware characterisation.
    ///
    /// Converts per-qubit T1/T2 and per-gate duration into amplitude damping plus
    /// dephasing, adds the excess-over-relaxation depolarizing part and readout
    /// confusion, all keyed by the device's native basis gates.
    /// @param device Device model to ingest.
    /// @return *this, so registrations chain.
    NoiseModel& load_device(const DeviceModel& device);

    /// Re-key a physical-qubit noise model into the dense [0,k) space produced
    /// by QubitTruncationPass (handoff). dense_to_physical
    /// maps each dense qubit d -> its original physical qubit (the exact map
    /// stamped into CircuitMetadata.qubit_truncation). The returned model carries
    /// the same channels/readout/T1/T2 re-indexed by dense qubit, so a backend
    /// executing the truncated circuit (dense operands) still looks up each
    /// qubit's real device parameters. Qubit-specific entries whose physical
    /// target is absent from the map are dropped; global (empty-filter) entries
    /// are copied verbatim. Identity maps yield an equivalent model.
    /// @param dense_to_physical Map from dense qubit index to its original
    ///                          physical qubit, as stamped into
    ///                          CircuitMetadata.qubit_truncation.
    /// @return An equivalent model keyed by dense qubit index.
    NoiseModel project_to_dense(
        const std::unordered_map<QubitIndex, QubitIndex>& dense_to_physical) const;

    /// Whether any channel is registered for a gate name.
    /// @param gate_name Gate name as it appears in GateOp.name.
    /// @return true when channels_for_gate() can return a non-empty list.
    bool has_channels_for_gate(const std::string& gate_name) const;
    /// Hot path: called for every noisy gate of every trajectory. The operand list
    /// used to be taken BY VALUE (one vector copy per call) and the result is a
    /// fresh vector of KrausChannel, each holding nested matrices (audit #27).
    /// The operand copy is gone; for the per-trajectory loops use the out-parameter
    /// overload with a hoisted buffer so the vector allocation is paid once per
    /// run instead of once per gate.
    /// @param gate_name Gate name as it appears in GateOp.name.
    /// @param qubits     The executing gate's operand list, in order.
    /// @return Channels to apply, in registration order.
    std::vector<KrausChannel>
         channels_for_gate(const std::string&             gate_name,
                           const std::vector<QubitIndex>& qubits) const;

    /// Same result, written into @p out so the caller can reuse capacity across
    /// calls. Behaviour and ordering are identical to the returning overload.
    /// @param gate_name Gate name as it appears in GateOp.name.
    /// @param qubits     The executing gate's operand list, in order.
    /// @param out        Destination, cleared before use.
    void channels_for_gate(const std::string&             gate_name,
                           const std::vector<QubitIndex>& qubits,
                           std::vector<KrausChannel>&     out) const;

    /// Explicit Kraus channel for one idle window, keyed by (qubit, duration)
    /// Registered channels take precedence over the T1/T2
    /// synthesis in idle_channels(), so a caller can replace an idle window's
    /// noise entirely -- the product stores and serves, it does not transform.
    /// The duration key is quantised to `llround(duration_ns * 1e6)` (1 fs)
    /// because an idle window's length is a sum of device durations produced by
    /// ALAPSchedulePass, which need not be the bit-identical double a caller
    /// read back from Circuit::to_json().
    /// Register AFTER load_device(): load_device() clears this registry.
    /// @param qubit        Physical qubit that idles.
    /// @param duration_ns  Idle duration in nanoseconds; must be finite and >= 0.
    /// @param channel      Channel to serve for that window. Multiple calls on
    ///                     one key accumulate and are served in registration order.
    /// @return *this, for chaining.
    /// @throws CircuitError when duration_ns is negative or not finite.
    NoiseModel& add_idle_kraus(QubitIndex qubit, double duration_ns,
                               const KrausChannel& channel);

    /// Whether an explicit idle channel is registered for (qubit, duration)
    /// Callers verify coverage with this before running: an
    /// unregistered window falls back to T1/T2 synthesis silently, by design.
    /// @param qubit        Physical qubit.
    /// @param duration_ns  Idle duration in nanoseconds, quantised as above.
    /// @return true when at least one channel is registered for that key.
    bool has_idle_kraus(QubitIndex qubit, double duration_ns) const;

    /// Whether per-qubit coherence data was loaded, gating idle-noise insertion
    /// (the inserter is ALAPSchedulePass since Phase 8-F).
    /// @return true when load_device() supplied T1/T2 values.
    bool has_t1_t2() const;

    /// Readout error lookup. Backends apply confusion to the
    /// reported classical bit at MeasureOp time; the quantum collapse stays on
    /// the true outcome. has_readout_errors() gates the per-measure lookup.
    /// @return true when at least one qubit has readout confusion registered.
    bool has_readout_errors() const;

    /// Readout confusion probabilities of one qubit.
    /// @param q Physical qubit index.
    /// @return {P(report 0 | true 1), P(report 1 | true 0)}; {0, 0} when none is
    ///         registered for @p q.
    std::pair<double, double> readout_error(QubitIndex q) const;

    /// Per-qubit idle relaxation Kraus channels.
    /// An explicit registration for (q, duration_ns) wins: those channels are
    /// returned in registration order and the T1/T2 synthesis is skipped
    /// With an empty registry the behaviour is bit-identical
    /// before that ADR.
    /// Returns T1T2Calculator::compute(t1_[q], t2_[q], duration_ns, q) for qubit q.
    /// Returns {} when q is out of range or when coherence is infinite (no noise).
    /// Called by backends to dispatch IdleOp nodes during Monte Carlo trajectories.
    /// @param q           Physical qubit that idles.
    /// @param duration_ns  Idle duration in nanoseconds.
    /// @return Relaxation channels for the idle window; empty when @p q is out of
    ///         range or its coherence is infinite (no noise).
    std::vector<KrausChannel> idle_channels(QubitIndex q, double duration_ns) const;

private:
    // Storage shape decided at implementation time — declared private so the
    // header stays free of dependency leaks. leaves the exact map
    // type open to allow swapping unordered_map for a denser representation
    // once perf data lands (Phase 8).

    /// A registered channel plus its qubit filter. qubits empty -> applies to
    /// every operand of the named gate (the F-NS global rule). channel
    /// holds the operator template; target_qubit is filled per operand at query.
    struct GateEntry {
        /// Operator template; target_qubit is filled per operand at query time.
        KrausChannel            channel;
        /// Operand filter, or an ordered operand signature when
        /// edge_signature is true; empty means the global rule.
        std::vector<QubitIndex> qubits;
        /// When true, `qubits` is an ORDERED gate-operand signature (a specific
        /// device edge, e.g. cx_0_1) rather than a per-operand filter set: the
        /// channel fires (with channel.target_qubit pre-set) only when the
        /// executing gate's operands equal `qubits` in order. load_device binds
        /// a 2Q gate's relaxation to its exact edge+direction so a qubit shared
        /// across edges is damped once per gate, not once per incident edge
        /// (#1 fix). Default false keeps the user-API per-qubit / global
        /// rules unchanged.
        bool                    edge_signature = false;
    };
    /// Per-gate registry keyed by GateOp.name. vector preserves registration
    /// order so channels_for_gate() returns a deterministic sequence.
    std::unordered_map<std::string, std::vector<GateEntry>> gate_channels_;

    /// Readout error is qubit-keyed and applied to measurement
    /// outcomes rather than the statevector, so it lives outside gate_channels_.
    struct ReadoutError {
        /// P(report 0 | true 1).
        double p_meas_0_given_1 = 0.0;
        /// P(report 1 | true 0).
        double p_meas_1_given_0 = 0.0;
    };
    std::unordered_map<QubitIndex, ReadoutError> readout_errors_;

    /// Explicit idle channels keyed by (qubit, quantised duration).
    /// vector preserves registration order, matching gate_channels_' contract.
    /// The key is integral so that two callers computing the same nanosecond
    /// window through different arithmetic still land on one entry.
    std::map<std::pair<QubitIndex, std::int64_t>, std::vector<KrausChannel>> idle_channels_;

    bool has_t1_t2_ = false;

    /// Per-qubit T1/T2 coherence times (nanoseconds) stored by load_device()
    /// Indexed by QubitIndex; infinity when absent.
    /// Sized to device.num_qubits() at load_device() time; backends access
    /// via idle_channels(q, duration_ns) rather than directly (encapsulation).
    std::vector<double> t1_;
    std::vector<double> t2_;
};

}  // namespace qplayer
