// run_acceptance.hpp -- QASM acceptance test runner.
// Owns the 9 fixed acceptance cases defined in acceptance
// section. Each Phase Gate flips `expected_fail` to false for the newly
// covered case(s); see tests/scenario/acceptance/ for the
// cumulative timetable.
//
// The 5-state Status enum diverges from the 4-state TestRunner classification
// in tests/framework/test_runner.hpp on purpose: AcceptanceRunner compares
// test bodies AGAINST EXPECTED VALUES (an extra dimension), while TestRunner
// only inspects whether the body asserted.
//
// Phase 0-T initial state: all 9 cases registered with expected_fail=true.
// QRegister::parse_file() throws NotImplementedError at the first call ->
// Status::NOT_IMPLEMENTED -> counted as PASS -> exit_code() == 0.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace qplayer::acceptance {

enum class Status {
    PASS,             // expected_fail=false and result matched expectation
    FAIL,             // expected_fail=false and result diverged from expectation
    NOT_IMPLEMENTED,  // NotImplementedError raised -- with expected_fail=true counts as PASS
    UNEXPECTED_PASS,  // expected_fail=true but the case actually passed -> failure (warning)
    UNEXPECTED_FAIL,  // expected_fail=true but a non-NotImplemented exception fired
};

struct TestCase {
    std::string qasm_path;          // absolute path under tests/data/qasm/
    std::string expected_json_path; // absolute path under tests/data/qasm/expected/
    bool        expected_fail;      // Phase 0-T initial: true for all 7
    std::string phase_gate;         // "Phase 4", "Phase 5", ... -- for cumulative tracking
    std::string label;              // e.g. "bell" -- for logs / cross-ref
};

class AcceptanceRunner {
public:
    explicit AcceptanceRunner(std::vector<TestCase> cases);

    // Returns the per-case Status. Side-effect: increments counts_.
    Status run_one(const TestCase& tc);

    // Iterates every registered case, populating counts_ + printing a per-case
    // line to stdout. Final summary is appended by print_summary().
    void run_all();

    // Pretty summary to stdout: 5-row table + total.
    void print_summary() const;

    // 0 if every Status is PASS or NOT_IMPLEMENTED (with expected_fail=true);
    // 1 if any FAIL / UNEXPECTED_PASS / UNEXPECTED_FAIL was recorded.
    int exit_code() const;

    int count(Status s) const;

private:
    std::vector<TestCase>      cases_;
    std::vector<Status>        per_case_status_;   // populated by run_all(); aligned with cases_
    std::map<Status, int>      counts_;
};

// Convenience: returns the 9 fixed cases with expected_fail=true. Used by the
// CLI driver in tests/scenario/acceptance/main.cpp and by Phase Gate tests that flip
// individual cases to expected_fail=false (Phase 4+ uses this seed list).
std::vector<TestCase> default_phase0t_cases();

// Resolve <data_dir>/<basename> against the compile-time QPLAYER_ACCEPTANCE_DATA_DIR
// macro (or an env-var override -- see run_acceptance.cpp). Exposed only for
// the CLI driver; library callers should construct TestCase paths directly.
std::string resolve_data_path(const std::string& subdir, const std::string& basename);

}  // namespace qplayer::acceptance
