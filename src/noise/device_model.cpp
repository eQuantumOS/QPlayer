// device_model.cpp — DeviceModel JSON ingestion + query.
// Parses the schema via the in-house
// detail::JsonValue parser (bars external JSON libraries) into dense
// T1/T2 vectors, a gate-property registry, and a coupling map. Value-range
// violations (non-positive T1/T2, error_rate outside [0,1], negative duration
// or qubit index) throw CircuitError at load time.

#include "noise/device_model.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "circuit/detail/json_value.hpp"
#include "error.hpp"

namespace qplayer {

namespace {

using detail::JsonValue;

// Accept int or double JSON numbers as a double; anything else is a schema error.
double as_number(const JsonValue& v, const std::string& field) {
    if (v.is_int())    return static_cast<double>(v.as_int());
    if (v.is_double()) return v.as_double();
    throw CircuitError{"DeviceModel: '" + field + "' must be a number"};
}

// Coherence times must be strictly positive — T1/T2 <= 0 is unphysical and would
// make the T1/T2 -> Kraus conversion divide by zero.
double read_coherence(const JsonValue& v, const std::string& field) {
    const double x = as_number(v, field);
    if (!(x > 0.0)) {
        throw CircuitError{"DeviceModel: '" + field + "' must be > 0, got " +
                           std::to_string(x)};
    }
    return x;
}

double read_duration(const JsonValue& v) {
    const double x = as_number(v, "duration_ns");
    if (!(x >= 0.0)) {
        throw CircuitError{"DeviceModel: 'duration_ns' must be >= 0, got " +
                           std::to_string(x)};
    }
    return x;
}

double read_error_rate(const JsonValue& v) {
    const double x = as_number(v, "error_rate");
    if (!(x >= 0.0 && x <= 1.0)) {
        throw CircuitError{"DeviceModel: 'error_rate' must be in [0, 1], got " +
                           std::to_string(x)};
    }
    return x;
}

// Readout confusion probability must be a [0, 1] number.
double read_probability(const JsonValue& v, const std::string& field) {
    const double x = as_number(v, field);
    if (!(x >= 0.0 && x <= 1.0)) {
        throw CircuitError{"DeviceModel: '" + field + "' must be in [0, 1], got " +
                           std::to_string(x)};
    }
    return x;
}

// Coupling-map qubit indices must be non-negative integers.
QubitIndex read_qubit_index(const JsonValue& v) {
    if (!v.is_int() || v.as_int() < 0) {
        throw CircuitError{"DeviceModel: coupling_map qubit index must be a "
                           "non-negative integer"};
    }
    return static_cast<QubitIndex>(v.as_int());
}

// Qubit-specific gate key: "cx" + operands -> "cx_0_1" (schema).
std::string make_gate_key(const std::string& name, const std::vector<QubitIndex>& qubits) {
    std::string key = name;
    for (QubitIndex q : qubits) {
        key += '_';
        key += std::to_string(q);
    }
    return key;
}

}  // namespace

DeviceModel DeviceModel::from_json_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw CircuitError{"DeviceModel: cannot open file '" + path + "'"};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return from_json_string(ss.str());
}

DeviceModel DeviceModel::from_json_string(const std::string& json_text) {
    // parse_json throws CircuitError("JSON syntax error at offset N: ...") so
    // syntax failures propagate with byte-offset diagnostics already attached.
    const JsonValue root = detail::parse_json(json_text);
    if (!root.is_object()) {
        throw CircuitError{"DeviceModel: top-level JSON must be an object"};
    }

    DeviceModel dm;

    // ── qubits: [{T1, T2}, ...] — dense per-index coherence times ────────────
    if (root.contains("qubits")) {
        const JsonValue& qarr = root.at("qubits");
        if (!qarr.is_array()) {
            throw CircuitError{"DeviceModel: 'qubits' must be an array"};
        }
        for (const JsonValue& q : qarr.as_array()) {
            if (!q.is_object()) {
                throw CircuitError{"DeviceModel: each 'qubits' entry must be an object"};
            }
            // Absent T1/T2 -> infinity (no noise on that channel).
            double t1 = std::numeric_limits<double>::infinity();
            double t2 = std::numeric_limits<double>::infinity();
            if (q.contains("T1")) t1 = read_coherence(q.at("T1"), "T1");
            if (q.contains("T2")) t2 = read_coherence(q.at("T2"), "T2");
            dm.t1_.push_back(t1);
            dm.t2_.push_back(t2);

            // Readout confusion. Prefer the asymmetric probabilities;
            // fall back to the symmetric 'readout_error' for either that is absent
            // (the generator emits all three, but symmetric-only JSON stays valid).
            double p01 = 0.0, p10 = 0.0;
            if (q.contains("readout_error")) {
                p01 = p10 = read_probability(q.at("readout_error"), "readout_error");
            }
            if (q.contains("prob_meas0_prep1")) {
                p01 = read_probability(q.at("prob_meas0_prep1"), "prob_meas0_prep1");
            }
            if (q.contains("prob_meas1_prep0")) {
                p10 = read_probability(q.at("prob_meas1_prep0"), "prob_meas1_prep0");
            }
            dm.prob_meas0_prep1_.push_back(p01);
            dm.prob_meas1_prep0_.push_back(p10);
        }
    }
    dm.num_qubits_ = static_cast<QubitIndex>(dm.t1_.size());

    // ── gates: {name|"name_q0_q1": {duration_ns, error_rate}} ────────────────
    if (root.contains("gates")) {
        const JsonValue& gates = root.at("gates");
        if (!gates.is_object()) {
            throw CircuitError{"DeviceModel: 'gates' must be an object"};
        }
        for (const auto& kv : gates.as_object()) {
            const std::string& key = kv.first;
            const JsonValue&   val = kv.second;
            if (!val.is_object()) {
                throw CircuitError{"DeviceModel: gate '" + key + "' must be an object"};
            }
            GateProperty gp{0.0, 0.0};
            if (val.contains("duration_ns")) gp.duration_ns = read_duration(val.at("duration_ns"));
            if (val.contains("error_rate"))  gp.error_rate  = read_error_rate(val.at("error_rate"));
            dm.gate_props_[key] = gp;
        }
    }

    // ── coupling_map: [[a, b], ...] — absent or empty means full connectivity ─
    if (root.contains("coupling_map")) {
        const JsonValue& cm = root.at("coupling_map");
        if (!cm.is_array()) {
            throw CircuitError{"DeviceModel: 'coupling_map' must be an array"};
        }
        for (const JsonValue& pair : cm.as_array()) {
            if (!pair.is_array() || pair.as_array().size() != 2) {
                throw CircuitError{"DeviceModel: each coupling_map entry must be "
                                   "a [a, b] pair"};
            }
            const QubitIndex a = read_qubit_index(pair.as_array()[0]);
            const QubitIndex b = read_qubit_index(pair.as_array()[1]);
            dm.coupling_map_.emplace_back(a, b);
            // A coupling pair may reference qubits beyond the 'qubits' array.
            const QubitIndex hi = std::max(a, b);
            if (hi + 1 > dm.num_qubits_) dm.num_qubits_ = hi + 1;
        }
    }
    // Present-but-empty coupling_map collapses to full connectivity,
    // identical to the absent case — has_coupling_map() then returns false.
    dm.has_coupling_map_ = !dm.coupling_map_.empty();

    // ── basis_gates: ["id","rz","sx","x","ecr",...] ──────────────
    if (root.contains("basis_gates")) {
        const JsonValue& bg = root.at("basis_gates");
        if (!bg.is_array()) {
            throw CircuitError{"DeviceModel: 'basis_gates' must be an array"};
        }
        for (const JsonValue& g : bg.as_array()) {
            if (!g.is_string()) {
                throw CircuitError{"DeviceModel: each 'basis_gates' entry must be "
                                   "a string"};
            }
            dm.basis_gates_.push_back(g.as_string());
        }
    }

    // ── two_qubit_gate: "ecr" | "cx" | null ───────────────────────
    // JSON null (device has no 2Q gate) leaves has_two_qubit_gate_ == false.
    if (root.contains("two_qubit_gate")) {
        const JsonValue& tq = root.at("two_qubit_gate");
        if (tq.is_string()) {
            dm.two_qubit_gate_     = tq.as_string();
            dm.has_two_qubit_gate_ = true;
        } else if (!tq.is_null()) {
            throw CircuitError{"DeviceModel: 'two_qubit_gate' must be a string or null"};
        }
    }

    return dm;
}

double DeviceModel::T1(QubitIndex q) const {
    return q < t1_.size() ? t1_[q] : std::numeric_limits<double>::infinity();
}

double DeviceModel::T2(QubitIndex q) const {
    return q < t2_.size() ? t2_[q] : std::numeric_limits<double>::infinity();
}

GateProperty DeviceModel::gate_property(const std::string&      gate_name,
                                        std::vector<QubitIndex> qubits) const {
    auto it = gate_props_.find(make_gate_key(gate_name, qubits));
    if (it != gate_props_.end()) return it->second;
    it = gate_props_.find(gate_name);
    if (it != gate_props_.end()) return it->second;
    throw CircuitError{"DeviceModel: no gate property for '" + gate_name +
                       "' on the given qubits"};
}

bool DeviceModel::has_gate_property(const std::string&      gate_name,
                                    std::vector<QubitIndex> qubits) const {
    if (gate_props_.count(make_gate_key(gate_name, qubits)) != 0) return true;
    return gate_props_.count(gate_name) != 0;
}

const std::vector<std::string>& DeviceModel::basis_gates() const {
    return basis_gates_;
}

bool DeviceModel::has_two_qubit_gate() const { return has_two_qubit_gate_; }

const std::string& DeviceModel::two_qubit_gate() const {
    if (!has_two_qubit_gate_) {
        throw CircuitError{"DeviceModel: no two_qubit_gate declared; guard with "
                           "has_two_qubit_gate()"};
    }
    return two_qubit_gate_;
}

bool DeviceModel::has_readout_error(QubitIndex q) const {
    const double p01 = q < prob_meas0_prep1_.size() ? prob_meas0_prep1_[q] : 0.0;
    const double p10 = q < prob_meas1_prep0_.size() ? prob_meas1_prep0_[q] : 0.0;
    return p01 > 0.0 || p10 > 0.0;
}

std::pair<double, double> DeviceModel::readout_error(QubitIndex q) const {
    const double p01 = q < prob_meas0_prep1_.size() ? prob_meas0_prep1_[q] : 0.0;
    const double p10 = q < prob_meas1_prep0_.size() ? prob_meas1_prep0_[q] : 0.0;
    return {p01, p10};
}

bool DeviceModel::has_coupling_map() const { return has_coupling_map_; }

const CouplingMap& DeviceModel::coupling_map() const {
    if (!has_coupling_map_) {
        throw CircuitError{"DeviceModel: no coupling map (full connectivity); "
                           "guard with has_coupling_map()"};
    }
    return coupling_map_;
}

QubitIndex DeviceModel::num_qubits() const { return num_qubits_; }

}  // namespace qplayer
