// test_pass_manager.cpp — Unit tests for PassManager.
// Verifies add_pass() ownership transfer, size() bookkeeping, run() value
// chain, make_default() factory, and integration with SemanticValidatorPass.
// Stub passes that are not yet implemented (NotImplementedError) are tested
// via EXPECT_NOT_IMPLEMENTED where appropriate.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cstddef>
#include <memory>
#include <string>

#include "../../../include/circuit/circuit.hpp"
#include "../../../include/error.hpp"
#include "../../../include/transpiler/ipass.hpp"
#include "../../../include/transpiler/pass_manager.hpp"
#include "../../../include/transpiler/passes/optimization/cancel_inverse.hpp"
#include "../../../include/transpiler/passes/scheduling/alap_schedule.hpp"
#include "../../../include/transpiler/passes/validation/semantic_validator_pass.hpp"

using qplayer::ALAPSchedulePass;
using qplayer::CancelInversePass;
using qplayer::Circuit;
using qplayer::IPass;
using qplayer::PassManager;
using qplayer::QubitIndex;
using qplayer::SemanticValidatorPass;

// ── Minimal concrete pass for wiring tests ───────────────────────────────────
// A do-nothing pass that records how many times it was called, letting tests
// verify PassManager invocation order and value chain behaviour without
// triggering NotImplementedError from the real stub passes.

namespace {

class CountingPass : public IPass {
public:
    explicit CountingPass(std::string n) : name_(std::move(n)) {}

    std::string name() const override { return name_; }

    Circuit run(Circuit circuit) override {
        ++call_count_;
        changed_ = false;
        return circuit;
    }

    bool changed() const override { return changed_; }

    int call_count() const { return call_count_; }

private:
    std::string name_;
    int         call_count_ = 0;
    bool        changed_    = false;
};

}  // anonymous namespace

// ── Empty PassManager has size 0 ──────────────────────────────────────────────

TEST(PassManager, Default_Size_IsZero) {
    PassManager pm;
    EXPECT_EQ(pm.size(), std::size_t{0});
}

// ── add_pass transfers ownership and increments size ─────────────────────────

TEST(PassManager, AddPass_SinglePass_SizeIsOne) {
    PassManager pm;
    pm.add_pass(std::make_unique<CountingPass>("p1"));
    EXPECT_EQ(pm.size(), std::size_t{1});
}

TEST(PassManager, AddPass_TwoPasses_SizeIsTwo) {
    PassManager pm;
    pm.add_pass(std::make_unique<CountingPass>("p1"));
    pm.add_pass(std::make_unique<CountingPass>("p2"));
    EXPECT_EQ(pm.size(), std::size_t{2});
}

// ── run() with no passes returns circuit unchanged ────────────────────────────

TEST(PassManager, Run_NoPasses_ReturnsSameNumQubits) {
    PassManager pm;
    Circuit in(3);
    Circuit out = pm.run(std::move(in));
    EXPECT_EQ(out.num_qubits(), QubitIndex{3});
}

// ── run() invokes each pass exactly once in order ────────────────────────────

TEST(PassManager, Run_TwoPasses_EachCalledOnce) {
    auto* p1_raw = new CountingPass("p1");
    auto* p2_raw = new CountingPass("p2");

    PassManager pm;
    pm.add_pass(std::unique_ptr<IPass>(p1_raw));
    pm.add_pass(std::unique_ptr<IPass>(p2_raw));

    Circuit c(1);
    pm.run(std::move(c));

    EXPECT_EQ(p1_raw->call_count(), 1);
    EXPECT_EQ(p2_raw->call_count(), 1);
}

// ── make_default() registers Level-1 passes ───────────────────────────
// make_default() registers exactly 3 Level-1 passes:
// CancelInverse → CommutationReorder → MergeSingleQubit.
// SemanticValidatorPass is NOT included because Transpiler::transpile()
// inserts it dynamically at position ⓪.

TEST(PassManager, MakeDefault_IsCallable) {
    PassManager pm = PassManager::make_default();
    // make_default() registers exactly 3 Level-1 optimization passes.
    EXPECT_EQ(pm.size(), std::size_t{3});
}

// ── SemanticValidatorPass integrates correctly into PassManager ───────────────

TEST(PassManager, RunWithSemanticValidator_CleanCircuit_NoThrow) {
    PassManager pm;
    pm.add_pass(std::make_unique<SemanticValidatorPass>());
    EXPECT_EQ(pm.size(), std::size_t{1});

    Circuit c(2, 2);
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

    EXPECT_NO_THROW(pm.run(std::move(c)));
}

// ── CancelInversePass is now real — verify it runs without throwing ────

TEST(PassManager, CancelInversePass_RunsWithoutThrow) {
    // CancelInversePass is fully implemented — should no longer throw
    // NotImplementedError. A simple single-qubit circuit must pass through.
    CancelInversePass pass;
    Circuit c(1);
    c.h(0);
    EXPECT_NO_THROW(pass.run(std::move(c)));
}

// ── ALAPSchedulePass is now real ───────────────────────────

TEST(PassManager, ALAPSchedulePass_RunsWithoutThrow) {
    // ALAPSchedulePass is implemented — schedules + inserts slot-aware
    // idle. An untimed circuit (no device durations) is a no-op pass-through.
    ALAPSchedulePass pass;
    Circuit c(1);
    c.h(0);
    EXPECT_NO_THROW(pass.run(std::move(c)));
}
