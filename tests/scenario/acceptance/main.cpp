// tests/scenario/acceptance/main.cpp -- CLI entry point for the QASM AcceptanceRunner
// Runs the 9 fixed cases from default_phase0t_cases() and exits with
// the runner's status code, so CTest can use the process exit code as the
// pass/fail signal (matches the qplayer_tests pattern in tests/CMakeLists.txt).
//
// Phase 0-T expected output:
//   9x [NOT_IMPL] lines + summary block reporting NOT_IMPLEMENTED=9 -> exit 0.

#include <cstdlib>
#include <iostream>

#include "run_acceptance.hpp"

int main(int /*argc*/, char** /*argv*/) {
    using namespace qplayer::acceptance;

    std::cout << "QPlayer QASM acceptance runner (Phase 0-T initial state)\n";
    std::cout << "Data dir: " << resolve_data_path("", "") << "\n\n";

    AcceptanceRunner runner{default_phase0t_cases()};
    runner.run_all();
    runner.print_summary();

    return runner.exit_code();
}
