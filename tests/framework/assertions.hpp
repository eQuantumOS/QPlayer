// assertions.hpp — QPlayer test assertion macros.
// All 12 macros listed in the SSOT table are implemented here.
// Failures are reported via AssertionFailure (not std::exception) so the runner
// can distinguish soft assertion failures from hard unexpected exceptions.
//
// Floating-point comparisons MUST use EXPECT_NEAR; using EXPECT_EQ on floating-
// point types triggers a static_assert to catch the mistake at compile time.

#pragma once

#include <cmath>
#include <sstream>
#include <string>
#include <type_traits>

#include "test_runner.hpp"           // AssertionFailure
#include "../../include/error.hpp"   // qplayer::NotImplementedError

// ── Internal helper ──────────────────────────────────────────────────────────

// Builds a formatted failure message and throws AssertionFailure.
// Declared inline so the macro call site's __FILE__/__LINE__ stay accurate.
namespace qplayer::test::detail {

[[noreturn]] inline void fail_here(const char* file, int line,
                                   const std::string& msg) {
    std::ostringstream oss;
    oss << file << ":" << line << ": " << msg;
    throw AssertionFailure{oss.str()};
}

// Increment the runner's not_implemented counter (used by EXPECT_NOT_IMPLEMENTED).
inline void count_not_implemented() {
    TestRunner::instance().not_implemented++;
}

}  // namespace qplayer::test::detail

// ── FAIL(message) ────────────────────────────────────────────────────────────
// Explicit failure with caller-supplied message.

#define FAIL(message) \
    ::qplayer::test::detail::fail_here(__FILE__, __LINE__, (message))

// ── EXPECT_TRUE / EXPECT_FALSE ───────────────────────────────────────────────

#define EXPECT_TRUE(expr)                                                       \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::ostringstream _oss;                                            \
            _oss << "EXPECT_TRUE(" #expr ") failed";                            \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__, _oss.str()); \
        }                                                                       \
    } while (false)

#define EXPECT_FALSE(expr)                                                      \
    do {                                                                        \
        if ((expr)) {                                                           \
            std::ostringstream _oss;                                            \
            _oss << "EXPECT_FALSE(" #expr ") failed";                           \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__, _oss.str()); \
        }                                                                       \
    } while (false)

// ── EXPECT_EQ / EXPECT_NE ────────────────────────────────────────────────────
// Guard against accidental floating-point use: if T is float/double/long double
// the static_assert fires at compile time, directing the user to EXPECT_NEAR.

#define EXPECT_EQ(actual, expected)                                             \
    do {                                                                        \
        using _T = std::decay_t<decltype(actual)>;                             \
        static_assert(!std::is_floating_point_v<_T>,                           \
            "Use EXPECT_NEAR for floating-point comparisons"); \
        if (!((actual) == (expected))) {                                        \
            std::ostringstream _oss;                                            \
            _oss << "EXPECT_EQ failed\n"                                        \
                 << "  actual:   " << (actual) << "\n"                         \
                 << "  expected: " << (expected);                               \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__, _oss.str()); \
        }                                                                       \
    } while (false)

#define EXPECT_NE(actual, expected)                                             \
    do {                                                                        \
        using _T = std::decay_t<decltype(actual)>;                             \
        static_assert(!std::is_floating_point_v<_T>,                           \
            "Use EXPECT_NEAR for floating-point comparisons"); \
        if ((actual) == (expected)) {                                           \
            std::ostringstream _oss;                                            \
            _oss << "EXPECT_NE failed: both sides equal " << (actual);         \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__, _oss.str()); \
        }                                                                       \
    } while (false)

// ── EXPECT_NEAR ──────────────────────────────────────────────────────────────
// Mandatory for floating-point comparisons. Tolerance rationale MUST be
// documented in a comment adjacent to the call site.

#define EXPECT_NEAR(actual, expected, tol)                                      \
    do {                                                                        \
        const auto _a = (actual);                                               \
        const auto _e = (expected);                                             \
        const auto _t = (tol);                                                  \
        if (std::abs(_a - _e) > _t) {                                          \
            std::ostringstream _oss;                                            \
            _oss << "EXPECT_NEAR failed\n"                                      \
                 << "  actual:    " << _a << "\n"                               \
                 << "  expected:  " << _e << "\n"                               \
                 << "  tolerance: " << _t << "\n"                               \
                 << "  diff:      " << std::abs(_a - _e);                       \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__, _oss.str()); \
        }                                                                       \
    } while (false)

// ── EXPECT_THROW ─────────────────────────────────────────────────────────────
// Verifies that evaluating `expr` throws an exception of exactly `ExcType`
// (or a publicly-derived type). If it throws something else or does not throw,
// the test fails.

#define EXPECT_THROW(expr, ExcType)                                             \
    do {                                                                        \
        bool _threw = false;                                                    \
        bool _right_type = false;                                               \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (const ExcType&) {                                              \
            _threw = true;                                                      \
            _right_type = true;                                                 \
        } catch (...) {                                                         \
            _threw = true;                                                      \
        }                                                                       \
        if (!_threw) {                                                          \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__,              \
                "EXPECT_THROW(" #expr ", " #ExcType ") — no exception thrown"); \
        } else if (!_right_type) {                                              \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__,              \
                "EXPECT_THROW(" #expr ", " #ExcType ") — wrong exception type");\
        }                                                                       \
    } while (false)

// ── EXPECT_NO_THROW ──────────────────────────────────────────────────────────

#define EXPECT_NO_THROW(expr)                                                   \
    do {                                                                        \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (const std::exception& _ex) {                                  \
            std::ostringstream _oss;                                            \
            _oss << "EXPECT_NO_THROW(" #expr ") — threw: " << _ex.what();      \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__, _oss.str()); \
        } catch (...) {                                                         \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__,              \
                "EXPECT_NO_THROW(" #expr ") — threw unknown exception");        \
        }                                                                       \
    } while (false)

// ── EXPECT_NOT_IMPLEMENTED ───────────────────────────────────────────────────
// Alias for EXPECT_THROW(expr, qplayer::NotImplementedError) with an extra
// counter increment so the runner summary shows "not_implemented" separately.
// Used by Phase 0-S / 0-T stub boundary tests.

#define EXPECT_NOT_IMPLEMENTED(expr)                                            \
    do {                                                                        \
        enum class _NiResult { NotThrown, Correct, Wrong };                    \
        _NiResult _result = _NiResult::NotThrown;                               \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (const ::qplayer::NotImplementedError&) {                       \
            _result = _NiResult::Correct;                                       \
        } catch (...) {                                                         \
            _result = _NiResult::Wrong;                                         \
        }                                                                       \
        if (_result == _NiResult::NotThrown) {                                  \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__,              \
                "EXPECT_NOT_IMPLEMENTED(" #expr ") -- no exception thrown");    \
        } else if (_result == _NiResult::Wrong) {                               \
            ::qplayer::test::detail::fail_here(__FILE__, __LINE__,              \
                "EXPECT_NOT_IMPLEMENTED(" #expr ") -- wrong exception type "    \
                "(expected NotImplementedError)");                               \
        } else {                                                                \
            ::qplayer::test::detail::count_not_implemented();                   \
        }                                                                       \
    } while (false)

// ── EXPECTED_FAIL_TEST(suite, test) ──────────────────────────────────────────
// Declares a test that is expected to fail.
//
// Semantics:
//   - Body has ≥1 assertion failure → ExpectedFail (counts as pass).
//   - Body passes all assertions    → UnexpectedPass (counts as failure).
//   - Body throws non-assertion exception (incl. NotImplementedError from a
//     bare stub call without EXPECT_NOT_IMPLEMENTED wrapper)
//                                   → UnexpectedError inside EXPECTED_FAIL_TEST
//     BUT: if the body only calls a stub (throws NotImplementedError) without
//     any assertion wrapper, the runner's run_one() classifies the bare throw
//     as ExpectedFail because is_xfail=true and the exception is NotImplementedError.
//
// Implementation: the macro defines the test function and registers it with
// expected_fail=true. The body is written inline by the caller, just like TEST().

// Unique symbol helper — concatenates tokens after one expansion pass.
#define QPLAYER_TEST_CAT_(a, b) a ## b
#define QPLAYER_TEST_CAT(a, b)  QPLAYER_TEST_CAT_(a, b)

// Registration object declared once per test to trigger the static initializer.
#define QPLAYER_TEST_REGISTER_(suite, name, fn_sym, reg_sym, is_xfail)         \
    static void fn_sym();                                                       \
    namespace {                                                                 \
    struct reg_sym {                                                             \
        reg_sym() {                                                             \
            ::qplayer::test::TestRunner::instance().register_test(             \
                #suite, #name, fn_sym, is_xfail);                              \
        }                                                                       \
    };                                                                          \
    static reg_sym QPLAYER_TEST_CAT(reg_sym, _instance_);                      \
    }                                                                           \
    static void fn_sym()

#define EXPECTED_FAIL_TEST(suite, test)                                         \
    QPLAYER_TEST_REGISTER_(suite, test,                                         \
        QPLAYER_TEST_CAT(qplayer_xfail_fn_  , QPLAYER_TEST_CAT(suite, QPLAYER_TEST_CAT(_, test))), \
        QPLAYER_TEST_CAT(qplayer_xfail_reg_ , QPLAYER_TEST_CAT(suite, QPLAYER_TEST_CAT(_, test))), \
        true)
