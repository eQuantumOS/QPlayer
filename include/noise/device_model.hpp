// device_model.hpp — Hardware device characterisation.
// Holds T1/T2 coherence, gate duration/error, and the coupling map. Loaded from
// JSON (schema) via the static factories. Used by NoiseModel::load_device
// and by SwapRouterPass for connectivity validation.

#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "types.hpp"

namespace qplayer {

// Per-gate metadata used by IdleNoiseInserter and noise channel
// synthesis.
struct GateProperty {
    double duration_ns;     // Gate duration in nanoseconds.
    double error_rate;      // 1 - gate fidelity.
};

// Undirected pairs of physical qubits that admit two-qubit gates.
// Empty set means full connectivity — SwapRouterPass treats it as a no-op.
using CouplingMap = std::vector<std::pair<QubitIndex, QubitIndex>>;

class DeviceModel {
public:
    // ── Factories (JSON schema) ────────────────────────────
    // from_json_file: read + parse + validate. Throws CircuitError on any I/O,
    // schema, or value-range violation.
    static DeviceModel from_json_file  (const std::string& path);
    static DeviceModel from_json_string(const std::string& json_text);

    // ── Coherence times ──────────────────────────────────
    // Unknown qubits return std::numeric_limits<double>::infinity() so the
    // T1/T2 -> Kraus conversion produces an identity channel (no noise).
    double T1(QubitIndex q) const;
    double T2(QubitIndex q) const;

    // ── Per-gate properties ──────────────────────────────
    // qubits identifies the operand list — order matters for asymmetric 2Q
    // gates (e.g. cx ctrl,tgt). has_gate_property() is the cheap probe.
    GateProperty gate_property    (const std::string& gate_name,
                                   std::vector<QubitIndex> qubits) const;
    bool         has_gate_property(const std::string& gate_name,
                                   std::vector<QubitIndex> qubits) const;

    // ── Native basis gate set ─────────────────
    // The hardware's native operation names (e.g. {id, rz, sx, x, ecr}).
    // BasisGateTranslationPass decomposes logical gates into these.
    // Empty when the JSON omits 'basis_gates'.
    const std::vector<std::string>& basis_gates() const;
    // two_qubit_gate() names the native entangling gate (e.g. "ecr", "cx").
    // has_two_qubit_gate() == false when absent/null; the accessor then throws.
    bool                            has_two_qubit_gate() const;
    const std::string&              two_qubit_gate()     const;

    // ── Readout confusion ─────────────────────
    // {prob_meas0_prep1, prob_meas1_prep0} = {p01, p10} in NoiseModel terms
    // (p01 = P(report 0 | true 1), p10 = P(report 1 | true 0)). Absent qubits
    // return {0, 0}. has_readout_error() is false when both probs are zero.
    bool                      has_readout_error(QubitIndex q) const;
    std::pair<double, double> readout_error    (QubitIndex q) const;

    // ── Coupling map ─────────────────────────────────────
    // has_coupling_map() == false signals full connectivity. coupling_map()
    // throws CircuitError when called in that state.
    bool                has_coupling_map() const;
    const CouplingMap&  coupling_map()     const;

    // Total qubits described by this device.
    QubitIndex num_qubits() const;

private:
    // NoiseModel::load_device enumerates the gate table + per-qubit T1/T2 to
    // synthesise relaxation Kraus channels. friend access keeps that
    // enumeration possible without widening the public DeviceModel surface.
    friend class NoiseModel;

    // Members materialise during the from_json_* factories; layout chosen to
    // give O(1) per-qubit lookup without hashing for small devices (~100 qubits).
    QubitIndex                num_qubits_ = 0;
    std::vector<double>       t1_;             // Indexed by qubit; infinity when absent.
    std::vector<double>       t2_;
    // Gate property registry. Keys are either the bare gate name ("cx") or the
    // qubit-specific form "cx_0_1" (schema). gate_property() prefers the
    // qubit-specific key and falls back to the bare name.
    std::unordered_map<std::string, GateProperty> gate_props_;
    CouplingMap               coupling_map_;
    bool                      has_coupling_map_ = false;
    // Native basis gate set + designated 2Q gate. two_qubit_gate_ is
    // meaningful only when has_two_qubit_gate_ (absent/null in the JSON otherwise).
    std::vector<std::string>  basis_gates_;
    std::string               two_qubit_gate_;
    bool                      has_two_qubit_gate_ = false;
    // Per-qubit readout confusion, indexed by qubit; 0 when absent.
    std::vector<double>       prob_meas0_prep1_;   // p01 = P(report 0 | true 1)
    std::vector<double>       prob_meas1_prep0_;   // p10 = P(report 1 | true 0)
};

}  // namespace qplayer
