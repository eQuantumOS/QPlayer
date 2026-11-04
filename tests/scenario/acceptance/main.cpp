// tests/scenario/acceptance/main.cpp -- CLI entry point for the QASM AcceptanceRunner
// Runs the 9 fixed cases from default_phase0t_cases() and exits
// the runner's status code, so CTest can use the process exit code as the
// pass/fail signal (matches the qplayer_tests pattern in tests/CMakeLists.txt).
//
// Phase 0-T expected output:
//   9x [NOT_IMPL] lines + summary block reporting NOT_IMPLEMENTED=9 -> exit 0.

#include <cstdlib>
#include <iostream>
#include <string>

#include "run_acceptance.hpp"

int main(int argc, char** argv) {
    using namespace qplayer::acceptance;

    // --sim=tn runs the TN-capable subset on SimType::TensorNetwork instead of
    // the 9 default rows. Keeping it a separate invocation leaves the cumulative
    // "9/9" summary untouched while giving the Phase 9 TN claim a real run.
    bool tn_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if (arg == "--sim=tn") {
            tn_mode = true;
        } else {
            std::cerr << "unknown argument: " << arg << " (accepted: --sim=tn)\n";
            return 2;
        }
    }

    std::cout << "QPlayer QASM acceptance runner"
              << (tn_mode ? " (TensorNetwork subset)" : " (default cases)") << "\n";
    std::cout << "Data dir: " << resolve_data_path("", "") << "\n\n";

    AcceptanceRunner runner{tn_mode ? tn_gate_cases() : default_phase0t_cases()};
    runner.run_all();
    runner.print_summary();

    return runner.exit_code();
}
