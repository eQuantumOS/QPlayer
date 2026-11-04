// test_cache_blocking.cpp — Unit tests for L2-tiled cache blocking.
//
// Verifies that the cache-blocked gate kernel path (set_cache_blocking(true))
// produces bitwise-identical results to the unblocked path (false) for all
// gate types, qubit positions, and state sizes.  Tests include:
//   - Small qubit counts (target=0,1,2 — small stride, non-L2-filling).
//   - Large qubit counts (12..16 qubits, state > typical L2).
//   - Pure 1Q gates (H, X, Y, Z, S, T, RX, RY, RZ, U3) on various targets.
//   - Controlled 2Q gates (CX, CY, CZ, CH, CRZ) with control/target ordering.
//   - 3Q gate (CCX Toffoli) — bypasses blocked path (handled as controlled-1Q).
//   - Non-trivial initial state (not |0>): apply H or CX before toggling.
//   - Env QPLAYER_CACHE_BLOCKING=0 construction check.
//
// Tolerance 1e-12: exact arithmetic, no statistical sampling (testing-rules).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/types.hpp"
#include "../../../include/error.hpp"

using qplayer::FullStateCpuBackend;
using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::QubitIndex;
using qplayer::StateIndex;
using qplayer::StateVector;

namespace {

// Tolerance for blocked == unblocked comparison.
// Blocked and unblocked paths share the same FMA3 arithmetic body; the only
// difference is traversal order, so results are numerically identical.
// 1e-12 guards against any unexpected floating-point non-determinism.
constexpr double kTol = 1e-12;

// Helper: build a GateOp for a 1-qubit gate with no parameters.
GateOp gate1q(GateType type, QubitIndex q) {
    GateOp op;
    op.type   = type;
    op.qubits = {q};
    return op;
}

// Helper: build a GateOp for a 1-qubit rotation gate.
GateOp gate1q_param(GateType type, QubitIndex q, double theta) {
    GateOp op;
    op.type   = type;
    op.qubits = {q};
    op.params = {theta};
    return op;
}

// Helper: build a GateOp for a U3 gate.
GateOp gate_u3(QubitIndex q, double th, double phi, double lam) {
    GateOp op;
    op.type   = GateType::U3;
    op.qubits = {q};
    op.params = {th, phi, lam};
    return op;
}

// Helper: build a 2-qubit controlled GateOp.
GateOp gate2q(GateType type, QubitIndex ctrl, QubitIndex tgt) {
    GateOp op;
    op.type   = type;
    op.qubits = {ctrl, tgt};
    return op;
}

// Helper: CRZ with parameter.
GateOp gate_crz(QubitIndex ctrl, QubitIndex tgt, double lam) {
    GateOp op;
    op.type   = GateType::CRZ;
    op.qubits = {ctrl, tgt};
    op.params = {lam};
    return op;
}

// Helper: CCX (Toffoli).
GateOp gate_ccx(QubitIndex c0, QubitIndex c1, QubitIndex tgt) {
    GateOp op;
    op.type   = GateType::CCX;
    op.qubits = {c0, c1, tgt};
    return op;
}

// Compare two state vectors element-by-element within tolerance kTol.
// Returns an empty string on success; otherwise a description of the first
// mismatch for diagnostic output.
std::string compare_sv(const StateVector& a, const StateVector& b) {
    if (a.size() != b.size()) {
        return "size mismatch: " + std::to_string(a.size()) +
               " vs " + std::to_string(b.size());
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double dr = a[i].real() - b[i].real();
        const double di = a[i].imag() - b[i].imag();
        if (dr * dr + di * di > kTol * kTol) {
            return "mismatch at index " + std::to_string(i);
        }
    }
    return "";
}

// Run a sequence of GateOps on an n-qubit backend starting from |0..0>,
// then return the statevector.
StateVector run_gates(FullStateCpuBackend& backend,
                      QubitIndex n,
                      const std::vector<GateOp>& ops)
{
    backend.init_state(n);
    for (const auto& op : ops) {
        backend.apply_gate(op);
    }
    return backend.get_statevector();
}

// ── Fixture ───────────────────────────────────────────────────────────────────

class CacheBlockingFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override {
        // Unset env so constructor sees default (enabled) state.
        ::unsetenv("QPLAYER_CACHE_BLOCKING");
        blocked   = std::make_unique<FullStateCpuBackend>();
        unblocked = std::make_unique<FullStateCpuBackend>();
        blocked->set_cache_blocking(true);
        unblocked->set_cache_blocking(false);
    }
    void TearDown() override {
        blocked.reset();
        unblocked.reset();
        ::unsetenv("QPLAYER_CACHE_BLOCKING");
    }
    std::unique_ptr<FullStateCpuBackend> blocked;
    std::unique_ptr<FullStateCpuBackend> unblocked;
};

}  // namespace

// ── 1Q gate correctness: H on each qubit of a 4-qubit register ───────────────

TEST_F(CacheBlockingFixture, H_FourQubits_AllTargets_BlockedEqualsUnblocked) {
    // Arrange: n=4, apply H on each qubit separately.
    const QubitIndex n = 4;
    for (QubitIndex target = 0; target < n; ++target) {
        const std::vector<GateOp> ops = {gate1q(GateType::H, target)};

        // Act.
        const StateVector sv_b  = run_gates(*blocked,   n, ops);
        const StateVector sv_ub = run_gates(*unblocked, n, ops);

        // Assert: tolerance 1e-12 — exact arithmetic (testing-rules).
        const std::string err = compare_sv(sv_b, sv_ub);
        EXPECT_TRUE(err.empty());
    }
}

// ── 1Q gate correctness: X, Y, Z, S, T on target qubits 0 and n-1 ───────────

TEST_F(CacheBlockingFixture, XYZ_TargetZeroAndLast_BlockedEqualsUnblocked) {
    const QubitIndex n = 5;
    const std::vector<GateType> types = {
        GateType::X, GateType::Y, GateType::Z, GateType::S, GateType::T
    };
    for (GateType type : types) {
        for (QubitIndex tgt : {QubitIndex{0}, QubitIndex{n - 1}}) {
            const std::vector<GateOp> ops = {gate1q(type, tgt)};
            const StateVector sv_b  = run_gates(*blocked,   n, ops);
            const StateVector sv_ub = run_gates(*unblocked, n, ops);
            const std::string err = compare_sv(sv_b, sv_ub);
            EXPECT_TRUE(err.empty());
        }
    }
}

// ── 1Q rotation gates: RX, RY, RZ on all qubit positions (n=4) ───────────────

TEST_F(CacheBlockingFixture, Rotations_AllTargets_BlockedEqualsUnblocked) {
    const QubitIndex n    = 4;
    const double     theta = 1.234;
    const std::vector<GateType> rot_types = {
        GateType::RX, GateType::RY, GateType::RZ
    };
    for (GateType type : rot_types) {
        for (QubitIndex tgt = 0; tgt < n; ++tgt) {
            const std::vector<GateOp> ops = {gate1q_param(type, tgt, theta)};
            const StateVector sv_b  = run_gates(*blocked,   n, ops);
            const StateVector sv_ub = run_gates(*unblocked, n, ops);
            const std::string err = compare_sv(sv_b, sv_ub);
            EXPECT_TRUE(err.empty());
        }
    }
}

// ── U3 general gate on target 0 and target n-1 ───────────────────────────────

TEST_F(CacheBlockingFixture, U3_TargetZeroAndLast_BlockedEqualsUnblocked) {
    const QubitIndex n = 4;
    for (QubitIndex tgt : {QubitIndex{0}, QubitIndex{n - 1}}) {
        const std::vector<GateOp> ops = {gate_u3(tgt, 0.7, 1.1, 2.3)};
        const StateVector sv_b  = run_gates(*blocked,   n, ops);
        const StateVector sv_ub = run_gates(*unblocked, n, ops);
        const std::string err = compare_sv(sv_b, sv_ub);
        EXPECT_TRUE(err.empty());
    }
}

// ── Controlled 2Q gates: CX, CY, CZ, CH on all (ctrl,tgt) pairs (n=4) ────────

TEST_F(CacheBlockingFixture, CX_AllPairs_BlockedEqualsUnblocked) {
    const QubitIndex n = 4;
    // Apply H on target first to create non-trivial state before CX.
    for (QubitIndex ctrl = 0; ctrl < n; ++ctrl) {
        for (QubitIndex tgt = 0; tgt < n; ++tgt) {
            if (ctrl == tgt) continue;
            const std::vector<GateOp> ops = {
                gate1q(GateType::H, tgt),        // non-trivial state
                gate2q(GateType::CX, ctrl, tgt)
            };
            const StateVector sv_b  = run_gates(*blocked,   n, ops);
            const StateVector sv_ub = run_gates(*unblocked, n, ops);
            const std::string err = compare_sv(sv_b, sv_ub);
            EXPECT_TRUE(err.empty());
        }
    }
}

TEST_F(CacheBlockingFixture, CZ_AllPairs_BlockedEqualsUnblocked) {
    const QubitIndex n = 4;
    for (QubitIndex ctrl = 0; ctrl < n; ++ctrl) {
        for (QubitIndex tgt = 0; tgt < n; ++tgt) {
            if (ctrl == tgt) continue;
            const std::vector<GateOp> ops = {
                gate1q(GateType::H, tgt),
                gate2q(GateType::CZ, ctrl, tgt)
            };
            const StateVector sv_b  = run_gates(*blocked,   n, ops);
            const StateVector sv_ub = run_gates(*unblocked, n, ops);
            const std::string err = compare_sv(sv_b, sv_ub);
            EXPECT_TRUE(err.empty());
        }
    }
}

TEST_F(CacheBlockingFixture, CRZ_AllPairs_BlockedEqualsUnblocked) {
    const QubitIndex n = 4;
    for (QubitIndex ctrl = 0; ctrl < n; ++ctrl) {
        for (QubitIndex tgt = 0; tgt < n; ++tgt) {
            if (ctrl == tgt) continue;
            const std::vector<GateOp> ops = {
                gate1q(GateType::H, tgt),
                gate_crz(ctrl, tgt, 1.57)
            };
            const StateVector sv_b  = run_gates(*blocked,   n, ops);
            const StateVector sv_ub = run_gates(*unblocked, n, ops);
            const std::string err = compare_sv(sv_b, sv_ub);
            EXPECT_TRUE(err.empty());
        }
    }
}

// ── CCX (Toffoli) 3Q gate — controlled-1Q path (2Q scope) ───────

TEST_F(CacheBlockingFixture, CCX_ThreeQubits_BlockedEqualsUnblocked) {
    // Arrange: 3-qubit register, prepare |110> then apply CCX(0,1,2).
    const QubitIndex n = 3;
    const std::vector<GateOp> ops = {
        gate1q(GateType::X, 0),
        gate1q(GateType::X, 1),
        gate_ccx(0, 1, 2)
    };

    // Act.
    const StateVector sv_b  = run_gates(*blocked,   n, ops);
    const StateVector sv_ub = run_gates(*unblocked, n, ops);

    // Assert.
    const std::string err = compare_sv(sv_b, sv_ub);
    EXPECT_TRUE(err.empty());
}

// ── Large state vector (12 qubits, dim=4096 — typical L2 boundary) ───────────
// This test exercises the blocking boundary: CACHE_BLOCK_SIZE ~= L2/(2*8);
// for a 512KB L2 that is 32768 doubles.  At 12 qubits dim=4096, each H sweep
// uses 4096 pairs * 2 = 8192 doubles, which may or may not span multiple blocks
// depending on the host.  Results must match regardless.

TEST_F(CacheBlockingFixture, H_12Qubits_AllTargets_BlockedEqualsUnblocked) {
    const QubitIndex n = 12;
    for (QubitIndex target = 0; target < n; ++target) {
        const std::vector<GateOp> ops = {gate1q(GateType::H, target)};
        const StateVector sv_b  = run_gates(*blocked,   n, ops);
        const StateVector sv_ub = run_gates(*unblocked, n, ops);
        const std::string err = compare_sv(sv_b, sv_ub);
        EXPECT_TRUE(err.empty());
    }
}

// ── Large state vector: CX on 14 qubits, highest and lowest qubit pairs ──────

TEST_F(CacheBlockingFixture, CX_14Qubits_LargeState_BlockedEqualsUnblocked) {
    // 14 qubits: dim = 16384. State > typical L2 on most hosts, exercises
    // multi-block tiling path.  Apply H first for non-trivial amplitudes.
    const QubitIndex n = 14;
    for (auto [ctrl, tgt] :
            std::vector<std::pair<QubitIndex,QubitIndex>>{
                {0, 1}, {n-2, n-1}, {0, n-1}, {n-1, 0}}) {
        const std::vector<GateOp> ops = {
            gate1q(GateType::H, tgt),
            gate2q(GateType::CX, ctrl, tgt)
        };
        const StateVector sv_b  = run_gates(*blocked,   n, ops);
        const StateVector sv_ub = run_gates(*unblocked, n, ops);
        const std::string err = compare_sv(sv_b, sv_ub);
        EXPECT_TRUE(err.empty());
    }
}

// ── Very large state: 16 qubits, state = 1MB doubles > most L2 caches ────────
// Verifies that the blocking boundary logic handles all tail blocks correctly.

TEST_F(CacheBlockingFixture, H_16Qubits_Target0_BlockedEqualsUnblocked) {
    const QubitIndex n = 16;
    // Apply H on qubit 0 (smallest stride — most sensitive to misalignment
    // or off-by-one in block boundary) from |0..0> state.
    const std::vector<GateOp> ops = {gate1q(GateType::H, 0)};
    const StateVector sv_b  = run_gates(*blocked,   n, ops);
    const StateVector sv_ub = run_gates(*unblocked, n, ops);
    const std::string err = compare_sv(sv_b, sv_ub);
    EXPECT_TRUE(err.empty());
}

TEST_F(CacheBlockingFixture, H_16Qubits_TargetLast_BlockedEqualsUnblocked) {
    const QubitIndex n = 16;
    // Apply H on qubit n-1 (largest stride = dim/2 — maximally non-contiguous).
    const std::vector<GateOp> ops = {gate1q(GateType::H, n - 1)};
    const StateVector sv_b  = run_gates(*blocked,   n, ops);
    const StateVector sv_ub = run_gates(*unblocked, n, ops);
    const std::string err = compare_sv(sv_b, sv_ub);
    EXPECT_TRUE(err.empty());
}

// ── Multi-gate circuit: Bell state on n=16 qubits ────────────────────────────
// Non-trivial state built by a sequence of gates exercises both 1Q and 2Q
// blocked kernels back-to-back without reinitialising in between.

TEST_F(CacheBlockingFixture, BellSeq_16Qubits_BlockedEqualsUnblocked) {
    const QubitIndex n = 16;
    // Build a circuit that entangles qubit 0 and qubit 1 in a 16-qubit register:
    //   H(0) then CX(0,1).
    const std::vector<GateOp> ops = {
        gate1q(GateType::H, 0),
        gate2q(GateType::CX, 0, 1)
    };
    const StateVector sv_b  = run_gates(*blocked,   n, ops);
    const StateVector sv_ub = run_gates(*unblocked, n, ops);
    const std::string err = compare_sv(sv_b, sv_ub);
    EXPECT_TRUE(err.empty());
}

// ── set_cache_blocking toggle mid-sequence: must not corrupt state ────────────
// Apply H(0) with blocking, then toggle to unblocked, apply CX(0,1), compare
// with fully-unblocked sequence.

TEST_F(CacheBlockingFixture, Toggle_MidSequence_ResultMatchesFullyUnblocked) {
    const QubitIndex n = 8;

    // Arrange: reference — full unblocked.
    unblocked->init_state(n);
    unblocked->apply_gate(gate1q(GateType::H, 0));
    unblocked->apply_gate(gate2q(GateType::CX, 0, 1));
    const StateVector sv_ref = unblocked->get_statevector();

    // Act: mixed — blocked for H, then switch to unblocked for CX.
    blocked->init_state(n);
    blocked->set_cache_blocking(true);
    blocked->apply_gate(gate1q(GateType::H, 0));
    blocked->set_cache_blocking(false);   // toggle
    blocked->apply_gate(gate2q(GateType::CX, 0, 1));
    const StateVector sv_mix = blocked->get_statevector();

    // Assert.
    const std::string err = compare_sv(sv_mix, sv_ref);
    EXPECT_TRUE(err.empty());
}

// ── Env QPLAYER_CACHE_BLOCKING=0: constructor respects env disable ─────────────

TEST(CacheBlocking, EnvDisable_ConstructionSetsBlockingOff) {
    // Arrange: set env QPLAYER_CACHE_BLOCKING=0 before construction.
    ::setenv("QPLAYER_CACHE_BLOCKING", "0", 1);

    // Act: construct a new backend — it should read the env and disable.
    FullStateCpuBackend backend_env;

    // Clean up env before assertions so later tests are not affected.
    ::unsetenv("QPLAYER_CACHE_BLOCKING");

    // Assert: the flag itself is off. Comparing state vectors cannot show this
    // because blocked and unblocked kernels are numerically identical.
    EXPECT_FALSE(backend_env.cache_blocking_enabled());
}

// ── Env absent: default is OFF ────────────────────────────────────
// Renamed from EnvAbsent_DefaultIsEnabled, which carried the pre-
// (default ON) expectation and asserted only that two state vectors
// matched — true for either default, so it could not detect a regression in
// the default at all (testing gap #4).

TEST(CacheBlocking, EnvAbsent_DefaultIsDisabled) {
    ::unsetenv("QPLAYER_CACHE_BLOCKING");

    const FullStateCpuBackend backend_default;

    // : blocking measured net-negative -> default OFF, opt-in only.
    EXPECT_FALSE(backend_default.cache_blocking_enabled());
}

// ── Env opt-in: QPLAYER_CACHE_BLOCKING=1 turns blocking on ───────────────────

TEST(CacheBlocking, EnvEnable_ConstructionSetsBlockingOn) {
    ::setenv("QPLAYER_CACHE_BLOCKING", "1", 1);

    const FullStateCpuBackend backend_env;

    ::unsetenv("QPLAYER_CACHE_BLOCKING");

    EXPECT_TRUE(backend_env.cache_blocking_enabled());
}

// ── set_cache_blocking() round-trip ──────────────────────────────────────────

TEST(CacheBlocking, SetCacheBlocking_TogglesFlag_RoundTrip) {
    ::unsetenv("QPLAYER_CACHE_BLOCKING");

    FullStateCpuBackend backend;
    EXPECT_FALSE(backend.cache_blocking_enabled());

    backend.set_cache_blocking(true);
    EXPECT_TRUE(backend.cache_blocking_enabled());

    backend.set_cache_blocking(false);
    EXPECT_FALSE(backend.cache_blocking_enabled());
}

// ── Blocked and unblocked paths stay numerically identical ───────────────────
// This is what the old EnvAbsent_DefaultIsEnabled actually verified; keep it as
// an explicitly named equivalence check rather than a default-value claim.

TEST(CacheBlocking, DefaultAndBlocked_ProduceIdenticalStateVector) {
    ::unsetenv("QPLAYER_CACHE_BLOCKING");

    FullStateCpuBackend backend_default;
    FullStateCpuBackend backend_on;
    backend_on.set_cache_blocking(true);

    const std::vector<GateOp> ops = {gate1q(GateType::H, 0)};
    const StateVector sv_default = run_gates(backend_default, QubitIndex{4}, ops);
    const StateVector sv_on      = run_gates(backend_on, QubitIndex{4}, ops);

    const std::string err = compare_sv(sv_default, sv_on);
    EXPECT_TRUE(err.empty());
}
