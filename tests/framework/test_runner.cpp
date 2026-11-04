// test_runner.cpp — implementation of TestRunner singleton.
// Output format is GoogleTest-like for familiar readability, with ANSI color
// when stdout is a tty (isatty(STDOUT_FILENO)), plain text otherwise.

#include "test_runner.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>  // isatty, STDOUT_FILENO

#include "../../include/error.hpp"  // qplayer::NotImplementedError
#include "test_groups.hpp"          // group_of / is_known_group / group_names

namespace qplayer::test {

// ── ANSI helpers ─────────────────────────────────────────────────────────────

namespace {

bool use_color() {
    static bool v = (isatty(STDOUT_FILENO) != 0);
    return v;
}

const char* green()   { return use_color() ? "\033[32m" : ""; }
const char* red()     { return use_color() ? "\033[31m" : ""; }
const char* yellow()  { return use_color() ? "\033[33m" : ""; }
const char* cyan()    { return use_color() ? "\033[36m" : ""; }
const char* reset()   { return use_color() ? "\033[0m"  : ""; }

// Print a fixed-width label prefix identical to GoogleTest's banner style.
void print_banner(const char* tag, const char* color,
                  const std::string& label) {
    std::printf("%s%-12s%s %s\n", color, tag, reset(), label.c_str());
}

// Glob match supporting '*' (any run, incl. empty) and '?' (single char).
// Iterative with backtracking on the most recent '*' (linear-ish, no recursion).
bool glob_match(const char* pat, const char* str) {
    const char* star = nullptr;
    const char* backtrack = str;
    while (*str) {
        if (*pat == '?' || *pat == *str) {
            ++pat;
            ++str;
        } else if (*pat == '*') {
            star = pat++;         // remember star, match zero chars for now
            backtrack = str;
        } else if (star) {
            pat = star + 1;       // backtrack: let '*' consume one more char
            str = ++backtrack;
        } else {
            return false;
        }
    }
    while (*pat == '*') ++pat;    // trailing stars match empty
    return *pat == '\0';
}

}  // namespace

// ── Singleton ────────────────────────────────────────────────────────────────

TestRunner& TestRunner::instance() {
    static TestRunner inst;
    return inst;
}

// ── Registration ─────────────────────────────────────────────────────────────

void TestRunner::register_test(const std::string& suite,
                               const std::string& name,
                               std::function<void()> fn,
                               bool expected_fail_flag) {
    entries_.push_back({suite, name, std::move(fn), expected_fail_flag});
}

// ── Filter matching (GoogleTest-style multi-pattern glob) ────────────────────
// Filter grammar: ':'-separated patterns. A token beginning with '-' is a
// negative pattern. A label matches iff it matches at least one positive
// pattern (or there are none) AND matches no negative pattern. '*' and '?'
// wildcards are honoured anywhere in a token.
//   "FullState*"                → all FullState suites
//   "Lexer.*:Parser.*"          → either suite
//   "*:-*Gpu*"                  → everything except GPU cases
bool TestRunner::matches_filter(const std::string& label,
                                const std::string& filter) {
    if (filter.empty()) return true;

    bool has_positive = false;
    bool positive_hit = false;
    bool negative_hit = false;

    size_t start = 0;
    while (true) {
        const size_t colon = filter.find(':', start);
        const std::string tok =
            filter.substr(start, colon == std::string::npos
                                     ? std::string::npos
                                     : colon - start);
        if (!tok.empty()) {
            if (tok.front() == '-') {
                if (glob_match(tok.c_str() + 1, label.c_str())) negative_hit = true;
            } else {
                has_positive = true;
                if (glob_match(tok.c_str(), label.c_str())) positive_hit = true;
            }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }

    if (negative_hit) return false;
    return has_positive ? positive_hit : true;
}

// ── Registered suite enumeration ─────────────────────────────────────────────

std::vector<std::string> TestRunner::registered_suites() const {
    std::vector<std::string> suites;
    suites.reserve(entries_.size());
    for (const auto& e : entries_) suites.push_back(e.suite);
    std::sort(suites.begin(), suites.end());
    suites.erase(std::unique(suites.begin(), suites.end()), suites.end());
    return suites;
}

// ── Single-test executor ─────────────────────────────────────────────────────

TestStatus TestRunner::run_one(const TestEntry& entry) {
    const bool is_xfail = entry.expected_fail;

    try {
        entry.fn();
        // Reached here without AssertionFailure — all assertions passed.
        if (is_xfail) {
            return TestStatus::UnexpectedPass;
        }
        return TestStatus::Passed;
    } catch (const AssertionFailure& af) {
        std::printf("  %sAssertion:%s %s\n", red(), reset(), af.message.c_str());
        if (is_xfail) {
            return TestStatus::ExpectedFail;
        }
        return TestStatus::Failed;
    } catch (const qplayer::NotImplementedError& e) {
        // EXPECT_NOT_IMPLEMENTED re-throws — already counted there.
        // Bare throw (e.g. from EXPECTED_FAIL_TEST body calling a stub) lands
        // here for the expected_fail case.
        std::printf("  %sNotImplementedError:%s %s\n",
                    yellow(), reset(), e.what());
        if (is_xfail) {
            // Stub throw inside an EXPECTED_FAIL_TEST body is normal (like an
            // assertion failure) — count as expected_fail.
            return TestStatus::ExpectedFail;
        }
        // For a normal test that did NOT wrap the call in EXPECT_NOT_IMPLEMENTED
        // this is an unhandled exception — treat as failure.
        return TestStatus::Failed;
    } catch (const std::exception& e) {
        std::printf("  %sException:%s %s\n", red(), reset(), e.what());
        if (is_xfail) {
            return TestStatus::UnexpectedError;
        }
        return TestStatus::Failed;
    } catch (...) {
        std::printf("  %sUnknown exception%s\n", red(), reset());
        if (is_xfail) {
            return TestStatus::UnexpectedError;
        }
        return TestStatus::Failed;
    }
}

// ── Main entry point ─────────────────────────────────────────────────────────

namespace {

// Print CLI usage to the given stream.
void print_usage(std::FILE* out) {
    std::fprintf(out,
        "Usage: qplayer_tests [options]\n"
        "  (no options)        run all registered tests (full suite)\n"
        "  --filter=PATTERN    run tests whose Suite.Case matches PATTERN\n"
        "                      (':'-separated globs; leading '-' negates; '*'/'?' wildcards)\n"
        "  --group=G[,G...]    run only tests in the given task group(s)\n"
        "  --list-groups       list task groups with test counts, then exit\n"
        "  --list-tests        list selected Suite.Case labels, then exit\n"
        "  --help, -h          show this help, then exit\n"
        "  --filter and --group accept both '--opt value' and '--opt=value' forms.\n"
        "Groups:");
    for (const auto& g : ::qplayer::test::group_names()) {
        std::fprintf(out, " %s", g.c_str());
    }
    std::fprintf(out, "\n");
}

}  // namespace

int TestRunner::run_all(int argc, char* argv[]) {
    std::string filter;
    std::string group_spec;
    bool show_help = false;
    bool list_tests = false;
    bool list_groups = false;
    std::string arg_err;

    // Parse arguments. --filter/--group accept "--opt value" and "--opt=value".
    // When neither is given, the full suite runs (backward-compatible default).
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") { show_help = true; continue; }
        if (arg == "--list-tests")  { list_tests  = true; continue; }
        if (arg == "--list-groups") { list_groups = true; continue; }

        if (arg == "--filter") {
            if (i + 1 >= argc) { arg_err = "--filter requires a value"; break; }
            filter = argv[++i]; continue;
        }
        if (arg.rfind("--filter=", 0) == 0) { filter = arg.substr(9); continue; }

        if (arg == "--group") {
            if (i + 1 >= argc) { arg_err = "--group requires a value"; break; }
            if (!group_spec.empty()) group_spec += ",";
            group_spec += argv[++i]; continue;
        }
        if (arg.rfind("--group=", 0) == 0) {
            if (!group_spec.empty()) group_spec += ",";
            group_spec += arg.substr(8); continue;
        }

        arg_err = "unrecognized argument '" + arg + "'";
        break;
    }

    if (!arg_err.empty()) {
        std::fprintf(stderr, "error: %s\n", arg_err.c_str());
        print_usage(stderr);
        exit_code_ = 2;
        return exit_code_;
    }

    if (show_help) { print_usage(stdout); return 0; }

    // Resolve --group tokens (comma-separated) into a validated set.
    std::set<std::string> groups;
    {
        size_t start = 0;
        while (start <= group_spec.size() && !group_spec.empty()) {
            const size_t comma = group_spec.find(',', start);
            const std::string g = group_spec.substr(
                start, comma == std::string::npos ? std::string::npos
                                                  : comma - start);
            if (!g.empty()) {
                if (!is_known_group(g)) {
                    std::fprintf(stderr, "error: unknown group '%s'\n", g.c_str());
                    print_usage(stderr);
                    exit_code_ = 2;
                    return exit_code_;
                }
                groups.insert(g);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    // --list-groups: report registered test/suite counts per group and exit.
    if (list_groups) {
        std::map<std::string, int> tests_in, suites_in;
        std::map<std::string, std::set<std::string>> suite_set;
        for (const auto& e : entries_) {
            const std::string grp = group_of(e.suite);
            const std::string key = grp.empty() ? "(ungrouped)" : grp;
            ++tests_in[key];
            suite_set[key].insert(e.suite);
        }
        std::printf("Task groups (registered):\n");
        for (const auto& name : group_names()) {
            std::printf("  %-11s : %4d test(s) in %2zu suite(s)\n",
                        name.c_str(), tests_in[name], suite_set[name].size());
        }
        if (tests_in.count("(ungrouped)")) {
            std::printf("  %-11s : %4d test(s) in %2zu suite(s)  <-- MAP ME\n",
                        "(ungrouped)", tests_in["(ungrouped)"],
                        suite_set["(ungrouped)"].size());
        }
        return 0;
    }

    // Collect entries surviving the group filter AND the pattern filter.
    std::vector<const TestEntry*> selected;
    for (const auto& e : entries_) {
        const std::string label = e.suite + "." + e.name;
        const bool group_ok = groups.empty() || groups.count(group_of(e.suite)) > 0;
        const bool filter_ok = matches_filter(label, filter);
        if (group_ok && filter_ok) selected.push_back(&e);
    }

    total = static_cast<int>(selected.size());

    // --list-tests: print selected labels and exit.
    if (list_tests) {
        for (const auto* e : selected) {
            std::printf("%s.%s\n", e->suite.c_str(), e->name.c_str());
        }
        std::printf("(%d test(s) selected)\n", total);
        return 0;
    }

    // Build a human-readable selection description for the banner.
    std::string sel_desc;
    if (!groups.empty()) {
        sel_desc += "group=";
        bool first = true;
        for (const auto& g : groups) { sel_desc += (first ? "" : ","); sel_desc += g; first = false; }
    }
    if (!filter.empty()) {
        if (!sel_desc.empty()) sel_desc += " ";
        sel_desc += "filter='" + filter + "'";
    }

    if (sel_desc.empty()) {
        std::printf("%s[==========]%s Running %d test(s)\n",
                    cyan(), reset(), total);
    } else {
        std::printf("%s[==========]%s Running %d test(s) [%s]\n",
                    cyan(), reset(), total, sel_desc.c_str());
    }

    // Per-group tallies for the grouped summary.
    std::map<std::string, int> g_run, g_pass, g_other;

    for (const auto* entry_ptr : selected) {
        const std::string label = entry_ptr->suite + "." + entry_ptr->name;
        const std::string grp = group_of(entry_ptr->suite);
        const std::string gkey = grp.empty() ? "(ungrouped)" : grp;
        print_banner("[ RUN      ]", cyan(), label);

        TestStatus st = run_one(*entry_ptr);
        ++g_run[gkey];

        switch (st) {
            case TestStatus::Passed:
                ++passed; ++g_pass[gkey];
                print_banner("[       OK ]", green(), label);
                break;
            case TestStatus::Failed:
                ++failed; ++g_other[gkey];
                print_banner("[  FAILED  ]", red(), label);
                break;
            case TestStatus::ExpectedFail:
                ++expected_fail; ++g_pass[gkey];
                print_banner("[ EXPECTED ]", yellow(), label);
                break;
            case TestStatus::UnexpectedPass:
                ++unexpected_pass; ++g_other[gkey];
                print_banner("[UNEXPECTED]", red(),
                             label + " (expected to fail but passed!)");
                break;
            case TestStatus::UnexpectedError:
                ++unexpected_error; ++g_other[gkey];
                print_banner("[ ERR XFIL ]", red(),
                             label + " (expected_fail got unexpected exception)");
                break;
        }
    }

    // Per-group summary (only groups that had ≥1 selected test), in defined order.
    std::printf("%s[----------]%s per-group summary\n", cyan(), reset());
    for (const auto& name : group_names()) {
        const auto it = g_run.find(name);
        if (it == g_run.end()) continue;
        const int run = it->second;
        const int pass = g_pass.count(name) ? g_pass[name] : 0;
        const int bad = g_other.count(name) ? g_other[name] : 0;
        const char* col = (bad > 0) ? red() : green();
        std::string tail = (bad > 0) ? (", " + std::to_string(bad) + " NOT ok") : "";
        std::printf("%s[ GROUP    ]%s %-11s %d run, %d ok%s\n",
                    col, reset(), name.c_str(), run, pass, tail.c_str());
    }
    if (g_run.count("(ungrouped)")) {
        std::printf("%s[ GROUP    ]%s %-11s %d run\n",
                    yellow(), reset(), "(ungrouped)", g_run["(ungrouped)"]);
    }

    // Overall summary.
    std::printf("%s[==========]%s %d test(s) ran.\n", cyan(), reset(), total);
    std::printf("%s[  PASSED  ]%s %d test(s).\n", green(), reset(), passed);
    if (expected_fail > 0) {
        std::printf("%s[ EXPECTED ]%s %d test(s) (expected failures — OK).\n",
                    yellow(), reset(), expected_fail);
    }
    if (not_implemented > 0) {
        std::printf("%s[NOT IMPL  ]%s %d test(s) (NotImplementedError caught).\n",
                    yellow(), reset(), not_implemented);
    }
    if (failed > 0) {
        std::printf("%s[  FAILED  ]%s %d test(s).\n", red(), reset(), failed);
    }
    if (unexpected_pass > 0) {
        std::printf("%s[UNEXPECTED]%s %d test(s) passed unexpectedly.\n",
                    red(), reset(), unexpected_pass);
    }
    if (unexpected_error > 0) {
        std::printf("%s[ ERR XFIL ]%s %d test(s) had unexpected errors in xfail.\n",
                    red(), reset(), unexpected_error);
    }

    // exit_code: 0 iff only passed + expected_fail; nonzero otherwise.
    exit_code_ = (failed + unexpected_pass + unexpected_error > 0) ? 1 : 0;
    return exit_code_;
}

}  // namespace qplayer::test
