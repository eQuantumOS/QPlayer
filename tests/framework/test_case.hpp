// test_case.hpp — TEST() and TEST_F() macros.
// Static initializers auto-register each test with TestRunner::instance()
// at program startup so no manual registration table is needed.
//
// Macro hygiene: every generated symbol uses suite+name concatenation to
// avoid collisions between suites that share a case name.

#pragma once

#include "test_runner.hpp"
#include "assertions.hpp"   // brings in QPLAYER_TEST_CAT / QPLAYER_TEST_REGISTER_

// ── Base fixture class ───────────────────────────────────────────────────────
// Fixture classes for TEST_F() should inherit from TestFixture and override
// SetUp() / TearDown(). The runner instantiates a fresh fixture per test case.

namespace qplayer::test {

class TestFixture {
public:
    virtual ~TestFixture() = default;
    virtual void SetUp()    {}
    virtual void TearDown() {}
};

}  // namespace qplayer::test

// ── TEST(suite, name) ────────────────────────────────────────────────────────
// Defines a free test function and registers it with expected_fail=false.

#define TEST(suite, name)                                                       \
    QPLAYER_TEST_REGISTER_(suite, name,                                         \
        QPLAYER_TEST_CAT(qplayer_test_fn_  , QPLAYER_TEST_CAT(suite, QPLAYER_TEST_CAT(_, name))), \
        QPLAYER_TEST_CAT(qplayer_test_reg_ , QPLAYER_TEST_CAT(suite, QPLAYER_TEST_CAT(_, name))), \
        false)

// ── TEST_F(FixtureClass, name) ───────────────────────────────────────────────
// Generates a subclass of FixtureClass, overrides TestBody(), then registers a
// lambda that allocates the subclass, calls SetUp → TestBody → TearDown.
// TearDown is called even when TestBody throws AssertionFailure so fixtures
// can release resources (files, allocations) without leaking.

#define TEST_F(FixtureClass, name)                                              \
    class QPLAYER_TEST_CAT(FixtureClass, QPLAYER_TEST_CAT(_, name))            \
        : public FixtureClass {                                                 \
    public:                                                                     \
        void TestBody();                                                        \
    };                                                                          \
    static void QPLAYER_TEST_CAT(qplayer_testf_fn_,                            \
                                  QPLAYER_TEST_CAT(FixtureClass,               \
                                                   QPLAYER_TEST_CAT(_, name)))() { \
        QPLAYER_TEST_CAT(FixtureClass, QPLAYER_TEST_CAT(_, name)) _fixture;    \
        _fixture.SetUp();                                                       \
        try {                                                                   \
            _fixture.TestBody();                                                \
        } catch (...) {                                                         \
            _fixture.TearDown();                                                \
            throw;                                                              \
        }                                                                       \
        _fixture.TearDown();                                                    \
    }                                                                           \
    namespace {                                                                 \
    struct QPLAYER_TEST_CAT(qplayer_testf_reg_,                                \
                             QPLAYER_TEST_CAT(FixtureClass,                    \
                                             QPLAYER_TEST_CAT(_, name))) {      \
        QPLAYER_TEST_CAT(qplayer_testf_reg_,                                   \
                         QPLAYER_TEST_CAT(FixtureClass,                        \
                                         QPLAYER_TEST_CAT(_, name)))() {        \
            ::qplayer::test::TestRunner::instance().register_test(             \
                #FixtureClass, #name,                                           \
                QPLAYER_TEST_CAT(qplayer_testf_fn_,                            \
                                  QPLAYER_TEST_CAT(FixtureClass,               \
                                                   QPLAYER_TEST_CAT(_, name))), \
                false);                                                         \
        }                                                                       \
    };                                                                          \
    static QPLAYER_TEST_CAT(qplayer_testf_reg_,                                \
                             QPLAYER_TEST_CAT(FixtureClass,                    \
                                             QPLAYER_TEST_CAT(_, name)))       \
        QPLAYER_TEST_CAT(qplayer_testf_reg_,                                   \
                         QPLAYER_TEST_CAT(FixtureClass,                        \
                                         QPLAYER_TEST_CAT(_, QPLAYER_TEST_CAT(name, _inst_)))); \
    }                                                                           \
    void QPLAYER_TEST_CAT(FixtureClass,                                        \
                          QPLAYER_TEST_CAT(_, name))::TestBody()
