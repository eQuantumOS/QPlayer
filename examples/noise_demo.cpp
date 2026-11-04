// noise_demo.cpp — Per-channel NoiseModel + DeviceModel demonstration.
// C++ counterpart of examples/noise_demo.py: same nine cases, same shots/seed,
// same output layout, so the two files can be diffed side by side.
//
// One test case per noise channel, a combined multi-channel
// case, and a device-noise case parsed from a JSON *string* (not a file). Each
// case runs the same circuit twice — once ideal (no noise) and once noisy — and
// prints the measured probability distributions side by side.
//
// Why distributions and not raw statevectors: a noisy run is a Monte Carlo
// ensemble over trajectories (a mixed state), so a single post-run statevector
// is not a meaningful "after" state. The measured Z-basis probability
// distribution is the well-defined observable, so before/after is compared on
// counts.
//
// Backend / processor selection follows run_qasm.cpp: --backend picks the
// simulator and --proc the processor. SparseState is rejected up front because
// it is noiseless by construction — every case would throw.
//
// Build:  cmake -B build -DQPLAYER_BUILD_EXAMPLES=ON && cmake --build build
// Run:    ./build/examples/noise_demo
// /build/examples/noise_demo --backend tn
// /build/examples/noise_demo --proc gpu

#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "qregister.hpp"
#include "circuit/circuit.hpp"
#include "error.hpp"
#include "noise/device_model.hpp"
#include "noise/noise_model.hpp"

namespace {

using qplayer::ChannelType;
using qplayer::Circuit;
using qplayer::ClassicalBitIndex;
using qplayer::DeviceModel;
using qplayer::KrausChannel;
using qplayer::NoiseModel;
using qplayer::ProcType;
using qplayer::QubitIndex;
using qplayer::QRegister;
using qplayer::SimType;

constexpr std::uint32_t kShots = 8192;
constexpr std::uint64_t kSeed  = 42;

// Exit codes mirror run_qasm.cpp so shell callers can branch on the outcome.
constexpr int kExitOk     = 0;
constexpr int kExitError  = 1;  // circuit / runtime failure
constexpr int kExitUsage  = 2;  // bad command line
constexpr int kExitUnsupp = 3;  // backend cannot run this demo

// Simulator / processor the cases run on; set by main() from the command line.
// Held at namespace scope rather than threaded through every case function so
// the nine case bodies stay focused on the channel they demonstrate.
SimType  g_sim  = SimType::FullState;
ProcType g_proc = ProcType::CPU;

using BuildFn = std::function<void(Circuit&)>;
using Dist    = std::map<std::string, double>;

void print_usage(std::ostream& os, const char* prog) {
    os << "Usage: " << prog << " [options]\n\n"
       << "Demonstrate every NoiseModel channel ideal vs. noisy.\n\n"
       << "Options:\n"
       << "  --backend NAME    fullstate | tn (default fullstate). sparse parses\n"
       << "                    but exits 3 with a reason: SparseState is\n"
       << "                    noiseless by construction, so it has nothing to\n"
       << " demonstrate here. Kept as a\n"
       << "                    value so that naming an existing backend stays\n"
       << "                    distinguishable from a typo -- exit 3 unsupported\n"
       << "                    vs. exit 2 bad command line\n"
       << "  --proc PROC       cpu | gpu | auto processor (default cpu).\n"
       << "                    FullState honours all three (the GPU noisy path\n"
       << "                    routes Kraus application through the host,\n"
       << "); sparse is CPU-only; tn accepts an\n"
       << " explicit gpu to offload SVD and keeps\n"
       << "                    CPU under auto\n"
       << "  -h, --help        show this help\n";
}

// Map a --backend name to SimType; false on an unknown name.
bool parse_backend(const std::string& name, SimType* out) {
    if (name == "fullstate") { *out = SimType::FullState;     return true; }
    if (name == "sparse")    { *out = SimType::SparseState;   return true; }
    if (name == "tn")        { *out = SimType::TensorNetwork; return true; }
    return false;
}

// Map a --proc name to ProcType; false on an unknown name.
bool parse_proc(const std::string& name, ProcType* out) {
    if (name == "cpu")  { *out = ProcType::CPU;  return true; }
    if (name == "gpu")  { *out = ProcType::GPU;  return true; }
    if (name == "auto") { *out = ProcType::Auto; return true; }
    return false;
}

// Consume the value that must follow an option flag; false (with a message)
// when it is missing.
bool need_value(int argc, char** argv, int* i, const char* flag, std::string* out) {
    if (*i + 1 >= argc) {
        std::cerr << "error: " << flag << " requires a value\n";
        return false;
    }
    *out = argv[++(*i)];
    return true;
}

// Run `build(circ)` on a fresh register, return P(state). The register is built
// from the namespace-level g_sim / g_proc that main() resolved from --backend /
// --proc. A null noise model means the ideal (noiseless) reference run.
Dist measure_distribution(const NoiseModel* noise_model,
                          const BuildFn&    build,
                          QubitIndex        num_qubits,
                          ClassicalBitIndex num_cubits) {
    // ProcType is explicit rather than defaulted: Auto would pick GPU on a CUDA
    // host, and the GPU noisy path takes a different route (D2H -> CPU Kraus ->
    // H2D), so the choice must stay the caller's.
    QRegister qr(g_sim, g_proc);
    qr.set_seed(kSeed);
    if (noise_model != nullptr) {
        qr.set_noise_model(*noise_model);
    }

    Circuit circ(num_qubits, num_cubits);
    build(circ);

    // Hold the Result in a named object: run() returns by value and counts()
    // hands back a reference *into* it. Binding straight to the temporary's
    // member reference does not extend its lifetime -> dangling (-Wdangling-reference).
    const qplayer::Result result = qr.run(circ, kShots);
    const auto& counts = result.counts();
    std::uint64_t total = 0;
    for (const auto& kv : counts) total += kv.second;
    if (total == 0) total = 1;

    Dist dist;
    for (const auto& kv : counts) {
        dist[kv.first] = static_cast<double>(kv.second) / static_cast<double>(total);
    }
    return dist;
}

double lookup(const Dist& d, const std::string& key) {
    const auto it = d.find(key);
    return (it == d.end()) ? 0.0 : it->second;
}

// Print a side-by-side ideal-vs-noisy distribution table.
void report(const std::string& title, const std::string& detail,
            const Dist& ideal, const Dist& noisy) {
    std::set<std::string> keys;
    for (const auto& kv : ideal) keys.insert(kv.first);
    for (const auto& kv : noisy) keys.insert(kv.first);

    // Printed unconditionally — an empty title emits a blank separator line, which
    // is what the Python counterpart's print(title) does. Keeps the two outputs
    // byte-identical (case 9 passes "" and relies on that blank line).
    std::cout << title << '\n';
    std::cout << "  " << detail << '\n';
    std::cout << "    " << std::setw(6) << "state" << " | "
              << std::setw(7) << "ideal"  << " | "
              << std::setw(7) << "noisy"  << " | "
              << std::setw(8) << "delta"  << '\n';
    std::cout << "    " << std::string(6, '-') << "-+-" << std::string(7, '-')
              << "-+-" << std::string(7, '-') << "-+-" << std::string(8, '-') << '\n';

    for (const std::string& k : keys) {
        const double p_i = lookup(ideal, k);
        const double p_n = lookup(noisy, k);
        std::cout << "    |" << k << "> | "
                  << std::fixed << std::setprecision(3)
                  << std::setw(7) << p_i << " | "
                  << std::setw(7) << p_n << " | "
                  << std::showpos << std::setw(8) << (p_n - p_i) << std::noshowpos
                  << '\n';
    }
    std::cout << '\n';
}

// Run one channel's circuit ideal vs noisy and report the comparison.
void run_case(const std::string& title, const std::string& detail,
              const NoiseModel& noise_model, const BuildFn& build,
              QubitIndex num_qubits, ClassicalBitIndex num_cubits) {
    const Dist ideal = measure_distribution(nullptr,      build, num_qubits, num_cubits);
    const Dist noisy = measure_distribution(&noise_model, build, num_qubits, num_cubits);
    report(title, detail, ideal, noisy);
}

// Shared circuit builders — X then measure is the workhorse for Z-basis cases.
void build_x_measure(Circuit& c)  { c.x(0).measure(0, 0); }
void build_hh_measure(Circuit& c) { c.h(0).h(0).measure(0, 0); }
void build_bell(Circuit& c)       { c.h(0).cx(0, 1).measure(0, 0).measure(1, 1); }

// ── Single-channel cases ────────────────────────────────

// depolarizing channel on X. |1> leaks toward |0>.
void case_depolarizing() {
    NoiseModel nm;
    nm.add_depolarizing("x", 0.10);
    run_case("Case 1 — Depolarizing channel on X gate",
             "p=0.10; ideal |1>=1.000, depolarizing mixes toward maximally mixed",
             nm, build_x_measure, 1, 1);
}

// amplitude damping (T1 decay) on X. |1> relaxes to |0>.
void case_amplitude_damping() {
    NoiseModel nm;
    nm.add_amplitude_damping("x", 0.20);
    run_case("Case 2 — Amplitude damping on X gate",
             "gamma=0.20; ideal |1>=1.000, T1 decay pulls |1>->|0> (~0.20 to |0>)",
             nm, build_x_measure, 1, 1);
}

// phase damping (T2 dephasing). Invisible in Z basis, so use an
// H-sandwich: H|0>=|+>, dephase, H back. Ideal returns |0>; dephasing drifts
// the outcome toward 50/50. The channel fires on each H gate.
void case_phase_damping() {
    NoiseModel nm;
    nm.add_phase_damping("h", 0.50);
    run_case("Case 3 — Phase damping on H gate",
             "lam=0.50 per H; ideal H.H|0>=|0>=1.000, dephasing erases coherence -> ~50/50",
             nm, build_hh_measure, 1, 1);
}

// bit flip (X with prob p) on X. Flips |1> back to |0>.
void case_bit_flip() {
    NoiseModel nm;
    nm.add_bit_flip("x", 0.15);
    run_case("Case 4 — Bit flip on X gate",
             "p=0.15; ideal |1>=1.000, bit flip returns to |0> with prob ~0.15",
             nm, build_x_measure, 1, 1);
}

// phase flip (Z with prob p). Z-invisible, so H-sandwich again.
// A Z between two H gates becomes an X, so an odd number of flips toggles the
// outcome. The channel fires on each H gate.
void case_phase_flip() {
    NoiseModel nm;
    nm.add_phase_flip("h", 0.20);
    run_case("Case 5 — Phase flip on H gate",
             "p=0.20 per H; ideal H.H|0>=|0>=1.000, phase flips (Z) surface as |1>",
             nm, build_hh_measure, 1, 1);
}

// readout error. Classical measurement bit-flip, no state change.
// p_meas_0_given_1 = P(read 0 | true 1).
void case_readout_error() {
    NoiseModel nm;
    nm.add_readout_error(0, 0.10, 0.0);
    run_case("Case 6 — Readout error",
             "P(0|1)=0.10, P(1|0)=0.00; ideal |1>=1.000, misreads as |0> ~0.10",
             nm, build_x_measure, 1, 1);
}

// raw user-supplied Kraus operators. Here an explicit amplitude
// damping channel: K0=diag(1, sqrt(1-g)), K1=[[0,sqrt(g)],[0,0]], which
// satisfies K0^dag K0 + K1^dag K1 = I.
void case_raw_kraus() {
    const double g = 0.30;
    KrausChannel ch;
    ch.operators = {
        {{{1.0, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {std::sqrt(1.0 - g), 0.0}}},
        {{{0.0, 0.0}, {std::sqrt(g), 0.0}}, {{0.0, 0.0}, {0.0, 0.0}}},
    };
    ch.target_qubit = 0;
    ch.type         = ChannelType::Custom;

    NoiseModel nm;
    nm.add_kraus("x", ch);
    run_case("Case 7 — Raw Kraus channel on X gate",
             "custom amplitude-damping Kraus, g=0.30; ideal |1>=1.000, decays ~0.30 to |0>",
             nm, build_x_measure, 1, 1);
}

// ── Multi-channel injection ────────────────────────────────────────────────

// Several channels on one Bell circuit: depolarizing on H and CX, amplitude
// damping on CX, and readout error on both qubits. Ideal Bell is 50/50 on
// |00>/|11>; combined noise leaks population into |01>/|10>.
void case_multi_channel() {
    NoiseModel nm;
    nm.add_depolarizing("h", 0.02)
      .add_depolarizing("cx", 0.04)
      .add_amplitude_damping("cx", 0.02)
      .add_readout_error(0, 0.03, 0.03)
      .add_readout_error(1, 0.03, 0.03);
    run_case("Case 8 — Multiple channels on a Bell circuit",
             "depol(h)=0.02, depol(cx)=0.04, amp_damp(cx)=0.02, readout(q0,q1)=0.03; "
             "ideal |00>=|11>=0.500",
             nm, build_bell, 2, 2);
}

// ── Device noise from a JSON string (not a file) ───────────────────────────

// parse a DeviceModel from a JSON *string*, synthesise T1/T2
// relaxation channels with load_device(), and apply them via the gate's
// duration. The X gate's 70 ns duration against T1=100 ns produces a visible
// amplitude-damping decay (gamma = 1 - exp(-70/100) ~= 0.50).
void case_device_noise_from_string() {
    const std::string device_json = R"({
  "qubits": [
    {"T1": 100.0, "T2": 80.0}
  ],
  "gates": {
    "x": {"duration_ns": 70.0, "error_rate": 0.0}
  },
  "coupling_map": []
})";

    const DeviceModel dm = DeviceModel::from_json_string(device_json);
    NoiseModel nm;
    nm.load_device(dm);

    std::cout << "Case 9 — Device noise from JSON string\n";
    std::cout << "  parsed: num_qubits=" << dm.num_qubits()
              << ", T1(q0)=" << std::fixed << std::setprecision(1) << dm.T1(0)
              << " ns, T2(q0)=" << dm.T2(0)
              << " ns, has_t1_t2=" << (nm.has_t1_t2() ? "True" : "False") << '\n';

    const Dist ideal = measure_distribution(nullptr, build_x_measure, 1, 1);
    const Dist noisy = measure_distribution(&nm,     build_x_measure, 1, 1);
    report("",
           "X duration 70 ns vs T1 100 ns; ideal |1>=1.000, "
           "relaxation decays ~0.50 to |0>",
           ideal, noisy);
}

}  // namespace

int main(int argc, char** argv) {
    std::string backend_name = "fullstate";
    std::string proc_name    = "cpu";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string       val;
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout, argv[0]);
            return kExitOk;
        } else if (arg == "--backend") {
            if (!need_value(argc, argv, &i, "--backend", &val)) return kExitUsage;
            if (!parse_backend(val, &g_sim)) {
                std::cerr << "error: unknown backend '" << val
                          << "' (expected fullstate | sparse | tn)\n";
                return kExitUsage;
            }
            backend_name = val;
        } else if (arg == "--proc") {
            if (!need_value(argc, argv, &i, "--proc", &val)) return kExitUsage;
            if (!parse_proc(val, &g_proc)) {
                std::cerr << "error: unknown proc '" << val
                          << "' (expected cpu | gpu | auto)\n";
                return kExitUsage;
            }
            proc_name = val;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            print_usage(std::cerr, argv[0]);
            return kExitUsage;
        }
    }

    // Reject SparseState here rather than letting all nine cases throw the same
    // SimulatorError one at a time: the backend is noiseless by construction, so
    // there is no partial result worth producing.
    if (backend_name == "sparse") {
        std::cerr << "unsupported: SparseState is a noiseless backend and rejects "
                     "any NoiseModel\n"
                     "(hint: use --backend fullstate or --backend tn)\n";
        return kExitUnsupp;
    }

    std::cout << "noise demo: backend=" << backend_name
              << ", proc=" << proc_name
              << ", shots=" << kShots << ", seed=" << kSeed << "\n\n";

    try {
        case_depolarizing();
        case_amplitude_damping();
        case_phase_damping();
        case_bit_flip();
        case_phase_flip();
        case_readout_error();
        case_raw_kraus();
        case_multi_channel();
        case_device_noise_from_string();
        std::cout << "All noise demo cases completed successfully." << std::endl;
    } catch (const qplayer::NotImplementedError& e) {
        std::cerr << "[noise_demo] unsupported: " << e.what() << "\n"
                  << "(hint: backend '" << backend_name << "' / proc '" << proc_name
                  << "' does not support this case; try --backend fullstate "
                     "--proc cpu)" << std::endl;
        return kExitUnsupp;
    } catch (const qplayer::QPlayerError& e) {
        std::cerr << "[noise_demo] QPlayer error: " << e.what() << std::endl;
        return kExitError;
    }
    return kExitOk;
}
