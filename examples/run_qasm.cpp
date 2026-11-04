// run_qasm.cpp — CLI example driving an OpenQASM v2 file through the QRegister
// Batch API: parse -> optional transpile -> run -> inspect. It exercises the
// full public surface (counts, statevector dump, expectation values, seed,
// gate fusion, OpenMP threads, backend selection, circuit diagram) so
// the file doubles as a usage reference and a quick hands-on runner.
//
// Verification here is intentionally lightweight: it only checks that the
// measurement counts sum to the shot count and that a recoverable statevector
// stays normalised. Golden / expected-value comparison lives in
// tests/scenario/acceptance/run_acceptance, not in this example.
//
// Build:  cmake -B build -DQPLAYER_BUILD_EXAMPLES=ON && cmake --build build
// Run:    ./build/examples/run_qasm tests/data/qasm/bell.qasm --shots 1000 --seed 1

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "qregister.hpp"
#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"
#include "error.hpp"
#include "qtimer.hpp"

namespace {

using qplayer::SimType;
using qplayer::ProcType;

// Exit codes: distinguish usage mistakes, recoverable QPlayer errors, and
// unsupported-backend so shell callers can branch on the outcome.
constexpr int kExitOk      = 0;
constexpr int kExitError   = 1;  // parse / circuit / verify failure
constexpr int kExitUsage   = 2;  // bad command line
constexpr int kExitUnsupp  = 3;  // NotImplementedError (backend / feature)

struct Options {
    std::string              qasm_path;
    SimType                  backend          = SimType::FullState;
    std::string              backend_name     = "fullstate";
    ProcType                 proc             = ProcType::CPU;  // default CPU, opt-in GPU/auto
    std::string              proc_name        = "cpu";
    std::uint32_t            shots            = 1024;
    bool                     have_seed        = false;
    std::uint64_t            seed             = 0;
    bool                     do_transpile     = false;
    int                      opt_level        = 1;
    bool                     dump_statevector = false;
    std::vector<std::string> expects;
    bool                     enable_fusion    = true;
    std::uint32_t            fusion_width     = 4;
    bool                     have_threads     = false;
    int                      threads          = 0;
    bool                     interactive      = false;
    bool                     draw             = false;  //
    bool                     draw_ascii       = false;  // ASCII glyph style
};

void print_usage(std::ostream& os, const char* prog) {
    os << "Usage: " << prog << " <file.qasm> [options]\n\n"
       << "Run an OpenQASM v2 circuit through the QRegister Batch API.\n\n"
       << "Options:\n"
       << "  --backend NAME    fullstate | sparse | tn (default fullstate)\n"
       << "  --proc PROC       cpu | gpu | auto processor for FullState (default cpu;\n"
       << "                    gpu needs a CUDA build, else warns + CPU fallback; auto\n"
       << "                    picks GPU when available. sparse/tn are CPU-only and\n"
       << " ignore this. --interactive forces cpu)\n"
       << "  --shots N         measurement shots (default 1024)\n"
       << "  --seed S          PRNG seed for reproducible sampling\n"
       << "  --opt-level L     transpile optimization level 0..3 (enables transpile)\n"
       << "  --statevector     print the final statevector (oracle .data format)\n"
       << " --draw print the circuit as a text diagram (Unicode)\n"
       << "  --draw-ascii      print the circuit diagram in ASCII style\n"
       << "  --expect PAULI    print <PAULI> expectation (repeatable, e.g. --expect ZZ)\n"
       << "  --no-fusion       disable runtime gate fusion\n"
       << "  --fusion-width W  gate fusion max width (default 4)\n"
       << "  --threads N       OpenMP thread count\n"
       << "  --interactive     replay the circuit op-by-op via qr.interactive\n"
       << "                    (immediate gate apply + measurement collapse, single\n"
       << "                     trajectory, no sampling). Rejects if/conditional ops\n"
       << " and the tn backend\n"
       << "  -h, --help        show this help\n";
}

// Map a --backend name to SimType; false on an unknown name.
bool parse_backend(const std::string& name, SimType* out) {
    if (name == "fullstate") { *out = SimType::FullState;     return true; }
    if (name == "sparse")    { *out = SimType::SparseState;   return true; }
    if (name == "tn")        { *out = SimType::TensorNetwork; return true; }
    return false;
}

// Map a --proc name to ProcType; false on an unknown name. FullState honours
// all three; sparse/tn are CPU-only and ignore ProcType by construction.
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

// Replay a parsed circuit op-by-op through the Interactive facade:
// gates apply immediately and measurements collapse the state on the spot. This
// is a single trajectory, not a sampled distribution, so it ignores --shots /
// --no-fusion (warned by the caller) and rejects IfOp (classical-conditioned)
// ops, which need accumulated-register evaluation. Returns an exit code.
int run_interactive(qplayer::QRegister& qr, const qplayer::Circuit& circ,
                    const Options& opt) {
    qr.interactive.initialize(circ.num_qubits());

    // Walk the raw IR. The Op variant has five alternatives; interactive mode
    // supports gates / measurements / barriers and rejects IfOp + IdleOp.
    for (const qplayer::Op& op : circ.ops()) {
        if (const auto* g = std::get_if<qplayer::GateOp>(&op)) {
            qr.interactive.apply_gate(*g);
        } else if (const auto* m = std::get_if<qplayer::MeasureOp>(&op)) {
            qr.interactive.measure(m->qubit, m->cubit);
        } else if (std::holds_alternative<qplayer::BarrierOp>(op)) {
            qr.interactive.barrier();
        } else if (std::holds_alternative<qplayer::IfOp>(op)) {
            std::cerr << "error: --interactive does not support if/conditional "
                         "(IfOp) operations; use the batch run() path "
                         "\n";
            return kExitError;
        } else {  // IdleOp — only present after the noise pipeline, not raw IR.
            std::cerr << "error: --interactive encountered an IdleOp "
                         "(noise-pipeline node) in the raw circuit; unsupported\n";
            return kExitError;
        }
    }

    std::cout << "seed         : " << qr.current_seed() << "\n";

    // Output the measured classical register bits (one trajectory). result()
    // carries a single {bitstring: 1} entry; empty when the circuit has no
    // measurements (state-only inspection via --statevector / --expect).
    const auto& counts = qr.interactive.result().counts();
    if (!counts.empty()) {
        std::cout << "measured     : " << counts.begin()->first << "\n";
    } else {
        std::cout << "measured     : (no measurement ops)\n";
    }

    // expectation_value / dump_statevector read the live backend state that the
    // interactive replay just produced (shared backend_), so they reflect the
    // post-collapse state.
    for (const auto& pauli : opt.expects) {
        std::cout << "<" << pauli << "> = " << qr.expectation_value(pauli) << "\n";
    }
    if (opt.dump_statevector) {
        qr.dump_statevector(std::cout);
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string       val;
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout, argv[0]);
            return kExitOk;
        } else if (arg == "--backend") {
            if (!need_value(argc, argv, &i, "--backend", &val)) return kExitUsage;
            if (!parse_backend(val, &opt.backend)) {
                std::cerr << "error: unknown backend '" << val
                          << "' (expected fullstate | sparse | tn)\n";
                return kExitUsage;
            }
            opt.backend_name = val;
        } else if (arg == "--proc") {
            if (!need_value(argc, argv, &i, "--proc", &val)) return kExitUsage;
            if (!parse_proc(val, &opt.proc)) {
                std::cerr << "error: unknown proc '" << val
                          << "' (expected cpu | gpu | auto)\n";
                return kExitUsage;
            }
            opt.proc_name = val;
        } else if (arg == "--shots") {
            if (!need_value(argc, argv, &i, "--shots", &val)) return kExitUsage;
            opt.shots = static_cast<std::uint32_t>(std::stoul(val));
        } else if (arg == "--seed") {
            if (!need_value(argc, argv, &i, "--seed", &val)) return kExitUsage;
            opt.seed      = static_cast<std::uint64_t>(std::stoull(val));
            opt.have_seed = true;
        } else if (arg == "--opt-level") {
            if (!need_value(argc, argv, &i, "--opt-level", &val)) return kExitUsage;
            opt.opt_level    = std::stoi(val);
            opt.do_transpile = true;
        } else if (arg == "--statevector") {
            opt.dump_statevector = true;
        } else if (arg == "--draw") {
            opt.draw = true;
        } else if (arg == "--draw-ascii") {
            opt.draw       = true;
            opt.draw_ascii = true;
        } else if (arg == "--expect") {
            if (!need_value(argc, argv, &i, "--expect", &val)) return kExitUsage;
            opt.expects.push_back(val);
        } else if (arg == "--no-fusion") {
            opt.enable_fusion = false;
        } else if (arg == "--fusion-width") {
            if (!need_value(argc, argv, &i, "--fusion-width", &val)) return kExitUsage;
            opt.fusion_width = static_cast<std::uint32_t>(std::stoul(val));
        } else if (arg == "--threads") {
            if (!need_value(argc, argv, &i, "--threads", &val)) return kExitUsage;
            opt.threads      = std::stoi(val);
            opt.have_threads = true;
        } else if (arg == "--interactive") {
            opt.interactive = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            print_usage(std::cerr, argv[0]);
            return kExitUsage;
        } else if (opt.qasm_path.empty()) {
            opt.qasm_path = arg;
        } else {
            std::cerr << "error: unexpected extra argument '" << arg << "'\n";
            return kExitUsage;
        }
    }

    if (opt.qasm_path.empty()) {
        std::cerr << "error: no QASM file given\n";
        print_usage(std::cerr, argv[0]);
        return kExitUsage;
    }

    try {
        // Batch mode honours --proc (default cpu; opt-in gpu/auto). Interactive
        // mode runs on CPU statevector backends only, so force CPU
        // there — a gpu/auto choice would resolve to the GPU backend on a GPU
        // host, which interactive.initialize() rejects (warned below).
        ProcType eff_proc = opt.proc;
        if (opt.interactive && eff_proc != ProcType::CPU) eff_proc = ProcType::CPU;
        qplayer::QRegister qr{opt.backend, eff_proc};
        if (opt.have_seed)    qr.set_seed(opt.seed);
        if (opt.have_threads) qr.set_omp_threads(opt.threads);

        qplayer::Circuit circ = qr.parse_file(opt.qasm_path);
        std::cout << "circuit: " << opt.qasm_path << "\n"
                  << "  backend    : " << opt.backend_name << "\n"
                  << "  proc       : " << opt.proc_name << "\n"
                  << "  qubits     : " << circ.num_qubits() << "\n"
                  << "  cubits     : " << circ.num_cubits() << "\n"
                  << "  operations : " << circ.num_ops() << "\n";

        // Circuit diagram. Drawn against the circuit that
        // actually executes: the raw circuit in interactive mode, the
        // post-transpile circuit in batch mode (called again after transpile).
        const auto draw_style = opt.draw_ascii ? qplayer::CircuitDrawStyle::Ascii
                                               : qplayer::CircuitDrawStyle::Unicode;
        auto maybe_draw = [&](const qplayer::Circuit& c) {
            if (opt.draw) std::cout << "diagram:\n" << c.draw(draw_style);
        };

        // Interactive mode: replay the raw circuit immediately. Warn
        // on options that have no meaning under single-trajectory execution.
        if (opt.interactive) {
            std::cout << "  mode       : interactive (immediate collapse, single trajectory)\n";
            if (opt.proc_name != "cpu")
                std::cerr << "warning: --proc " << opt.proc_name << " is overridden to "
                             "cpu in --interactive mode (CPU statevector only)\n";
            if (opt.shots != 1024)
                std::cerr << "warning: --shots is ignored in --interactive mode "
                             "(single trajectory)\n";
            if (!opt.enable_fusion)
                std::cerr << "warning: --no-fusion is a no-op in --interactive mode "
                             "(runtime fusion is batch-only)\n";
            if (opt.do_transpile)
                std::cerr << "warning: --opt-level is ignored in --interactive mode; "
                             "the raw circuit is replayed\n";
            maybe_draw(circ);
            return run_interactive(qr, circ, opt);
        }

        if (opt.do_transpile) {
            circ = qr.transpile(std::move(circ), opt.opt_level);
            std::cout << "  transpiled : opt-level " << opt.opt_level
                      << " -> " << circ.num_ops() << " ops\n";
        }
        maybe_draw(circ);

        // Time only the simulation (parse / transpile excluded), the region of
        // interest for a quick perf feel. QTimer uses CLOCK_MONOTONIC.
        qplayer::QTimer timer;
        timer.start();
        const qplayer::Result r =
            qr.run(circ, opt.shots, opt.enable_fusion, opt.fusion_width);
        timer.end();

        std::cout << "seed         : " << qr.current_seed() << "\n"
                  << "shots        : " << r.shots() << "\n"
                  << "sim time     : " << timer.getTime() << "\n"
                  << "counts       :\n";
        std::uint64_t total = 0;
        for (const auto& [bits, n] : r.counts()) {
            std::cout << "  " << bits << " : " << n << "\n";
            total += n;
        }

        // Lightweight invariant 1: counts must sum to the shot count.
        if (total != r.shots()) {
            std::cerr << "verify: FAIL counts sum " << total
                      << " != shots " << r.shots() << "\n";
            return kExitError;
        }

        // Lightweight invariant 2: a recoverable statevector stays normalised.
        if (r.has_statevector()) {
            double norm2 = 0.0;
            for (const auto& z : r.statevector()) norm2 += std::norm(z);
            if (std::abs(norm2 - 1.0) > 1e-9) {
                std::cerr << "verify: FAIL |psi|^2 = " << norm2 << " (expected 1)\n";
                return kExitError;
            }
        }
        std::cout << "verify       : OK (counts sum, norm)\n";

        for (const auto& pauli : opt.expects) {
            std::cout << "<" << pauli << "> = " << qr.expectation_value(pauli) << "\n";
        }

        if (opt.dump_statevector) {
            qr.dump_statevector(std::cout);
        }
        return kExitOk;

    } catch (const qplayer::NotImplementedError& e) {
        std::cerr << "unsupported: " << e.what() << "\n"
                  << "(hint: backend '" << opt.backend_name
                  << "' is not available in this build/phase; try --backend fullstate)\n";
        return kExitUnsupp;
    } catch (const qplayer::QASMParseError& e) {
        std::cerr << "parse error: " << e.what() << "\n";
        return kExitError;
    } catch (const qplayer::QPlayerError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return kExitError;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return kExitError;
    }
}
