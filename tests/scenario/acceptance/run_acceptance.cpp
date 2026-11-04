// run_acceptance.cpp -- AcceptanceRunner implementation.
//
// Phase 0-T / Phase 4 scope:
//   expected_fail=true cases: NotImplementedError path -> NOT_IMPLEMENTED.
//   expected_fail=false cases with kind=circuit_metric: parse QASM, run
//     transpile() at the recorded optimization_level, compare num_ops to
// expected_value. No backend execution required.
//
// Phase 5 additions:
//   kind=counts: parse+run FullState, compare counts distribution per bitstring.
//   kind=statevector: parse+run FullState, compare statevector L2 norm.
//   kind=statevector_dump: parse+run FullState, compare per-amplitude + L2
//     against inline expected_amplitudes (oracle_10) or sibling .data file
//     (oracle_20).
//
// JSON parsing uses a minimal hand-rolled key extractor (no external library)
// because the acceptance JSON files use a flat, well-known schema.

#include "run_acceptance.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // isatty(1) -- TTY-aware colour output.
#endif

#include "error.hpp"
#include "noise/noise_model.hpp"
#include "parser/parser.hpp"
#include "qregister.hpp"
#include "transpiler/transpiler.hpp"

#ifndef QPLAYER_ACCEPTANCE_DATA_DIR
// Defensive fallback if the CMake compile define did not reach this TU.
// Production builds always pass -DQPLAYER_ACCEPTANCE_DATA_DIR="<absolute>"
// via target_compile_definitions (tests/CMakeLists.txt).
#define QPLAYER_ACCEPTANCE_DATA_DIR "tests/data"
#endif

#ifndef QPLAYER_ACCEPTANCE_EXTRACT_DIR
// Build-tree directory where archived reference dumps are extracted.
#define QPLAYER_ACCEPTANCE_EXTRACT_DIR ""
#endif

namespace qplayer::acceptance {

namespace {

// ANSI color helpers -- only emit escape codes when stdout is a TTY. Matches
// the convention from tests/framework/test_runner.cpp (Phase 1 session log
// item 3).
const char* color_for(Status s) {
    static const bool is_tty = []() {
#if defined(__unix__) || defined(__APPLE__)
        return ::isatty(1) != 0;
#else
        return false;
#endif
    }();
    if (!is_tty) return "";
    switch (s) {
        case Status::PASS:             return "\033[32m";  // green
        case Status::NOT_IMPLEMENTED:  return "\033[36m";  // cyan
        case Status::FAIL:             return "\033[31m";  // red
        case Status::UNEXPECTED_PASS:  return "\033[33m";  // yellow
        case Status::UNEXPECTED_FAIL:  return "\033[31m";  // red
    }
    return "";
}

const char* color_reset() {
    static const bool is_tty = []() {
#if defined(__unix__) || defined(__APPLE__)
        return ::isatty(1) != 0;
#else
        return false;
#endif
    }();
    return is_tty ? "\033[0m" : "";
}

const char* status_label(Status s) {
    switch (s) {
        case Status::PASS:             return "PASS";
        case Status::FAIL:             return "FAIL";
        case Status::NOT_IMPLEMENTED:  return "NOT_IMPL";
        case Status::UNEXPECTED_PASS:  return "UNEXPECTED_PASS";
        case Status::UNEXPECTED_FAIL:  return "UNEXPECTED_FAIL";
    }
    return "UNKNOWN";
}

// Confirms the expected JSON file exists -- the only check we run on it during
// Phase 0-T. Throws std::runtime_error so the caller can route it through the
// "other exception" branch (-> UNEXPECTED_FAIL when expected_fail=true).
void require_expected_json(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("expected JSON file not found: " + path);
    }
}

// Read entire file into a string. Throws std::runtime_error on failure.
std::string read_file(const std::string& path) {
    std::ifstream ifs{path};
    if (!ifs.is_open()) {
        throw std::runtime_error("cannot open file: " + path);
    }
    std::ostringstream buf;
    buf << ifs.rdbuf();
    return buf.str();
}

// Minimal JSON key extractor for the flat acceptance JSON schema.
// Searches for `"key": <value>` where value is either a quoted string or
// an integer. Returns empty string if key not found.
// NOTE: This is intentionally minimal -- it is not a general JSON parser.
// It handles only the flat, well-known keys in tests/data/qasm/expected/*.json.
std::string json_get_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    // Skip whitespace + colon.
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                  json[pos] == ':' || json[pos] == '\n' ||
                                  json[pos] == '\r')) {
        ++pos;
    }
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        // Quoted string value.
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }
    // Unquoted numeric value -- read until non-digit.
    auto start = pos;
    while (pos < json.size() && (std::isdigit(static_cast<unsigned char>(json[pos])) ||
                                   json[pos] == '-')) {
        ++pos;
    }
    return json.substr(start, pos - start);
}

// Extract the balanced JSON array text (`[ ... ]`, brackets matched) bound to
// `key`, or "" when the key is absent. Used to lift device_coupling_map verbatim
// into a device_json string for transpile(). Minimal — assumes no '[' inside
// string values of the array, which holds for the integer-pair coupling maps.
std::string json_extract_array(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return "";
    int depth = 0;
    for (std::size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '[') ++depth;
        else if (json[i] == ']') {
            if (--depth == 0) return json.substr(pos, i - pos + 1);
        }
    }
    return "";
}

// Parse a floating-point number starting at position *pos in json.
// Advances *pos past the number. Returns the parsed value.
static double parse_double(const std::string& json, std::size_t* pos) {
    const std::size_t start = *pos;
    // Accept leading minus, digits, dot, exponent notation.
    while (*pos < json.size()) {
        const char c = json[*pos];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            c == '-' || c == '+' || c == '.' ||
            c == 'e' || c == 'E') {
            ++(*pos);
        } else {
            break;
        }
    }
    if (*pos == start) {
        throw std::runtime_error("parse_double: no digits at position " +
                                 std::to_string(start));
    }
    return std::stod(json.substr(start, *pos - start));
}

// Extract the numeric value following "key": from json. Understands integer
// and floating-point literals (including scientific notation).
static double json_get_number(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0.0;
    pos += needle.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':' ||
            json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
    if (pos >= json.size()) return 0.0;
    return parse_double(json, &pos);
}

// Parse the expected_counts map from the JSON.
// Format: "expected_counts": { "00": 0.5, "11": 0.5 }
static std::map<std::string, double> parse_expected_counts(
        const std::string& json) {
    std::map<std::string, double> result;
    const std::string key = "\"expected_counts\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return result;
    pos += key.size();
    // Find opening '{'.
    while (pos < json.size() && json[pos] != '{') ++pos;
    if (pos >= json.size()) return result;
    ++pos;  // skip '{'
    while (pos < json.size() && json[pos] != '}') {
        // Skip whitespace and commas.
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' ||
                json[pos] == '\n' || json[pos] == '\r' || json[pos] == ',')) {
            ++pos;
        }
        if (pos >= json.size() || json[pos] == '}') break;
        // Read bitstring key.
        if (json[pos] != '"') { ++pos; continue; }
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) break;
        std::string bitstring = json.substr(pos, end - pos);
        pos = end + 1;
        // Skip ':' + whitespace.
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':' ||
                json[pos] == '\n' || json[pos] == '\r')) {
            ++pos;
        }
        // Read probability value.
        double prob = parse_double(json, &pos);
        result[bitstring] = prob;
    }
    return result;
}

// Parse the expected_amplitudes array from the JSON.
// Format: "expected_amplitudes": [[re, im], ...]
static std::vector<std::complex<double>> parse_expected_amplitudes(
        const std::string& json) {
    std::vector<std::complex<double>> result;
    const std::string key = "\"expected_amplitudes\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return result;
    pos += key.size();
    // Find '[' opening the outer array.
    while (pos < json.size() && json[pos] != '[') ++pos;
    if (pos >= json.size()) return result;
    ++pos;  // skip outer '['
    while (pos < json.size()) {
        // Skip whitespace and commas.
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' ||
                json[pos] == '\n' || json[pos] == '\r' || json[pos] == ',')) {
            ++pos;
        }
        if (pos >= json.size() || json[pos] == ']') break;  // end of outer array
        if (json[pos] != '[') { ++pos; continue; }
        ++pos;  // skip '['
        // Read re.
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' ||
                json[pos] == '\n' || json[pos] == '\r')) ++pos;
        double re = parse_double(json, &pos);
        // Skip ',' between re and im.
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ',')) ++pos;
        double im = parse_double(json, &pos);
        // Skip ']'.
        while (pos < json.size() && json[pos] != ']') ++pos;
        if (pos < json.size()) ++pos;
        result.emplace_back(re, im);
    }
    return result;
}

// Parse amplitudes from oracle_20.data format.
// Header line starts with "==="; subsequent lines match:
//   [P=<f>] [<+f>, <+f>] |<bitstring>>
// Returns the parsed amplitudes in basis-index order (line i -> index i).
static std::vector<std::complex<double>> parse_data_file(const std::string& path) {
    std::ifstream ifs{path};
    if (!ifs.is_open()) {
        throw std::runtime_error("cannot open data file: " + path);
    }
    std::vector<std::complex<double>> result;
    std::string line;
    bool header_seen = false;
    while (std::getline(ifs, line)) {
        // Skip the header line starting with '='.
        if (!header_seen) {
            if (!line.empty() && line[0] == '=') {
                header_seen = true;
                continue;
            }
            continue;
        }
        if (line.empty()) continue;
        // Format: [P=<f>] [<+f>, <+f>] |<bitstring>>
        // Find '[' after 'P=...'.
        auto bracket1 = line.find('[');
        if (bracket1 == std::string::npos) continue;
        auto bracket2 = line.find('[', bracket1 + 1);
        if (bracket2 == std::string::npos) continue;
        ++bracket2;  // skip the second '['
        std::size_t pos = bracket2;
        double re = parse_double(line, &pos);
        // Skip ',' and whitespace.
        while (pos < line.size() && (line[pos] == ',' || line[pos] == ' ')) ++pos;
        double im = parse_double(line, &pos);
        result.emplace_back(re, im);
    }
    return result;
}

// Verify the counts distribution for a "counts" kind case.
// Parses the QASM, runs with set_seed + nshots, compares each bitstring.
// Returns true on match, false on mismatch. Throws on hard errors.
static bool run_counts(const TestCase& tc) {
    const std::string json_text = read_file(tc.expected_json_path);
    const int nshots = static_cast<int>(json_get_number(json_text, "nshots"));
    const double tolerance = json_get_number(json_text, "tolerance");
    const auto expected = parse_expected_counts(json_text);

    if (nshots <= 0 || expected.empty()) {
        throw std::runtime_error(
            "counts: invalid nshots or empty expected_counts in " +
            tc.expected_json_path);
    }

    Parser p;
    Circuit circ = p.parse_file(tc.qasm_path);

    // tc.sim selects the backend: FullState for the reference rows, and
    // TensorNetwork for the Phase 9 TN gate rows running the same expectation.
    QRegister qr{tc.sim};
    qr.set_seed(42);  // Deterministic seed for reproducible counts.

    // Coupling-map compilation: when the expected JSON carries a device_coupling_map, compile the
    // circuit against it first (② layout → ③ routing → ⑤ verify). Results stay
    // logical-indexed, so the comparison below is unchanged.
    const std::string coupling = json_extract_array(json_text, "device_coupling_map");
    if (!coupling.empty()) {
        const std::string device_json = "{\"coupling_map\":" + coupling + "}";
        circ = qr.transpile(std::move(circ), 1, device_json);
    }

    Result r = qr.run(circ, static_cast<std::size_t>(nshots));
    const auto& actual_counts = r.counts();

    bool ok = true;

    // Check all expected bitstrings.
    for (const auto& [bs, exp_prob] : expected) {
        std::uint64_t cnt = 0;
        auto it = actual_counts.find(bs);
        if (it != actual_counts.end()) cnt = it->second;
        const double obs_prob =
            static_cast<double>(cnt) / static_cast<double>(nshots);
        const double err = std::abs(obs_prob - exp_prob);
        if (err > tolerance) {
            std::cerr << "  counts MISMATCH bitstring=" << bs
                      << " expected=" << exp_prob
                      << " observed=" << obs_prob
                      << " err=" << err
                      << " tol=" << tolerance << "\n";
            ok = false;
        }
    }

    // Check that unregistered bitstrings do not exceed tolerance (no leakage).
    for (const auto& [bs, cnt] : actual_counts) {
        if (expected.find(bs) == expected.end()) {
            const double obs_prob =
                static_cast<double>(cnt) / static_cast<double>(nshots);
            if (obs_prob > tolerance) {
                std::cerr << "  counts LEAK bitstring=" << bs
                          << " observed=" << obs_prob
                          << " tol=" << tolerance << "\n";
                ok = false;
            }
        }
    }
    return ok;
}

// Verify the Bell-state fidelity for a "fidelity" kind case (Phase 8 noise
// gate). The noise model is depolarizing per gate (probabilities in the JSON),
// supplied at runtime via set_noise_model (the QASM carries no noise). Under a
// depolarizing channel the Bell pair becomes a Werner state
//   rho = F |Phi+><Phi+| + (1-F) I/4,
// whose Z-basis diagonal gives P(00)=P(11)=(1+F)/4 and P(01)=P(10)=(1-F)/4.
// Hence the fidelity is recoverable from computational-basis counts alone:
//   F = 2*(P00 + P11) - 1
// (no density-matrix or basis-rotation machinery needed —).
static bool run_fidelity(const TestCase& tc) {
    const std::string json_text = read_file(tc.expected_json_path);
    const int    nshots             = static_cast<int>(json_get_number(json_text, "nshots"));
    const double tolerance          = json_get_number(json_text, "tolerance");
    const double expected_fidelity  = json_get_number(json_text, "expected_fidelity");
    const double p_1q               = json_get_number(json_text, "depolarizing_1q_prob");
    const double p_2q               = json_get_number(json_text, "depolarizing_2q_prob");

    if (nshots <= 0) {
        throw std::runtime_error("fidelity: invalid nshots in " +
                                 tc.expected_json_path);
    }

    Parser  p;
    Circuit circ = p.parse_file(tc.qasm_path);

    // Build the depolarizing model: p_1q on single-qubit gates, p_2q on the
    // two-qubit gate. noisy_bell.qasm uses exactly h (1Q) and cx (2Q).
    NoiseModel nm;
    nm.add_depolarizing("h",  p_1q);
    nm.add_depolarizing("cx", p_2q);

    QRegister qr{SimType::FullState};
    qr.set_seed(42);
    qr.set_noise_model(nm);
    Result r = qr.run(circ, static_cast<std::size_t>(nshots));
    const auto& counts = r.counts();

    auto prob = [&](const std::string& bs) {
        auto it = counts.find(bs);
        return it == counts.end()
                   ? 0.0
                   : static_cast<double>(it->second) / static_cast<double>(nshots);
    };
    const double fidelity = 2.0 * (prob("00") + prob("11")) - 1.0;
    const double err = std::abs(fidelity - expected_fidelity);
    if (err > tolerance) {
        std::cerr << "  fidelity MISMATCH measured=" << fidelity
                  << " expected=" << expected_fidelity
                  << " err=" << err << " tol=" << tolerance << "\n";
        return false;
    }
    return true;
}

// Verify the counts distribution for a "counts_sparse" kind case.
// Identical to run_counts() but runs on the SparseState backend so
// the circuit can exceed the dense statevector ceiling (e.g. 100 qubits). The
// sparse-amplitude invariant (only a handful of non-zero terms) is implicitly
// proven by the run completing at all: a dense backend would need 2^n
// amplitudes and could never allocate for n=100. The expected JSON's
// nonzero_amplitude_count field documents that invariant but is not separately
// asserted here (no public sparse-map introspection API in Phase 7).
static bool run_counts_sparse(const TestCase& tc) {
    const std::string json_text = read_file(tc.expected_json_path);
    const int nshots = static_cast<int>(json_get_number(json_text, "nshots"));
    const double tolerance = json_get_number(json_text, "tolerance");
    const auto expected = parse_expected_counts(json_text);

    if (nshots <= 0 || expected.empty()) {
        throw std::runtime_error(
            "counts_sparse: invalid nshots or empty expected_counts in " +
            tc.expected_json_path);
    }

    Parser p;
    Circuit circ = p.parse_file(tc.qasm_path);

    QRegister qr{SimType::SparseState};
    qr.set_seed(42);  // Deterministic seed for reproducible counts.
    Result r = qr.run(circ, static_cast<std::size_t>(nshots));
    const auto& actual_counts = r.counts();

    bool ok = true;

    // Check all expected bitstrings against the per-bitstring tolerance.
    for (const auto& [bs, exp_prob] : expected) {
        std::uint64_t cnt = 0;
        auto it = actual_counts.find(bs);
        if (it != actual_counts.end()) cnt = it->second;
        const double obs_prob =
            static_cast<double>(cnt) / static_cast<double>(nshots);
        const double err = std::abs(obs_prob - exp_prob);
        if (err > tolerance) {
            std::cerr << "  counts_sparse MISMATCH bitstring=" << bs
                      << " expected=" << exp_prob
                      << " observed=" << obs_prob
                      << " err=" << err
                      << " tol=" << tolerance << "\n";
            ok = false;
        }
    }

    // No leakage: any bitstring outside the expected set must stay within tol.
    for (const auto& [bs, cnt] : actual_counts) {
        if (expected.find(bs) == expected.end()) {
            const double obs_prob =
                static_cast<double>(cnt) / static_cast<double>(nshots);
            if (obs_prob > tolerance) {
                std::cerr << "  counts_sparse LEAK bitstring=" << bs
                          << " observed=" << obs_prob
                          << " tol=" << tolerance << "\n";
                ok = false;
            }
        }
    }
    return ok;
}

// Verify the statevector for a "statevector" kind case.
// Checks that all amplitudes equal the expected uniform value (L2 norm check).
static bool run_statevector(const TestCase& tc) {
    const std::string json_text = read_file(tc.expected_json_path);
    const double tolerance_l2 = json_get_number(json_text, "tolerance_l2");
    const double exp_amp = json_get_number(json_text,
        "expected_statevector_uniform_amplitude");
    const int exp_dim = static_cast<int>(
        json_get_number(json_text, "expected_state_dimension"));

    Parser p;
    Circuit circ = p.parse_file(tc.qasm_path);

    // Use CPU explicitly: statevector accuracy tests compare against CPU
    // baselines (generated with enable_fusion=true). GPU no-fusion path
    // diverges on circuits with ctrl>tgt gate ordering. TensorNetwork ignores
    // ProcType (CPU-only backend), so the TN gate rows pass through unchanged.
    QRegister qr{tc.sim, qplayer::ProcType::CPU};
    qr.set_seed(42);
    qr.run(circ, 1);
    const StateVector sv = qr.get_statevector();

    if (static_cast<int>(sv.size()) != exp_dim) {
        std::cerr << "  statevector DIM mismatch: expected=" << exp_dim
                  << " got=" << sv.size() << "\n";
        return false;
    }

    // L2 distance between computed sv and expected uniform state (exp_amp, 0).
    double l2sq = 0.0;
    const std::complex<double> exp_c{exp_amp, 0.0};
    for (const auto& a : sv) {
        const std::complex<double> diff = a - exp_c;
        l2sq += (diff.real() * diff.real() + diff.imag() * diff.imag());
    }
    const double l2 = std::sqrt(l2sq);
    if (l2 > tolerance_l2) {
        std::cerr << "  statevector L2 err=" << l2
                  << " tol=" << tolerance_l2 << "\n";
        return false;
    }
    return true;
}

// Verify the statevector for a "statevector_dump" kind case.
// Supports inline expected_amplitudes (oracle_10) and external data_file
// (oracle_20). Runs per-amplitude and L2 checks.
static bool run_statevector_dump(const TestCase& tc) {
    const std::string json_text = read_file(tc.expected_json_path);
    const double tol_l2  = json_get_number(json_text, "tolerance_l2");
    const double tol_per = json_get_number(json_text, "tolerance_per_amplitude");
    const int num_states = static_cast<int>(
        json_get_number(json_text, "num_states"));

    // Load expected amplitudes -- inline or from sibling .data file.
    std::vector<std::complex<double>> expected;
    const std::string data_file_key = json_get_string(json_text, "data_file");
    if (!data_file_key.empty()) {
        // Prefer a sibling file next to the expected JSON; large dumps are
        // shipped as <name>.tar.gz and extracted into the build tree instead.
        const std::filesystem::path json_dir =
            std::filesystem::path(tc.expected_json_path).parent_path();
        std::filesystem::path data_path = json_dir / data_file_key;
        if (!std::filesystem::exists(data_path)) {
            const char* env = std::getenv("QPLAYER_ACCEPTANCE_EXTRACT_DIR");
            const std::string extract_dir =
                env != nullptr ? env : QPLAYER_ACCEPTANCE_EXTRACT_DIR;
            data_path = std::filesystem::path(extract_dir) / data_file_key;
            if (extract_dir.empty() || !std::filesystem::exists(data_path)) {
                throw std::runtime_error(
                    "cannot find data file " + data_file_key + " (expected next to "
                    + json_dir.string() + " or extracted from " + data_file_key
                    + ".tar.gz into the build tree by the qplayer_acceptance_data target)");
            }
        }
        expected = parse_data_file(data_path.string());
    } else {
        expected = parse_expected_amplitudes(json_text);
    }

    if (static_cast<int>(expected.size()) != num_states) {
        throw std::runtime_error(
            "statevector_dump: expected " + std::to_string(num_states) +
            " amplitudes, loaded " + std::to_string(expected.size()));
    }

    Parser p;
    Circuit circ = p.parse_file(tc.qasm_path);

    // Use CPU explicitly: statevector_dump baselines (oracle_10.json,
    // oracle_20.data) were generated with the CPU backend + Gate Fusion.
    // GPU no-fusion diverges on ctrl>tgt gate circuits (see fix comment).
    // GPU golden comparison is covered by the GoldenGpu suite in test_golden_gpu.cpp.
    QRegister qr{tc.sim, qplayer::ProcType::CPU};
    qr.set_seed(42);
    qr.run(circ, 1);
    const StateVector sv = qr.get_statevector();

    if (static_cast<int>(sv.size()) != num_states) {
        std::cerr << "  statevector_dump DIM mismatch: expected=" << num_states
                  << " got=" << sv.size() << "\n";
        return false;
    }

    bool ok = true;
    double l2sq = 0.0;
    double max_per = 0.0;
    std::size_t first_fail = static_cast<std::size_t>(-1);

    for (std::size_t i = 0; i < static_cast<std::size_t>(num_states); ++i) {
        const std::complex<double> diff = sv[i] - expected[i];
        const double err =
            std::sqrt(diff.real() * diff.real() + diff.imag() * diff.imag());
        l2sq += err * err;
        if (err > max_per) max_per = err;
        if (err > tol_per && first_fail == static_cast<std::size_t>(-1)) {
            first_fail = i;
            ok = false;
        }
    }
    const double l2 = std::sqrt(l2sq);
    if (l2 > tol_l2) ok = false;

    if (!ok) {
        std::cerr << "  statevector_dump mismatch: L2=" << l2
                  << " (tol=" << tol_l2 << ")"
                  << " max_per_amplitude=" << max_per
                  << " (tol=" << tol_per << ")";
        if (first_fail != static_cast<std::size_t>(-1)) {
            const std::complex<double> diff = sv[first_fail] - expected[first_fail];
            std::cerr << " first_fail_idx=" << first_fail
                      << " got=(" << sv[first_fail].real() << ","
                      << sv[first_fail].imag() << ")"
                      << " exp=(" << expected[first_fail].real() << ","
                      << expected[first_fail].imag() << ")"
                      << " |diff|=" << std::sqrt(diff.real()*diff.real() +
                                                  diff.imag()*diff.imag());
        }
        std::cerr << "\n";
    }
    return ok;
}

// Runs the circuit_metric comparison for a single acceptance case whose
// expected_fail is false. Parses the QASM file, calls transpile() at the
// recorded optimization_level, and checks num_ops == expected_value.
// Returns true if the metric matches, false otherwise.
// Throws on any parser / transpile error so the caller can classify it.
bool run_circuit_metric(const TestCase& tc) {
    const std::string json_text = read_file(tc.expected_json_path);

    const std::string metric =
        json_get_string(json_text, "metric");
    const std::string opt_level_str =
        json_get_string(json_text, "optimization_level");
    const std::string expected_str =
        json_get_string(json_text, "expected_value");

    if (metric != "num_ops_after_transpile") {
        throw std::runtime_error(
            "circuit_metric: unsupported metric '" + metric + "'");
    }
    if (opt_level_str.empty() || expected_str.empty()) {
        throw std::runtime_error(
            "circuit_metric: missing optimization_level or expected_value in " +
            tc.expected_json_path);
    }

    const int  opt_level    = std::stoi(opt_level_str);
    const long expected_ops = std::stol(expected_str);

    // Parse the QASM source using the standalone Parser facade (not
    // QRegister::parse_file() which is still a stub in Phase 4).
    Parser p;
    Circuit circuit = p.parse_file(tc.qasm_path);

    // Transpile using the standalone Transpiler facade (same path that
    // QRegister::transpile() delegates to after).
    Transpiler t;
    Circuit result = t.transpile(std::move(circuit), opt_level, "");

    const long actual_ops = static_cast<long>(result.num_ops());
    if (actual_ops != expected_ops) {
        std::cerr << "  circuit_metric MISMATCH: expected num_ops="
                  << expected_ops << " got=" << actual_ops << "\n";
        return false;
    }
    return true;
}

}  // namespace

AcceptanceRunner::AcceptanceRunner(std::vector<TestCase> cases)
    : cases_(std::move(cases)) {
    counts_[Status::PASS]            = 0;
    counts_[Status::FAIL]            = 0;
    counts_[Status::NOT_IMPLEMENTED] = 0;
    counts_[Status::UNEXPECTED_PASS] = 0;
    counts_[Status::UNEXPECTED_FAIL] = 0;
}

Status AcceptanceRunner::run_one(const TestCase& tc) {
    try {
        require_expected_json(tc.expected_json_path);

        // For expected_fail=false cases we run the real comparison.
        if (!tc.expected_fail) {
            // Determine the kind of check from the expected JSON.
            const std::string json_text = read_file(tc.expected_json_path);
            const std::string kind = json_get_string(json_text, "kind");

            if (kind == "circuit_metric") {
                const bool ok = run_circuit_metric(tc);
                return ok ? Status::PASS : Status::FAIL;
            }
            if (kind == "counts") {
                const bool ok = run_counts(tc);
                return ok ? Status::PASS : Status::FAIL;
            }
            if (kind == "fidelity") {
                const bool ok = run_fidelity(tc);
                return ok ? Status::PASS : Status::FAIL;
            }
            if (kind == "counts_sparse") {
                const bool ok = run_counts_sparse(tc);
                return ok ? Status::PASS : Status::FAIL;
            }
            if (kind == "statevector") {
                const bool ok = run_statevector(tc);
                return ok ? Status::PASS : Status::FAIL;
            }
            if (kind == "statevector_dump") {
                const bool ok = run_statevector_dump(tc);
                return ok ? Status::PASS : Status::FAIL;
            }
            // Unknown kind -- treat as not yet implemented so the runner does
            // not falsely report PASS for future schema additions.
            throw NotImplementedError{
                "AcceptanceRunner",
                "comparison kind '" + kind + "' not implemented yet"};
        }

        // expected_fail=true: downstream comparison is not yet active for this
        // case (Phase 5+ gate target). Throw NotImplementedError directly so
        // the runner classifies it as NOT_IMPLEMENTED.
        // We do NOT call parse_file() here because the QASM source may already
        // parse correctly (oracle_20, etc.) but the expected comparison logic
        // (statevector dump, counts, etc.) is not yet wired up. Calling the
        // parser and then throwing would hide future UNEXPECTED_FAIL cases
        // when expected_fail is eventually flipped to false.
        throw NotImplementedError{
            "AcceptanceRunner",
            "downstream comparison not yet active for '" + tc.label + "'"
                " (Phase Gate: " + tc.phase_gate + ")"};
    } catch (const NotImplementedError&) {
        return Status::NOT_IMPLEMENTED;
    } catch (const std::exception& ex) {
        if (tc.expected_fail) {
            std::cerr << "  UNEXPECTED_FAIL (" << tc.label << "): "
                      << ex.what() << "\n";
            return Status::UNEXPECTED_FAIL;
        }
        std::cerr << "  FAIL (" << tc.label << "): " << ex.what() << "\n";
        return Status::FAIL;
    }
}

void AcceptanceRunner::run_all() {
    per_case_status_.clear();
    per_case_status_.reserve(cases_.size());
    for (const auto& tc : cases_) {
        const Status s = run_one(tc);
        ++counts_[s];
        per_case_status_.push_back(s);

        std::cout << "[" << color_for(s) << status_label(s) << color_reset() << "] "
                  << tc.label
                  << "  (gate=" << tc.phase_gate
                  << ", expected_fail=" << (tc.expected_fail ? "true" : "false") << ")\n";
    }
}

void AcceptanceRunner::print_summary() const {
    const int total = static_cast<int>(cases_.size());
    std::cout << "\n---------------- AcceptanceRunner summary ----------------\n";
    std::cout << "  PASS              : " << count(Status::PASS)             << "\n";
    std::cout << "  FAIL              : " << count(Status::FAIL)             << "\n";
    std::cout << "  NOT_IMPLEMENTED   : " << count(Status::NOT_IMPLEMENTED)  << "\n";
    std::cout << "  UNEXPECTED_PASS   : " << count(Status::UNEXPECTED_PASS)  << "\n";
    std::cout << "  UNEXPECTED_FAIL   : " << count(Status::UNEXPECTED_FAIL)  << "\n";
    std::cout << "  total cases       : " << total                           << "\n";
    std::cout << "  exit_code         : " << exit_code()                     << "\n";
    std::cout << "----------------------------------------------------------\n";
}

int AcceptanceRunner::exit_code() const {
    // Zero only if every case ended in PASS or (expected_fail && NOT_IMPLEMENTED).
    // row "expected_fail=false / NotImplementedError ->
    // NOT_IMPLEMENTED -> nonzero exit" requires per-case lookup, which is why
    // we keep per_case_status_ aligned with cases_.
    if (count(Status::FAIL) > 0)             return 1;
    if (count(Status::UNEXPECTED_PASS) > 0)  return 1;
    if (count(Status::UNEXPECTED_FAIL) > 0)  return 1;

    // NOT_IMPLEMENTED on a case that expected to PASS is also a failure.
    for (std::size_t i = 0; i < cases_.size() && i < per_case_status_.size(); ++i) {
        if (per_case_status_[i] == Status::NOT_IMPLEMENTED && !cases_[i].expected_fail) {
            return 1;
        }
    }
    return 0;
}

int AcceptanceRunner::count(Status s) const {
    auto it = counts_.find(s);
    return it == counts_.end() ? 0 : it->second;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string resolve_data_path(const std::string& subdir, const std::string& basename) {
    // Allow an env-var override so CI can relocate the data dir without a
    // rebuild. Falls back to the CMake compile define.
    const char* env = std::getenv("QPLAYER_ACCEPTANCE_DATA_DIR");
    const std::string base = env != nullptr ? env : QPLAYER_ACCEPTANCE_DATA_DIR;
    return base + "/" + subdir + "/" + basename;
}

std::vector<TestCase> default_phase0t_cases() {
    // Phase 4 : opt_cancel flipped to expected_fail=false.
    // Phase 5: bell, ghz_5, qft_4, oracle_10, oracle_20 flipped to false.
    //   bell/ghz_5/qft_4 are analytic -- must pass.
    //   oracle_10/oracle_20 are compared against pre-computed reference
    //   baselines; if convention differences exist they are reported as FAIL
    // and the caller is expected to investigate.
    // NOT_IMPLEMENTED on expected_fail=true -> counted as PASS.
    return {
        // Phase 5: counts check -- Bell pair.
        {resolve_data_path("qasm", "bell.qasm"),
         resolve_data_path("qasm/expected", "bell.json"),
         false, "Phase 5", "bell"},

        // Phase 5: counts check -- 5-qubit GHZ.
        {resolve_data_path("qasm", "ghz_5.qasm"),
         resolve_data_path("qasm/expected", "ghz_5.json"),
         false, "Phase 5", "ghz_5"},

        // Phase 5: statevector L2 check -- 4-qubit QFT uniform superposition.
        {resolve_data_path("qasm", "qft_4.qasm"),
         resolve_data_path("qasm/expected", "qft_4.json"),
         false, "Phase 5", "qft_4"},

        // Phase 4 : CancelInversePass transpile check -- expected_fail=false.
        // Verification: parse opt_cancel.qasm, transpile(level=1), num_ops==1.
        {resolve_data_path("qasm", "opt_cancel.qasm"),
         resolve_data_path("qasm/expected", "opt_cancel.json"),
         false, "Phase 4", "opt_cancel"},

        {resolve_data_path("qasm", "noisy_bell.qasm"),
         resolve_data_path("qasm/expected", "noisy_bell.json"),
         false, "Phase 8", "noisy_bell"},

        // Phase 7: SparseState counts check -- 100-qubit GHZ keeps two non-zero
        // amplitudes; flipped to expected_fail=false now that the SparseState
        // backend (-) is implemented (cumulative gate 7/9).
        {resolve_data_path("qasm", "sparse_100q.qasm"),
         resolve_data_path("qasm/expected", "sparse_100q.json"),
         false, "Phase 7", "sparse_100q"},

        // Phase 8: SWAP routing -- transpile() against a linear coupling
        // map inserts SWAPs; logical measurement distribution is preserved
        // Flipped to expected_fail=false now that the device-aware
        // pipeline (-) is implemented (cumulative gate 9/9).
        {resolve_data_path("qasm", "compile_swap.qasm"),
         resolve_data_path("qasm/expected", "compile_swap.json"),
         false, "Phase 8", "compile_swap"},

        // Phase 5: statevector_dump check -- 10-qubit oracle (inline amplitudes).
        // Exercises all qelib1.inc gates added by (ch/crz/cu1/cu3/ccx)
        // plus many reversed-order (ctrl>tgt) gates. Initial L2=1.41 was traced
        // to a fusion bug (fuse_circuit stored sorted qubits, swapping ctrl/tgt
        // for ctrl>tgt gates) -- fixed in gate_fusion.cpp. All individual
        // gate matrices match qelib1.inc exactly. Residual L2=1.34e-5 is the
        // 6-decimal baseline rounding floor (per-amplitude max err 8.2e-7).
        {resolve_data_path("qasm", "oracle_10.qasm"),
         resolve_data_path("qasm/expected", "oracle_10.json"),
         false, "Phase 5", "oracle_10"},

        // Phase 5: statevector_dump check -- 20-qubit oracle (sibling .data).
        // The original user-uploaded oracle_20.data was a divergent external
        // baseline (L2=0.0294 vs qelib1). Confirmed via cross-check: QPlayer
        // reproduces the qiskit 1.2.4 Statevector of oracle_20.qasm to
        // L2=7.55e-15 (machine epsilon, all gates verified vs qelib1, SWAP/CSwap
        // verified for non-adjacent orderings). oracle_20.data was regenerated
        // from the qiskit-validated statevector (%.12e, 2026-06-02), so the case
        // now passes. Whole-register broadcast (`h q;`) support also landed.
        {resolve_data_path("qasm", "oracle_20.qasm"),
         resolve_data_path("qasm/expected", "oracle_20.json"),
         false, "Phase 5", "oracle_20"},
    };
}

// The TN gate subset. Six of the nine acceptance circuits, not nine:
//
//   bell / ghz_5      counts on the MPS path
//   qft_4             statevector comparison (n <= MAX_DENSE_EXPORT_QUBITS)
//   opt_cancel        circuit_metric — transpile only, backend-independent
// noisy_bell Monte Carlo trajectories on MPS (Phase 10)
//   compile_swap      the device pipeline's output run on MPS (Phase 10-A)
//
// noisy_bell joined when the TN noise route landed (-): the circuit
// is bell.qasm with a NoiseModel supplied at runtime, which is exactly what
// execute_trajectories() carries. compile_swap joined at — it is a
// transpile-then-run row, and what it checks (logical results survive inserted
// SWAPs) is backend-independent, so the only question was whether the routed
// physical circuit runs on MPS. It does: SWAP is a 2Q gate, and the coupling
// map is the linear chain an MPS already prefers.
//
// oracle_10 joined at : it uses ccx, which the TN backend used to reject
// outright and now runs through MpsState::apply_3q()'s fixed decomposition.
//
// oracle_20 is NOT here, and the reason is the decomposition's cost rather than
// its correctness. Six CX per Toffoli each take their own SVD, so the chain needs
// far more bond dimension than the final state does; at 20 qubits with ten ccx
// that overshoots the default cap (chi_max 256) and the run turns
// LOSSY. Measured: the statevector dump misses by L2 = 1.25 with a per-amplitude
// worst of 1.3e-02 against a 1e-07 tolerance, while oracle_10 passes exactly.
// That tradeoff was named up front, and this is the number on it.
// The row comes back one of two ways: a native three-site apply, which is the
// deferred alternative, or a per-row cap knob the runner does not
// have.
//
// sparse_100q stays out by kind rather than capability: it selects SparseState.
std::vector<TestCase> tn_gate_cases() {
    // The gate label is what made the row runnable on TN, not the Phase that
    // added the circuit — that is what makes the cumulative count readable as TN
    // progress (4 at Phase 9, 5 at Phase 10, 6 at Phase 10-A, 7).
    // lands after Phase 11, the last Lynx Phase, so it names itself.
    struct TnRow { const char* label; const char* gate; };
    static constexpr TnRow kRows[] = {
        {"bell",         "Phase 9"},
        {"ghz_5",        "Phase 9"},
        {"qft_4",        "Phase 9"},
        {"opt_cancel",   "Phase 9"},
        {"noisy_bell",   "Phase 10"},
        {"compile_swap", "Phase 10-A"},
        {"oracle_10",    ""},
    };
    std::vector<TestCase> cases;
    for (const TnRow& row : kRows) {
        cases.push_back({resolve_data_path("qasm", std::string{row.label} + ".qasm"),
                         resolve_data_path("qasm/expected", std::string{row.label} + ".json"),
                         false, row.gate, std::string{row.label} + "_tn",
                         SimType::TensorNetwork});
    }
    return cases;
}

}  // namespace qplayer::acceptance
