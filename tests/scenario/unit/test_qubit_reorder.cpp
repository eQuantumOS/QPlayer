// test_qubit_reorder.cpp — Unit tests for the qubit-reorder primitive
//
//
// Tests cover the five correctness requirements:
//   Reorder01: round-trip identity (apply perm then inverse_perm -> unchanged).
//   Reorder02: identity permutation leaves state unchanged.
//   Reorder03: inverse_permutation correctness (inverse[perm[i]] == i).
//   Reorder04: swap-two-qubits via perm matches explicit SWAP gate result.
//   Reorder05: invalid permutation (bad size or duplicate) -> SimulatorError.
//
// (Arrange/Act/Assert), (EXPECT_NEAR 1e-15 exact).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/backend/full_state/qubit_reorder.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/error.hpp"
#include "../../../include/types.hpp"

using qplayer::FullStateCpuBackend;
using qplayer::Circuit;
using qplayer::GateType;
using qplayer::QubitIndex;
using qplayer::StateIndex;
using qplayer::RawStateView;
using qplayer::SimulatorError;
using qplayer::detail::reorder_qubits;
using qplayer::detail::inverse_permutation;

namespace {

// ── Helper: allocate a 64-byte-aligned copy of re/im buffers ─────────────────

struct AlignedBuf {
    double* re;
    double* im;
    StateIndex dim;

    explicit AlignedBuf(StateIndex d) : dim(d) {
        const std::size_t bytes   = static_cast<std::size_t>(d) * sizeof(double);
        const std::size_t align   = 64;
        const std::size_t padded  = (bytes + align - 1) & ~(align - 1);
        re = static_cast<double*>(std::aligned_alloc(align, padded));
        im = static_cast<double*>(std::aligned_alloc(align, padded));
        if (!re || !im) {
            std::free(re);
            std::free(im);
            re = im = nullptr;
        }
    }

    // Copy from a RawStateView (const pointers) into this mutable buffer.
    void copy_from(const RawStateView& rv) {
        const std::size_t bytes = static_cast<std::size_t>(dim) * sizeof(double);
        std::memcpy(re, rv.re, bytes);
        std::memcpy(im, rv.im, bytes);
    }

    ~AlignedBuf() {
        std::free(re);
        std::free(im);
    }

    // Non-copyable.
    AlignedBuf(const AlignedBuf&)            = delete;
    AlignedBuf& operator=(const AlignedBuf&) = delete;
};

// Build a non-trivial state using the CPU backend: apply H on each qubit so
// all 2^n amplitudes are equal to 1/sqrt(2^n), giving a known reference.
// Returns the backend (caller keeps ownership) with state already prepared.
std::unique_ptr<FullStateCpuBackend>
make_uniform_state(QubitIndex n_qubits) {
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
    auto b = std::make_unique<FullStateCpuBackend>();
    b->init_state(n_qubits);
    Circuit circ(n_qubits);
    for (QubitIndex q = 0; q < n_qubits; ++q) {
        circ.h(q);
    }
    for (const auto& op : circ.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            b->apply_gate(std::get<qplayer::GateOp>(op));
        }
    }
    return b;
}

// Fixture: create a non-trivial 3-qubit state (H on all qubits) and expose
// mutable aligned copies of re/im for direct reorder_qubits testing.
class ReorderFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override {
        ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
        n_qubits = 3;
        backend  = make_uniform_state(n_qubits);
        RawStateView rv = backend->get_statevector_raw();
        dim = rv.size;
        buf = std::make_unique<AlignedBuf>(dim);
        buf->copy_from(rv);
        // Keep a reference copy for round-trip comparison.
        ref = std::make_unique<AlignedBuf>(dim);
        ref->copy_from(rv);
    }
    void TearDown() override {
        buf.reset();
        ref.reset();
        backend.reset();
        ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
    }

    QubitIndex                           n_qubits = 0;
    StateIndex                           dim      = 0;
    std::unique_ptr<FullStateCpuBackend> backend;
    std::unique_ptr<AlignedBuf>          buf;   // mutable working copy
    std::unique_ptr<AlignedBuf>          ref;   // immutable reference copy
};

}  // namespace

// ── Reorder01: round-trip identity ───────────────────────────────────────────
// Apply perm = [2,0,1] to a 3-qubit state, then apply inverse_permutation(perm).
// Result must equal the original state within 1e-15 (exact floating-point copy).

TEST_F(ReorderFixture, Reorder01_RoundTrip_IsIdentity) {
    // Arrange: perm cycles qubits 0->1->2->0.
    const std::vector<QubitIndex> perm    = {2, 0, 1};
    const std::vector<QubitIndex> inv_perm = inverse_permutation(perm);

    // Act: forward then inverse permutation.
    reorder_qubits(buf->re, buf->im, dim, n_qubits, perm);
    reorder_qubits(buf->re, buf->im, dim, n_qubits, inv_perm);

    // Assert: every amplitude must match the original within 1e-15.
    // 1e-15 is valid here because reorder_qubits is pure copy/scatter — no
    // floating-point arithmetic that could accumulate rounding error.
    for (StateIndex i = 0; i < dim; ++i) {
        EXPECT_NEAR(buf->re[i], ref->re[i], 1e-15);
        EXPECT_NEAR(buf->im[i], ref->im[i], 1e-15);
    }
}

// ── Reorder02: identity permutation leaves state unchanged ────────────────────

TEST_F(ReorderFixture, Reorder02_IdentityPerm_StateUnchanged) {
    // Arrange: identity permutation [0,1,2].
    std::vector<QubitIndex> id_perm(n_qubits);
    for (QubitIndex i = 0; i < n_qubits; ++i) {
        id_perm[i] = i;
    }

    // Act.
    reorder_qubits(buf->re, buf->im, dim, n_qubits, id_perm);

    // Assert: state must be bitwise unchanged (pure copy path, 1e-15 tolerance).
    for (StateIndex i = 0; i < dim; ++i) {
        EXPECT_NEAR(buf->re[i], ref->re[i], 1e-15);
        EXPECT_NEAR(buf->im[i], ref->im[i], 1e-15);
    }
}

// ── Reorder03: inverse_permutation correctness ────────────────────────────────
// inverse[perm[i]] == i for all i, tested on several permutations.

TEST(QubitReorder, Reorder03_InversePermutation_IsCorrect) {
    // Arrange: three different permutations of [0,1,2,3].
    const std::vector<std::vector<QubitIndex>> perms = {
        {0, 1, 2, 3},       // identity
        {3, 2, 1, 0},       // reversal
        {1, 2, 3, 0},       // left-cycle
        {2, 0, 3, 1},       // arbitrary
    };

    for (const auto& perm : perms) {
        const auto inv = inverse_permutation(perm);
        const QubitIndex n = static_cast<QubitIndex>(perm.size());

        // Assert: inverse[perm[i]] == i.
        for (QubitIndex i = 0; i < n; ++i) {
            EXPECT_EQ(inv[perm[i]], i);
        }
        // Also: perm[inverse[i]] == i (two-sided inverse).
        for (QubitIndex i = 0; i < n; ++i) {
            EXPECT_EQ(perm[inv[i]], i);
        }
    }
}

// ── Reorder04: swap two qubits via perm matches explicit SWAP gate ────────────
// Circuit: prepare |01> (X on q0), then compare:
//   Path A: apply SWAP(0,1) gate -> backend->get_statevector_raw().
//   Path B: reorder_qubits with perm [1,0] on the unswapped state.
// The two resulting amplitude arrays must agree within 1e-15.

TEST(QubitReorder, Reorder04_SwapTwoQubits_MatchesExplicitSwap) {
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");

    // Arrange: prepare |10> (q0=1, q1=0) on both backends.
    // Bit convention: bit q of index i = (i >> q) & 1.
    // |10> means qubit 0 = 1 -> index bit 0 = 1 -> index = 1.
    auto backA = std::make_unique<FullStateCpuBackend>();
    auto backB = std::make_unique<FullStateCpuBackend>();
    Circuit circ(2);
    circ.x(0);  // flip q0: |00> -> |10> (index 1)

    backA->init_state(2);
    backB->init_state(2);
    for (const auto& op : circ.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            backA->apply_gate(std::get<qplayer::GateOp>(op));
            backB->apply_gate(std::get<qplayer::GateOp>(op));
        }
    }

    // Path A: apply SWAP(0,1) gate.
    Circuit swap_circ(2);
    swap_circ.swap(0, 1);
    for (const auto& op : swap_circ.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            backA->apply_gate(std::get<qplayer::GateOp>(op));
        }
    }

    // Path B: reorder_qubits with perm=[1,0] (exchange bit 0 and bit 1).
    RawStateView rvB = backB->get_statevector_raw();
    const StateIndex dim = rvB.size;
    AlignedBuf bufB(dim);
    bufB.copy_from(rvB);
    const std::vector<QubitIndex> swap_perm = {1, 0};
    reorder_qubits(bufB.re, bufB.im, dim, 2, swap_perm);

    // Assert: every amplitude must match within 1e-15.
    RawStateView rvA = backA->get_statevector_raw();
    for (StateIndex i = 0; i < dim; ++i) {
        EXPECT_NEAR(bufB.re[i], rvA.re[i], 1e-15);
        EXPECT_NEAR(bufB.im[i], rvA.im[i], 1e-15);
    }

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// ── Reorder05a: size mismatch -> SimulatorError ───────────────────────────────

TEST(QubitReorder, Reorder05a_SizeMismatch_ThrowsSimulatorError) {
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
    AlignedBuf buf(4);  // dim=4 (2 qubits)
    // perm has 3 elements but n_qubits=2 -> mismatch.
    const std::vector<QubitIndex> bad_perm = {0, 1, 2};
    EXPECT_THROW(reorder_qubits(buf.re, buf.im, 4, 2, bad_perm),
                 SimulatorError);
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// ── Reorder05b: duplicate element -> SimulatorError ──────────────────────────

TEST(QubitReorder, Reorder05b_DuplicateInPerm_ThrowsSimulatorError) {
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
    AlignedBuf buf(4);  // dim=4 (2 qubits)
    // perm=[0,0] is not a valid permutation of [0,1].
    const std::vector<QubitIndex> dup_perm = {0, 0};
    EXPECT_THROW(reorder_qubits(buf.re, buf.im, 4, 2, dup_perm),
                 SimulatorError);
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}

// ── Reorder06: 4-qubit round-trip with Bell-like state ────────────────────────
// Verifies that a more complex state (H on q0, CX q0->q1, H on q2, CX q2->q3)
// survives an arbitrary 4-element permutation followed by its inverse.

TEST(QubitReorder, Reorder06_FourQubit_RoundTrip_ComplexState) {
    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");

    auto b = std::make_unique<FullStateCpuBackend>();
    b->init_state(4);
    Circuit circ(4);
    circ.h(0).cx(0, 1).h(2).cx(2, 3);
    for (const auto& op : circ.ops()) {
        if (std::holds_alternative<qplayer::GateOp>(op)) {
            b->apply_gate(std::get<qplayer::GateOp>(op));
        }
    }

    RawStateView rv = b->get_statevector_raw();
    const StateIndex dim = rv.size;
    AlignedBuf work(dim);
    AlignedBuf orig(dim);
    work.copy_from(rv);
    orig.copy_from(rv);

    // Apply arbitrary permutation [3,1,0,2] then its inverse.
    const std::vector<QubitIndex> perm     = {3, 1, 0, 2};
    const std::vector<QubitIndex> inv_perm = inverse_permutation(perm);

    reorder_qubits(work.re, work.im, dim, 4, perm);
    reorder_qubits(work.re, work.im, dim, 4, inv_perm);

    // Assert: identical to original within 1e-15.
    for (StateIndex i = 0; i < dim; ++i) {
        EXPECT_NEAR(work.re[i], orig.re[i], 1e-15);
        EXPECT_NEAR(work.im[i], orig.im[i], 1e-15);
    }

    ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
}
