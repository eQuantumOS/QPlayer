/// @file device_model.hpp
/// @brief Hardware device characterisation.
/// @ingroup noise
///
/// Holds T1/T2 coherence, gate duration/error, and the coupling map. Loaded from
/// JSON (schema) via the static factories. Used by NoiseModel::load_device
/// and by SwapRouterPass for connectivity validation.

#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "types.hpp"

namespace qplayer {

/// Per-gate metadata used by ALAPSchedulePass and noise channel
/// synthesis.
struct GateProperty {
    /// Gate duration in nanoseconds.
    double duration_ns;
    /// 1 - gate fidelity.
    double error_rate;
};

/// Undirected pairs of physical qubits that admit two-qubit gates.
/// Empty set means full connectivity — SwapRouterPass treats it as a no-op.
using CouplingMap = std::vector<std::pair<QubitIndex, QubitIndex>>;

/// Hardware characterisation: T1/T2 coherence, per-gate duration and error,
/// and the coupling map. Loaded from the device JSON schema.
class DeviceModel {
public:
    /// Loads a device from a JSON file on disk (schema).
    /// @param path Path to the device JSON file.
    /// @return The parsed and validated device model.
    /// @throws CircuitError I/O failure, schema violation, or out-of-range value.
    static DeviceModel from_json_file(const std::string& path);

    /// Loads a device from an in-memory JSON document (schema).
    /// @param json_text The JSON document text.
    /// @return The parsed and validated device model.
    /// @throws CircuitError Schema violation or out-of-range value.
    static DeviceModel from_json_string(const std::string& json_text);

    /// Amplitude relaxation time of one qubit.
    /// @param q Physical qubit index.
    /// @return T1 in nanoseconds, or infinity for a qubit the device does not
    ///         describe, which yields an identity (noiseless) channel.
    double T1(QubitIndex q) const;

    /// Dephasing time of one qubit.
    /// @param q Physical qubit index.
    /// @return T2 in nanoseconds, or infinity for a qubit the device does not
    ///         describe, which yields an identity (noiseless) channel.
    double T2(QubitIndex q) const;

    /// Duration and error rate of one gate on one operand list.
    /// @param gate_name Native gate name as it appears in the device JSON.
    /// @param qubits    Operand list; order matters for asymmetric 2Q gates
    ///                  such as cx ctrl,tgt.
    /// @return The recorded duration and error rate.
    /// @throws CircuitError No entry exists — probe with has_gate_property().
    GateProperty gate_property(const std::string& gate_name,
                               std::vector<QubitIndex> qubits) const;

    /// Cheap probe for gate_property().
    /// @param gate_name Native gate name as it appears in the device JSON.
    /// @param qubits    Operand list, in the same order gate_property() expects.
    /// @return true when gate_property() will succeed for this key.
    bool has_gate_property(const std::string& gate_name,
                           std::vector<QubitIndex> qubits) const;

    /// Native operation names the hardware executes directly.
    ///
    /// BasisGateTranslationPass decomposes logical gates into this set.
    /// @return The basis gate names (e.g. {id, rz, sx, x, ecr}); empty when the
    ///         device JSON omits 'basis_gates'.
    const std::vector<std::string>& basis_gates() const;

    /// Whether a native entangling gate is declared.
    /// @return true when two_qubit_gate() will succeed.
    bool has_two_qubit_gate() const;

    /// Native entangling gate name (e.g. "ecr", "cx").
    /// @return Reference to the stored gate name.
    /// @throws CircuitError The device declares no native 2Q gate.
    const std::string& two_qubit_gate() const;

    /// Whether readout confusion is declared for a qubit.
    /// @param q Physical qubit index.
    /// @return false when both probabilities are zero or the qubit is absent.
    bool has_readout_error(QubitIndex q) const;

    /// Readout confusion probabilities of one qubit.
    /// @param q Physical qubit index.
    /// @return {p01, p10} where p01 = P(report 0 | true 1) and
    ///         p10 = P(report 1 | true 0); {0, 0} for an absent qubit.
    std::pair<double, double> readout_error(QubitIndex q) const;

    /// Whether the device restricts two-qubit connectivity.
    /// @return false for full connectivity, which SwapRouterPass treats as a
    ///         no-op; true when coupling_map() will succeed.
    bool has_coupling_map() const;

    /// Undirected physical qubit pairs that admit two-qubit gates.
    /// @return Reference to the stored coupling map.
    /// @throws CircuitError The device declares full connectivity.
    const CouplingMap& coupling_map() const;

    /// Total qubits described by this device.
    /// @return The device width.
    QubitIndex num_qubits() const;

private:
    /// NoiseModel::load_device enumerates the gate table + per-qubit T1/T2 to
    /// synthesise relaxation Kraus channels. friend access keeps that
    /// enumeration possible without widening the public DeviceModel surface.
    friend class NoiseModel;

    /// Members materialise during the from_json_* factories; layout chosen to
    /// give O(1) per-qubit lookup without hashing for small devices (~100 qubits).
    QubitIndex                num_qubits_ = 0;
    std::vector<double>       t1_;             // Indexed by qubit; infinity when absent.
    std::vector<double>       t2_;
    /// Gate property registry. Keys are either the bare gate name ("cx") or the
    /// qubit-specific form "cx_0_1" (schema). gate_property() prefers the
    /// qubit-specific key and falls back to the bare name.
    std::unordered_map<std::string, GateProperty> gate_props_;
    CouplingMap               coupling_map_;
    bool                      has_coupling_map_ = false;
    /// Native basis gate set + designated 2Q gate. two_qubit_gate_ is
    /// meaningful only when has_two_qubit_gate_ (absent/null in the JSON otherwise).
    std::vector<std::string>  basis_gates_;
    std::string               two_qubit_gate_;
    bool                      has_two_qubit_gate_ = false;
    /// Per-qubit readout confusion, indexed by qubit; 0 when absent.
    std::vector<double>       prob_meas0_prep1_;   // p01 = P(report 0 | true 1)
    std::vector<double>       prob_meas1_prep0_;   // p10 = P(report 1 | true 0)
};

}  // namespace qplayer
