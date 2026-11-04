// test_framework.cpp — self-tests for the QPlayer test framework.
// Verifies that the runner accurately classifies passed, failed, expected_fail,
// unexpected_pass, unexpected_error, and not_implemented cases.
//
// Strategy: run a nested TestRunner instance (separate from the global singleton)
// on purpose-built lambda tests, then assert on the resulting counters.
// This avoids polluting the outer runner's statistics with intentional failures.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"
#include "../../framework/test_groups.hpp"

#include <functional>
#include <stdexcept>

#include "../../../include/error.hpp"   // qplayer::NotImplementedError
#include "../../../include/qregister.hpp"

using namespace qplayer::test;

// ── Internal helper: run a set of lambdas in an isolated runner ───────────────

namespace {

struct RunResult {
    int total            = 0;
    int passed           = 0;
    int failed           = 0;
    int expected_fail    = 0;
    int unexpected_pass  = 0;
    int unexpected_error = 0;
    int not_implemented  = 0;
    int exit_code        = 0;
};

// Runs the provided entries through a *fresh* TestRunner (not the singleton)
// and returns statistics. Suppresses stdout during the nested run by
// redirecting to /dev/null — we only care about the counters.
RunResult run_isolated(std::vector<TestEntry> entries) {
    // Construct a temporary runner by populating the singleton's entries
    // then resetting. This is the simplest approach that avoids a separate
    // class — we snapshot + restore the global state.
    // Note: this is single-threaded test code only; no OpenMP racing here.

    TestRunner& runner = TestRunner::instance();

    // Save previous counters and clear.
    const int prev_total            = runner.total;
    const int prev_passed           = runner.passed;
    const int prev_failed           = runner.failed;
    const int prev_xfail            = runner.expected_fail;
    const int prev_unpass           = runner.unexpected_pass;
    const int prev_unerr            = runner.unexpected_error;
    const int prev_ni               = runner.not_implemented;

    // Temporarily reset counters — we will restore them after.
    runner.total            = 0;
    runner.passed           = 0;
    runner.failed           = 0;
    runner.expected_fail    = 0;
    runner.unexpected_pass  = 0;
    runner.unexpected_error = 0;
    runner.not_implemented  = 0;

    // We cannot call run_all (that processes ALL registered entries).
    // Instead, invoke run_one per entry and tally manually.
    RunResult r;
    r.total = static_cast<int>(entries.size());
    for (const auto& e : entries) {
        TestStatus st = runner.run_one(e);
        switch (st) {
            case TestStatus::Passed:
                ++runner.passed;
                ++r.passed;
                break;
            case TestStatus::Failed:
                ++runner.failed;
                ++r.failed;
                break;
            case TestStatus::ExpectedFail:
                ++runner.expected_fail;
                ++r.expected_fail;
                break;
            case TestStatus::UnexpectedPass:
                ++runner.unexpected_pass;
                ++r.unexpected_pass;
                break;
            case TestStatus::UnexpectedError:
                ++runner.unexpected_error;
                ++r.unexpected_error;
                break;
        }
    }
    r.not_implemented  = runner.not_implemented;
    r.exit_code = (r.failed + r.unexpected_pass + r.unexpected_error > 0) ? 1 : 0;

    // Restore global runner counters so the outer run_all sees the real totals.
    runner.total            = prev_total;
    runner.passed           = prev_passed;
    runner.failed           = prev_failed;
    runner.expected_fail    = prev_xfail;
    runner.unexpected_pass  = prev_unpass;
    runner.unexpected_error = prev_unerr;
    runner.not_implemented  = prev_ni;

    return r;
}

}  // namespace

// ── Framework self-tests ─────────────────────────────────────────────────────

TEST(Framework, PassingTest_Counted_AsPassed) {
    // Arrange: a test that always passes.
    std::vector<TestEntry> entries{
        {"X", "pass", []() { /* nothing — trivially passes */ }, false}
    };

    // Act
    RunResult r = run_isolated(entries);

    // Assert
    EXPECT_EQ(r.passed, 1);
    EXPECT_EQ(r.failed, 0);
    EXPECT_EQ(r.exit_code, 0);
}

TEST(Framework, FailingTest_Counted_AsFailed) {
    // Arrange: a test that throws AssertionFailure.
    std::vector<TestEntry> entries{
        {"X", "fail", []() {
            throw AssertionFailure{"deliberate failure"};
        }, false}
    };

    // Act
    RunResult r = run_isolated(entries);

    // Assert
    EXPECT_EQ(r.passed,  0);
    EXPECT_EQ(r.failed,  1);
    EXPECT_EQ(r.exit_code, 1);
}

TEST(Framework, ExpectedFailTest_BodyFails_Counted_AsExpectedFail) {
    // Arrange: an EXPECTED_FAIL_TEST body that throws AssertionFailure.
    std::vector<TestEntry> entries{
        {"X", "xfail", []() {
            throw AssertionFailure{"intentional stub failure"};
        }, true /* expected_fail */}
    };

    // Act
    RunResult r = run_isolated(entries);

    // Assert: body failed → expected_fail (not counted as failed).
    EXPECT_EQ(r.expected_fail, 1);
    EXPECT_EQ(r.failed,         0);
    EXPECT_EQ(r.exit_code,      0);
}

TEST(Framework, ExpectedFailTest_BodyPasses_Counted_AsUnexpectedPass) {
    // Arrange: an EXPECTED_FAIL_TEST body that passes (bad!).
    std::vector<TestEntry> entries{
        {"X", "xfail_pass", []() { /* passes trivially */ }, true}
    };

    // Act
    RunResult r = run_isolated(entries);

    // Assert: body passed in an expected_fail test → unexpected_pass.
    EXPECT_EQ(r.unexpected_pass, 1);
    EXPECT_EQ(r.exit_code,       1);
}

TEST(Framework, ExpectedFailTest_BodyThrowsOtherException_Counted_AsUnexpectedError) {
    // Arrange: xfail body throws a non-assertion std::exception (not
    // NotImplementedError and not AssertionFailure).
    std::vector<TestEntry> entries{
        {"X", "xfail_err", []() {
            throw std::runtime_error{"unexpected runtime error"};
        }, true}
    };

    // Act
    RunResult r = run_isolated(entries);

    // Assert: unexpected exception in expected_fail body → unexpected_error.
    EXPECT_EQ(r.unexpected_error, 1);
    EXPECT_EQ(r.exit_code,        1);
}

TEST(Framework, ExpectedFailTest_BodyThrowsNotImplemented_Counted_AsExpectedFail) {
    // Arrange: xfail body has a bare stub call (NotImplementedError).
    std::vector<TestEntry> entries{
        {"X", "xfail_ni", []() {
            throw qplayer::NotImplementedError{"Stub", "Phase N pending"};
        }, true}
    };

    // Act
    RunResult r = run_isolated(entries);

    // Assert: NotImplementedError in xfail body → expected_fail ().
    EXPECT_EQ(r.expected_fail, 1);
    EXPECT_EQ(r.exit_code,     0);
}

TEST(Framework, MultipleTests_Statistics_AreAccurate) {
    // Arrange: 3 passing, 2 failing, 1 xfail-failing, 1 xfail-passing.
    std::vector<TestEntry> entries{
        {"X", "p1", []() {}, false},
        {"X", "p2", []() {}, false},
        {"X", "p3", []() {}, false},
        {"X", "f1", []() { throw AssertionFailure{"f1"}; }, false},
        {"X", "f2", []() { throw AssertionFailure{"f2"}; }, false},
        {"X", "xf1", []() { throw AssertionFailure{"xf1"}; }, true},
        {"X", "xfp1", []() {}, true},
    };

    // Act
    RunResult r = run_isolated(entries);

    // Assert
    EXPECT_EQ(r.passed,          3);
    EXPECT_EQ(r.failed,          2);
    EXPECT_EQ(r.expected_fail,   1);
    EXPECT_EQ(r.unexpected_pass, 1);
    EXPECT_EQ(r.exit_code,       1);   // failed + unexpected_pass > 0
}

// ── EXPECT_THROW / EXPECT_NO_THROW sanity ────────────────────────────────────

TEST(Framework, ExpectThrow_WhenThrows_Passes) {
    EXPECT_THROW(
        throw std::runtime_error{"ok"},
        std::runtime_error);
}

TEST(Framework, ExpectThrow_WhenNoThrow_Fails) {
    std::vector<TestEntry> entries{
        {"X", "expect_throw_no_throw", []() {
            EXPECT_THROW((void)0, std::runtime_error);
        }, false}
    };
    RunResult r = run_isolated(entries);
    EXPECT_EQ(r.failed, 1);
}

TEST(Framework, ExpectNoThrow_WhenNoThrow_Passes) {
    EXPECT_NO_THROW((void)42);
}

TEST(Framework, ExpectNoThrow_WhenThrows_Fails) {
    std::vector<TestEntry> entries{
        {"X", "expect_no_throw_throws", []() {
            EXPECT_NO_THROW(throw std::runtime_error{"boom"});
        }, false}
    };
    RunResult r = run_isolated(entries);
    EXPECT_EQ(r.failed, 1);
}

// ── EXPECT_NOT_IMPLEMENTED directly on Phase 0-S stubs ───────────────────────

// : QRegister::parse() now delegates to the Phase 3 Parser facade (no
// longer a stub). The test name retains the Phase0S suite for historical
// continuity; the stub contract has been superseded.
TEST(Phase0S, ParseStub_NoLongerThrows) {
    qplayer::QRegister qr;
    EXPECT_NO_THROW(qr.parse(
        "OPENQASM 2.0;\ninclude \"qelib1.inc\";\nqreg q[1];\nh q[0];\n"));
}

// : QRegister::run() now drives the execution pipeline.
TEST(Phase0S, RunStub_NoLongerThrows) {
    qplayer::QRegister qr;
    qplayer::Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    EXPECT_NO_THROW(qr.run(circ, 100));
}

// : QRegister::transpile() is now implemented (no longer a stub).
// Verify that the default call (optimization_level=1, no device) succeeds and
// does NOT throw NotImplementedError. The test name retains Phase0S suite for
// historical continuity -- the stub contract has been superseded.
TEST(Phase0S, TranspileStub_NoLongerThrows) {
    qplayer::QRegister qr;
    qplayer::Circuit circ(2, 0);
    circ.h(0).cx(0, 1);
    // Level 1 default path must succeed without throwing.
    EXPECT_NO_THROW(qr.transpile(std::move(circ), 1));
}

// : get_statevector() now delegates to the backend (no longer a stub).
// On an uninitialised QRegister it raises SimulatorError ("before init_state"),
// not NotImplementedError — so the call must not surface the old stub marker.
TEST(Phase0S, GetStatevectorStub_NoLongerNotImplemented) {
    qplayer::QRegister qr;
    bool threw_not_implemented = false;
    try {
        qr.get_statevector();
    } catch (const qplayer::NotImplementedError&) {
        threw_not_implemented = true;
    } catch (...) {
        // SimulatorError (uninitialised state) is the expected outcome.
    }
    EXPECT_FALSE(threw_not_implemented);
}

// : parse_file() now delegates to the Parser facade. A missing file no
// longer yields NotImplementedError — it surfaces a parse/IO error instead, so
// the call must throw *something* other than the old stub marker.
TEST(Phase0S, ParseFileStub_NoLongerNotImplemented) {
    qplayer::QRegister qr;
    bool threw_not_implemented = false;
    try {
        qr.parse_file("/tmp/nonexistent-qplayer-test.qasm");
    } catch (const qplayer::NotImplementedError&) {
        threw_not_implemented = true;
    } catch (...) {
        // Any other error (missing file / parse error) is the expected outcome.
    }
    EXPECT_FALSE(threw_not_implemented);
}

TEST(Phase0S, LoadDeviceModel_MissingFile_ThrowsCircuitError) {
    // : load_device_model() is now fully implemented (stub removed).
    // A non-existent file triggers CircuitError from DeviceModel::from_json_file()
    // rather than NotImplementedError. The stub contract is retired.
    qplayer::QRegister qr;
    EXPECT_THROW(qr.load_device_model("/tmp/nonexistent.json"),
                 qplayer::CircuitError);
}

// EXPECTED_FAIL_TEST: kept as a framework xfail machinery smoke test.
// load_device_model() is now fully real. The xfail path is exercised
// via a deliberate EXPECT_EQ assertion failure ( rule ①:
// assertion failure >= 1 -> ExpectedFail pass). No exception must propagate
// from this body — only assertion failures qualify as xfail.
EXPECTED_FAIL_TEST(Phase0S, DeviceModelStubIsStillStub) {
    // Deliberate assertion failure to keep the xfail machinery exercised.
    // 1 != 2 is always false; this satisfies rule ① of EXPECTED_FAIL_TEST
    // without throwing any exception.
    EXPECT_EQ(1, 2);
}

// ── EXPECT_EQ / EXPECT_NE / EXPECT_TRUE / EXPECT_FALSE sanity ────────────────

TEST(Framework, ExpectEq_IntValues_Passes) {
    EXPECT_EQ(42, 42);
}

TEST(Framework, ExpectNe_DifferentValues_Passes) {
    EXPECT_NE(1, 2);
}

TEST(Framework, ExpectTrue_TrueExpr_Passes) {
    EXPECT_TRUE(1 + 1 == 2);
}

TEST(Framework, ExpectFalse_FalseExpr_Passes) {
    EXPECT_FALSE(1 + 1 == 3);
}

TEST(Framework, ExpectNear_WithinTolerance_Passes) {
    EXPECT_NEAR(0.5, 0.5, 1e-10);  // exact — zero diff
    EXPECT_NEAR(0.5, 0.50001, 1e-4);  // within 1e-4 tolerance
}

TEST(Framework, ExpectNear_OutsideTolerance_Fails) {
    std::vector<TestEntry> entries{
        {"X", "near_fail", []() {
            EXPECT_NEAR(0.0, 1.0, 0.5);  // diff=1.0 > tol=0.5
        }, false}
    };
    RunResult r = run_isolated(entries);
    EXPECT_EQ(r.failed, 1);
}

// ── TEST_F fixture sanity ─────────────────────────────────────────────────────

namespace {

class CounterFixture : public qplayer::test::TestFixture {
public:
    int setup_count    = 0;
    int teardown_count = 0;

    void SetUp()    override { ++setup_count;    }
    void TearDown() override { ++teardown_count; }
};

// Track counts in static storage since TEST_F creates fresh instances.
static int g_setup_count    = 0;
static int g_teardown_count = 0;

}  // namespace

TEST_F(CounterFixture, SetupAndTeardown_AreCalled) {
    // Arrange (done in SetUp)
    // Act (body runs here, fixture members are accessible)
    g_setup_count    = setup_count;
    g_teardown_count = teardown_count;

    // Assert — SetUp incremented setup_count before body ran.
    EXPECT_EQ(setup_count, 1);
}

TEST(Framework, TestF_SetupCalled_VerifiedViaGlobal) {
    // Confirms the TEST_F above ran with SetUp() called once.
    EXPECT_EQ(g_setup_count, 1);
}

// ── Filter sanity (run via qplayer_tests --filter=Framework.*) ───────────────
// This test exists so Phase 1 verification can confirm filter works.
// Run: ./build/qplayer_tests --filter=FilterSanity.Only_This_Should_Run
TEST(FilterSanity, Only_This_Should_Run) {
    EXPECT_TRUE(true);  // trivial pass
}

// ── Task-group mapping completeness (SSOT enforcement) ───────────────────────
// Every registered suite must resolve to a known task group in test_groups.hpp.
// A newly added/renamed suite left unmapped fails here, forcing classification
// so --group and the per-group summary stay complete.
TEST(Framework, EveryRegisteredSuiteHasKnownGroup) {
    for (const auto& suite : TestRunner::instance().registered_suites()) {
        const std::string grp = ::qplayer::test::group_of(suite);
        if (grp.empty() || !::qplayer::test::is_known_group(grp)) {
            FAIL(std::string("suite '") + suite +
                 "' is not mapped to a task group in test_groups.hpp");
        }
    }
}
