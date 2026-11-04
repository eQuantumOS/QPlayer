// serializer.cpp -- Circuit IR <-> JSON round-trip.
// Implements Circuit::to_json / from_json / load_json_file by translating
// Op variant nodes, ClassicalRegister entries, OpaqueDefinition entries,
// and CircuitMetadata sidecar fields to/from qplayer::detail::JsonValue.
//
// Schema reference (canonical form):
//   {
//     "version":              1,
//     "num_qubits":           <uint32>,
//     "classical_registers":  [ {"name", "width", "offset"}, ... ],
//     "opaque_definitions":   [ {"name", "num_qubits", "body": {...}}, ... ],
//     "metadata":             { "name", "layout"?, "coupling_map"?, "device_model_ref"? },
// "expectation_targets": [ <pauli-string>,... ],
//     "ops":                  [ <op>, ... ]
//   }
// Op encodings:
//   gate:    {"type":"gate", "name":<str>, "gate_type":<int>, "qubits":[...],
//             "params":[...], "opaque_name":<str>?, "duration_ns":<double>?,
//             "is_fused":<bool>, "is_opaque":<bool>}
//   measure: {"type":"measure", "qubit":<uint32>, "cubit":<uint32>}
//   barrier: {"type":"barrier", "qubits":[...]}
//   if:      {"type":"if", "creg":<uint32>, "value":<int64>, "body":<gate-op>}
// idle: {"type":"idle", "qubit":<uint32>, "duration_ns":<double>}
// Opaque body discriminator:
//   declaration_only: {"type":"declaration_only"}
//   matrix:           {"type":"matrix",  "rows":[ [[re,im], ...], ... ]}
//   kraus:            {"type":"kraus",   "operators":[ <matrix>, ... ]}

#include "circuit/circuit.hpp"

#include <complex>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

#include "circuit/detail/json_value.hpp"
#include "error.hpp"

namespace qplayer {

namespace {

constexpr int kSchemaVersion = 1;

using detail::JsonArray;
using detail::JsonKind;
using detail::JsonObject;
using detail::JsonValue;

// ── Encode helpers ──────────────────────────────────────────────────────────

JsonValue encode_complex(const std::complex<double>& c) {
    JsonArray pair;
    pair.emplace_back(c.real());
    pair.emplace_back(c.imag());
    return JsonValue(std::move(pair));
}

JsonValue encode_matrix(const UnitaryMatrix& m) {
    JsonArray rows;
    rows.reserve(m.size());
    for (const auto& row : m) {
        JsonArray jrow;
        jrow.reserve(row.size());
        for (const auto& z : row) jrow.push_back(encode_complex(z));
        rows.emplace_back(std::move(jrow));
    }
    return JsonValue(std::move(rows));
}

JsonValue encode_opaque_body(const OpaqueDefinition& def) {
    JsonObject body;
    if (std::holds_alternative<DeclarationOnly>(def.body)) {
        body["type"] = JsonValue(std::string{"declaration_only"});
    } else if (std::holds_alternative<UnitaryMatrix>(def.body)) {
        body["type"] = JsonValue(std::string{"matrix"});
        body["rows"] = encode_matrix(std::get<UnitaryMatrix>(def.body));
    } else {
        body["type"] = JsonValue(std::string{"kraus"});
        const auto& ch = std::get<KrausChannel>(def.body);
        JsonArray ops;
        ops.reserve(ch.operators.size());
        for (const auto& K : ch.operators) ops.push_back(encode_matrix(K));
        body["operators"] = JsonValue(std::move(ops));
    }
    return JsonValue(std::move(body));
}

JsonValue encode_gate(const GateOp& g) {
    JsonObject node;
    node["type"]      = JsonValue(std::string{"gate"});
    node["name"]      = JsonValue(g.name);
    node["gate_type"] = JsonValue(static_cast<std::int64_t>(g.type));

    JsonArray qubits;
    qubits.reserve(g.qubits.size());
    for (QubitIndex q : g.qubits) qubits.emplace_back(static_cast<std::int64_t>(q));
    node["qubits"] = JsonValue(std::move(qubits));

    JsonArray params;
    params.reserve(g.params.size());
    for (double p : g.params) params.emplace_back(p);
    node["params"] = JsonValue(std::move(params));

    if (!g.opaque_name.empty())   node["opaque_name"] = JsonValue(g.opaque_name);
    if (g.duration_ns.has_value()) node["duration_ns"] = JsonValue(*g.duration_ns);

    node["is_fused"]  = JsonValue(g.is_fused);
    node["is_opaque"] = JsonValue(g.is_opaque);

    if (!g.noise_channels.empty()) {
        // noise_channels are populated by Phase 8 NoiseModel pipeline; round-
        // trip them so a transpiled circuit dump can be reloaded without loss.
        JsonArray channels;
        channels.reserve(g.noise_channels.size());
        for (const auto& ch : g.noise_channels) {
            JsonArray ops;
            ops.reserve(ch.operators.size());
            for (const auto& K : ch.operators) ops.push_back(encode_matrix(K));
            JsonObject obj;
            obj["operators"] = JsonValue(std::move(ops));
            channels.emplace_back(std::move(obj));
        }
        node["noise_channels"] = JsonValue(std::move(channels));
    }

    return JsonValue(std::move(node));
}

JsonValue encode_op(const Op& op) {
    if (const auto* g = std::get_if<GateOp>(&op)) return encode_gate(*g);

    if (const auto* m = std::get_if<MeasureOp>(&op)) {
        JsonObject node;
        node["type"]  = JsonValue(std::string{"measure"});
        node["qubit"] = JsonValue(static_cast<std::int64_t>(m->qubit));
        node["cubit"] = JsonValue(static_cast<std::int64_t>(m->cubit));
        // Scheduling metadata: emit only when present so JSON produced
        // before the field existed still round-trips unchanged.
        if (m->duration_ns.has_value())
            node["duration_ns"] = JsonValue(*m->duration_ns);
        return JsonValue(std::move(node));
    }

    if (const auto* b = std::get_if<BarrierOp>(&op)) {
        JsonObject node;
        node["type"] = JsonValue(std::string{"barrier"});
        JsonArray qubits;
        qubits.reserve(b->qubits.size());
        for (QubitIndex q : b->qubits) qubits.emplace_back(static_cast<std::int64_t>(q));
        node["qubits"] = JsonValue(std::move(qubits));
        return JsonValue(std::move(node));
    }

    if (const auto* i = std::get_if<IfOp>(&op)) {
        JsonObject node;
        node["type"]  = JsonValue(std::string{"if"});
        node["creg"]  = JsonValue(static_cast<std::int64_t>(i->creg));
        node["value"] = JsonValue(static_cast<std::int64_t>(i->value));
        node["body"]  = encode_gate(i->body);
        return JsonValue(std::move(node));
    }

    if (const auto* idle = std::get_if<IdleOp>(&op)) {
        JsonObject node;
        node["type"]        = JsonValue(std::string{"idle"});
        node["qubit"]       = JsonValue(static_cast<std::int64_t>(idle->qubit));
        node["duration_ns"] = JsonValue(idle->duration_ns);
        return JsonValue(std::move(node));
    }

    // Variant has 5 alternatives, all handled above. The throw is a defensive
    // measure if Op variant gains a new alternative without updating this
    // encoder -- tests/scenario/unit/test_circuit_ir.cpp static_asserts also detect
    // the variant width change.
    throw CircuitError{"to_json: unknown Op variant alternative"};
}

JsonValue encode_metadata(const CircuitMetadata& meta) {
    JsonObject obj;
    obj["name"] = JsonValue(meta.name);

    if (meta.layout.has_value()) {
        JsonObject layout;
        JsonArray fwd;
        fwd.reserve(meta.layout->logical_to_physical.size());
        for (const auto& [logical, physical] : meta.layout->logical_to_physical) {
            JsonObject pair;
            pair["logical"]  = JsonValue(static_cast<std::int64_t>(logical));
            pair["physical"] = JsonValue(static_cast<std::int64_t>(physical));
            fwd.emplace_back(std::move(pair));
        }
        layout["logical_to_physical"] = JsonValue(std::move(fwd));
        // physical_to_logical is the inverse of logical_to_physical; we omit
        // it from the JSON and reconstruct on parse to keep payload small.
        obj["layout"] = JsonValue(std::move(layout));
    }

    if (meta.coupling_map.has_value()) {
        JsonArray edges;
        edges.reserve(meta.coupling_map->size());
        for (const auto& [a, b] : *meta.coupling_map) {
            JsonArray pair;
            pair.emplace_back(static_cast<std::int64_t>(a));
            pair.emplace_back(static_cast<std::int64_t>(b));
            edges.emplace_back(std::move(pair));
        }
        obj["coupling_map"] = JsonValue(std::move(edges));
    }

    if (meta.device_model_ref.has_value()) {
        obj["device_model_ref"] = JsonValue(*meta.device_model_ref);
    }

    return JsonValue(std::move(obj));
}

// ── Decode helpers ──────────────────────────────────────────────────────────

std::complex<double> decode_complex(const JsonValue& v) {
    const auto& arr = v.as_array();
    if (arr.size() != 2) {
        throw CircuitError{"circuit.json: complex number must be [re, im]"};
    }
    return std::complex<double>{arr[0].as_double(), arr[1].as_double()};
}

UnitaryMatrix decode_matrix(const JsonValue& v) {
    const auto& rows = v.as_array();
    UnitaryMatrix m;
    m.reserve(rows.size());
    for (const auto& row : rows) {
        const auto& cols = row.as_array();
        std::vector<std::complex<double>> r;
        r.reserve(cols.size());
        for (const auto& c : cols) r.push_back(decode_complex(c));
        m.push_back(std::move(r));
    }
    return m;
}

KrausChannel decode_kraus(const JsonValue& v) {
    const auto& ops = v.as_array();
    KrausChannel ch;
    ch.operators.reserve(ops.size());
    for (const auto& op : ops) ch.operators.push_back(decode_matrix(op));
    return ch;
}

GateType gate_type_from_int(std::int64_t v) {
    // GateType has 33 enumerators (0..32: I,X,Y,Z,H,S,Sdg,T,Tdg,SX,SXdg,RX,RY,
    // RZ,U1,U2,U3,CX,CY,CZ,CH,SWAP,CRZ,CU1,CU3,ECR,ISWAP,SQRT_ISWAP,FSIM,CCX,CSwap,
    // Reset,Opaque; inserted SX/SXdg, inserted ECR, inserted
    // ISWAP/SQRT_ISWAP/FSIM after ECR). Reject any value outside this range -- foreign
    // JSON producers must not invent enum values, and a stale dump from before an enum
    // change would crash backend dispatch silently otherwise.
    constexpr std::int64_t kMax = static_cast<std::int64_t>(GateType::Opaque);
    if (v < 0 || v > kMax) {
        throw CircuitError{"circuit.json: gate_type=" + std::to_string(v) +
                           " out of range (0.." + std::to_string(kMax) + ")"};
    }
    return static_cast<GateType>(v);
}

GateOp decode_gate(const JsonValue& v) {
    GateOp g;
    g.type = gate_type_from_int(v.at("gate_type").as_int());
    g.name = v.at("name").as_string();

    for (const auto& q : v.at("qubits").as_array()) {
        g.qubits.push_back(static_cast<QubitIndex>(q.as_int()));
    }
    for (const auto& p : v.at("params").as_array()) {
        g.params.push_back(p.as_double());
    }

    if (v.contains("opaque_name"))  g.opaque_name = v.at("opaque_name").as_string();
    if (v.contains("duration_ns"))  g.duration_ns = v.at("duration_ns").as_double();

    g.is_fused  = v.contains("is_fused")  ? v.at("is_fused").as_bool()  : false;
    g.is_opaque = v.contains("is_opaque") ? v.at("is_opaque").as_bool() : false;

    if (v.contains("noise_channels")) {
        for (const auto& ch_obj : v.at("noise_channels").as_array()) {
            KrausChannel ch;
            for (const auto& K : ch_obj.at("operators").as_array()) {
                ch.operators.push_back(decode_matrix(K));
            }
            g.noise_channels.push_back(std::move(ch));
        }
    }
    return g;
}

OpaqueDefinition decode_opaque_definition(const JsonValue& v) {
    OpaqueDefinition def;
    def.name       = v.at("name").as_string();
    def.num_qubits = static_cast<QubitIndex>(v.at("num_qubits").as_int());

    const auto& body = v.at("body");
    const std::string& body_type = body.at("type").as_string();
    if (body_type == "declaration_only") {
        def.body = DeclarationOnly{};
    } else if (body_type == "matrix") {
        def.body = decode_matrix(body.at("rows"));
    } else if (body_type == "kraus") {
        def.body = decode_kraus(body.at("operators"));
    } else {
        throw CircuitError{"circuit.json: unknown opaque body.type=\"" + body_type + "\""};
    }
    return def;
}

CircuitMetadata decode_metadata(const JsonValue& v) {
    CircuitMetadata meta;
    if (v.contains("name")) meta.name = v.at("name").as_string();

    if (v.contains("layout")) {
        QubitLayout layout;
        for (const auto& pair : v.at("layout").at("logical_to_physical").as_array()) {
            const auto logical  = static_cast<QubitIndex>(pair.at("logical").as_int());
            const auto physical = static_cast<QubitIndex>(pair.at("physical").as_int());
            layout.logical_to_physical[logical]  = physical;
            layout.physical_to_logical[physical] = logical;
        }
        meta.layout = std::move(layout);
    }

    if (v.contains("coupling_map")) {
        CouplingMap edges;
        for (const auto& pair : v.at("coupling_map").as_array()) {
            const auto& arr = pair.as_array();
            if (arr.size() != 2) {
                throw CircuitError{"circuit.json: coupling_map entry must be [a, b]"};
            }
            edges.emplace_back(static_cast<QubitIndex>(arr[0].as_int()),
                               static_cast<QubitIndex>(arr[1].as_int()));
        }
        meta.coupling_map = std::move(edges);
    }

    if (v.contains("device_model_ref")) {
        meta.device_model_ref = v.at("device_model_ref").as_string();
    }
    return meta;
}

}  // namespace

// detail_make_circuit_from_payload is declared in circuit.hpp + defined in
// circuit.cpp; it is the friend-ed bridge that lets us populate Circuit's
// private storage from a decoded JSON payload without a setter API.

// ── Public API entry points (called by Circuit::to_json / from_json) ────────

std::string serialize_circuit_json(const Circuit& c) {
    detail::JsonObject root;
    root["version"]    = detail::JsonValue(static_cast<std::int64_t>(kSchemaVersion));
    root["num_qubits"] = detail::JsonValue(static_cast<std::int64_t>(c.num_qubits()));

    detail::JsonArray regs;
    regs.reserve(c.classical_registers().size());
    for (const auto& cr : c.classical_registers()) {
        detail::JsonObject obj;
        obj["name"]   = detail::JsonValue(cr.name);
        obj["width"]  = detail::JsonValue(static_cast<std::int64_t>(cr.width));
        obj["offset"] = detail::JsonValue(static_cast<std::int64_t>(cr.offset));
        regs.emplace_back(std::move(obj));
    }
    root["classical_registers"] = detail::JsonValue(std::move(regs));

    detail::JsonArray opaques;
    opaques.reserve(c.opaque_definitions().size());
    for (const auto& def : c.opaque_definitions()) {
        detail::JsonObject obj;
        obj["name"]       = detail::JsonValue(def.name);
        obj["num_qubits"] = detail::JsonValue(static_cast<std::int64_t>(def.num_qubits));
        obj["body"]       = encode_opaque_body(def);
        opaques.emplace_back(std::move(obj));
    }
    root["opaque_definitions"] = detail::JsonValue(std::move(opaques));

    root["metadata"] = encode_metadata(c.metadata());

    // Expectation targets — preserved for round-trip.
    detail::JsonArray exp_targets;
    exp_targets.reserve(c.expectation_targets().size());
    for (const auto& p : c.expectation_targets()) {
        exp_targets.emplace_back(detail::JsonValue(p));
    }
    root["expectation_targets"] = detail::JsonValue(std::move(exp_targets));

    detail::JsonArray ops;
    ops.reserve(c.ops().size());
    for (const auto& op : c.ops()) ops.push_back(encode_op(op));
    root["ops"] = detail::JsonValue(std::move(ops));

    return detail::write_json(detail::JsonValue(std::move(root)), /*pretty=*/true);
}

namespace {

Op decode_op(const JsonValue& v) {
    const std::string& type = v.at("type").as_string();
    if (type == "gate")    return decode_gate(v);
    if (type == "measure") {
        MeasureOp m{
            static_cast<QubitIndex>       (v.at("qubit").as_int()),
            static_cast<ClassicalBitIndex>(v.at("cubit").as_int())
        };
        if (v.contains("duration_ns")) m.duration_ns = v.at("duration_ns").as_double();
        return m;
    }
    if (type == "barrier") {
        BarrierOp b;
        for (const auto& q : v.at("qubits").as_array()) {
            b.qubits.push_back(static_cast<QubitIndex>(q.as_int()));
        }
        return b;
    }
    if (type == "if") {
        IfOp ifop;
        ifop.creg  = static_cast<ClassicalRegisterIndex>(v.at("creg").as_int());
        ifop.value = static_cast<std::uint64_t>        (v.at("value").as_int());
        ifop.body  = decode_gate(v.at("body"));
        return ifop;
    }
    if (type == "idle") {
        IdleOp idle;
        idle.qubit       = static_cast<QubitIndex>(v.at("qubit").as_int());
        idle.duration_ns = v.at("duration_ns").as_double();
        return idle;
    }
    throw CircuitError{"circuit.json: unknown op type=\"" + type + "\""};
}

}  // namespace

Circuit deserialize_circuit_json(std::string_view text) {
    detail::JsonValue root_val;
    try {
        root_val = detail::parse_json(text);
    } catch (const CircuitError& e) {
        // Tag the message with the canonical prefix that mandates.
        throw CircuitError{std::string{"circuit.json: "} + e.what()};
    }

    if (!root_val.is_object()) {
        throw CircuitError{"circuit.json: root must be an object"};
    }

    if (root_val.contains("version") && root_val.at("version").as_int() != kSchemaVersion) {
        throw CircuitError{"circuit.json: unsupported schema version " +
                           std::to_string(root_val.at("version").as_int())};
    }

    if (!root_val.contains("num_qubits")) {
        throw CircuitError{"circuit.json: missing required field \"num_qubits\""};
    }
    const auto num_qubits = static_cast<QubitIndex>(root_val.at("num_qubits").as_int());

    std::vector<ClassicalRegister> regs;
    if (root_val.contains("classical_registers")) {
        for (const auto& cr_v : root_val.at("classical_registers").as_array()) {
            ClassicalRegister cr;
            cr.name   = cr_v.at("name").as_string();
            cr.width  = static_cast<ClassicalBitIndex>(cr_v.at("width").as_int());
            cr.offset = static_cast<ClassicalBitIndex>(cr_v.at("offset").as_int());
            regs.push_back(std::move(cr));
        }
    }

    std::vector<OpaqueDefinition> opaques;
    if (root_val.contains("opaque_definitions")) {
        for (const auto& def_v : root_val.at("opaque_definitions").as_array()) {
            opaques.push_back(decode_opaque_definition(def_v));
        }
    }

    std::vector<Op> ops;
    if (root_val.contains("ops")) {
        for (const auto& op_v : root_val.at("ops").as_array()) {
            ops.push_back(decode_op(op_v));
        }
    }

    std::vector<std::string> expectation_targets;
    if (root_val.contains("expectation_targets")) {
        for (const auto& p_v : root_val.at("expectation_targets").as_array()) {
            expectation_targets.push_back(p_v.as_string());
        }
    }

    CircuitMetadata meta;
    if (root_val.contains("metadata")) meta = decode_metadata(root_val.at("metadata"));

    return detail_make_circuit_from_payload(num_qubits,
                                            std::move(regs),
                                            std::move(opaques),
                                            std::move(ops),
                                            std::move(expectation_targets),
                                            std::move(meta));
}

Circuit load_circuit_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw CircuitError{"circuit.json: cannot open file \"" + path + "\""};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return deserialize_circuit_json(buf.str());
}

}  // namespace qplayer
