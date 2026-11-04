// test_main.cpp — entry point for the qplayer_tests executable.
// Links against test_runner.cpp and all test translation units.
// Passes argc/argv to TestRunner::run_all() so --filter= is supported.

#include "test_runner.hpp"

int main(int argc, char* argv[]) {
    return ::qplayer::test::TestRunner::instance().run_all(argc, argv);
}
