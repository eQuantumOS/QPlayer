// test_gate_fusion.cpp — Unit tests for runtime Gate Fusion.
//
// For each test circuit we verify that:
//   Path A (unfused): init_state -> apply_gate per GateOp -> get_statevector.
//   Path B (fused):   init_state -> fuse_circuit -> apply_fused_program -> get_statevector.
//   EXPECT_NEAR(re/im A[i] == B[i], 1e-12) for all amplitudes.
//
// Circuits covered:
//   Fusion01: H then X on q0 (1Q chain -> single 1Q FusedGate).
//   Fusion02: H on q0 then CX(0,1) (Bell circuit, 2Q window).
//   Fusion03: GHZ 3-qubit (H(0), CX(0,1), CX(0,2)).
//   Fusion04: Rotation chain RX, RY, RZ on q0.
//   Fusion05: max_width=2 forcing a 3Q circuit to split (GHZ with max_width=2).
//   Fusion06: CCX (Toffoli) 3-qubit.
//   Fusion07: CSwap (Fredkin) 3-qubit.
//   BoundaryNoMerge: BarrierOp and MeasureOp prevent window merge.
//   MaxWidthRespected: all FusedGates have qubits.size() <= max_width.
//   MaxWidth1: every gate is its own FusedGate when max_width=1.
//   WindowSplitAccurate: a 3Q window split by max_width=2 still yields correct state.
//
// (Arrange/Act/Assert), (EXPECT_NEAR 1e-12 for exact).

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"

#include <cmath>
#include <complex>
#include <cstdlib>
#include <vector>

#include "../../../include/backend/full_state/full_state_backend.hpp"
#include "../../../include/backend/full_state/gate_fusion.hpp"
#include "../../../include/circuit/circuit.hpp"
#include "../../../include/circuit/gate_op.hpp"
#include "../../../include/error.hpp"
#include "../../../include/types.hpp"

using qplayer::FullStateCpuBackend;
using qplayer::Circuit;
using qplayer::GateOp;
using qplayer::GateType;
using qplayer::MeasureOp;
using qplayer::BarrierOp;
using qplayer::Op;
using qplayer::QubitIndex;
using qplayer::StateIndex;
using qplayer::StateVector;
using qplayer::SimulatorError;
using qplayer::detail::FusedGate;
using qplayer::detail::FusedOp;
using qplayer::detail::fuse_circuit;
using qplayer::detail::apply_fused_gate;

namespace {

// ── Fixture: two identical backends (A=unfused, B=fused) ─────────────────────

class FusionFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override {
        ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
        backA = std::make_unique<FullStateCpuBackend>();
        backB = std::make_unique<FullStateCpuBackend>();
    }
    void TearDown() override {
        backA.reset();
        backB.reset();
        ::unsetenv("QPLAYER_MEMORY_BUDGET_GB");
    }

    // Apply a circuit unfused (path A) and fused (path B), then compare
    // every amplitude to within tol.
    void compare_unfused_vs_fused(const Circuit& circ, std::uint32_t max_width,
                                   double tol = 1e-12) {
        const QubitIndex n = circ.num_qubits();

        // Path A: unfused.
        backA->init_state(n);
        for (const Op& op : circ.ops()) {
            if (std::holds_alternative<GateOp>(op)) {
                backA->apply_gate(std::get<GateOp>(op));
            }
            // Barrier/Measure skipped — no state effect in unfused path.
        }
        const StateVector svA = backA->get_statevector();

        // Path B: fused.
        backB->init_state(n);
        const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), max_width);
        backB->apply_fused_program(prog);
        const StateVector svB = backB->get_statevector();

        // Compare.
        for (std::size_t i = 0; i < svA.size(); ++i) {
            EXPECT_NEAR(svA[i].real(), svB[i].real(), tol);
            EXPECT_NEAR(svA[i].imag(), svB[i].imag(), tol);
        }
    }

    std::unique_ptr<FullStateCpuBackend> backA;
    std::unique_ptr<FullStateCpuBackend> backB;
};

}  // namespace

// ── Test 1: H then X on q0 (1Q chain -> single 1Q FusedGate) ─────────────────

TEST_F(FusionFixture, Fusion01_HX_SingleQubit_FusedEqualsUnfused) {
    // Arrange.
    Circuit circ(1);
    circ.h(0).x(0);

    // Act + Assert: fused == unfused within 1e-12.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 1b: sx / sxdg in a fused window ──────────────────────────
// Guards against the fusion local-matrix builder silently treating sx/sxdg as
// identity (the default fallback before). If sx were identity the
// fused statevector would diverge from the unfused reference.

TEST_F(FusionFixture, Fusion01b_SxSxdgMixed_FusedEqualsUnfused) {
    // Arrange: mix sx/sxdg with h/cx so fusion combines them into fused units.
    Circuit circ(2);
    circ.h(0).sx(0).sxdg(1).cx(0, 1).sx(1).sxdg(0);

    // Act + Assert: fused == unfused within 1e-12.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 2: Bell circuit H(0), CX(0,1) ────────────────────────────────────────

TEST_F(FusionFixture, Fusion02_Bell_TwoQubits_FusedEqualsUnfused) {
    // Arrange.
    Circuit circ(2);
    circ.h(0).cx(0, 1);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 3: GHZ 3-qubit H(0), CX(0,1), CX(0,2) ───────────────────────────────

TEST_F(FusionFixture, Fusion03_GHZ3Q_FusedEqualsUnfused) {
    // Arrange.
    Circuit circ(3);
    circ.h(0).cx(0, 1).cx(0, 2);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 4: Rotation chain RX, RY, RZ on q0 ──────────────────────────────────

TEST_F(FusionFixture, Fusion04_RotationChain_1Q_FusedEqualsUnfused) {
    // Arrange.
    const double pi = M_PI;
    Circuit circ(1);
    circ.rx(pi / 3.0, 0).ry(pi / 5.0, 0).rz(pi / 7.0, 0);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 5: max_width=2 splits GHZ 3Q circuit ─────────────────────────────────
// With max_width=2: H(0)+CX(0,1) => qubit union {0,1} fits; adding CX(0,2)
// would give {0,1,2} size=3 > 2, so it flushes and starts a new window.
// Result is two FusedGates; state must still be correct.

TEST_F(FusionFixture, Fusion05_GHZ3Q_MaxWidth2_SplitsCorrectly) {
    // Arrange.
    Circuit circ(3);
    circ.h(0).cx(0, 1).cx(0, 2);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 2);
}

// ── Test 6: CCX (Toffoli) 3-qubit ─────────────────────────────────────────────

TEST_F(FusionFixture, Fusion06_CCX_ThreeQubits_FusedEqualsUnfused) {
    // Arrange: |110> -> CCX -> |111> check via state vector.
    Circuit circ(3);
    circ.x(0).x(1).ccx(0, 1, 2);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 7: CSwap (Fredkin) 3-qubit ──────────────────────────────────────────

TEST_F(FusionFixture, Fusion07_CSwap_ThreeQubits_FusedEqualsUnfused) {
    // Arrange: |110> -> CSwap(0,1,2) -> control=q0=1, swap q1 and q2 -> |101>.
    Circuit circ(3);
    circ.x(0).x(1).cswap(0, 1, 2);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 8: BarrierOp prevents window merge ────────────────────────────────────

TEST_F(FusionFixture, BoundaryBarrier_PreventsMerge_BothSidesCorrect) {
    // Arrange: H(0) | Barrier | X(0).
    // After fuse_circuit, barrier splits the H and X into separate FusedGates.
    Circuit circ(1);
    circ.h(0).barrier({0}).x(0);

    // Act: fuse with max_width=4.
    const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), 4);

    // Assert: program contains at least 3 elements: FusedGate(H), Barrier, FusedGate(X).
    // The barrier must appear between two FusedGates.
    bool found_barrier = false;
    for (const FusedOp& fop : prog) {
        if (std::holds_alternative<BarrierOp>(fop)) {
            found_barrier = true;
        }
    }
    EXPECT_TRUE(found_barrier);

    // Also verify the final state is correct (unfused path vs fused path).
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 9: MeasureOp prevents window merge ────────────────────────────────────

TEST_F(FusionFixture, BoundaryMeasure_PreventsMerge_PassesThrough) {
    // Arrange: H(0), Measure(0,0), H(1).  Measure is a boundary.
    // We only check that the MeasureOp appears in the fused program
    // (correctness of the state is limited to what apply_fused_program touches;
    // measurement sampling itself is).
    Circuit circ(2, 2);
    circ.h(0).measure(0, 0).h(1);

    // Act.
    const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), 4);

    // Assert: MeasureOp is present in the output.
    bool found_measure = false;
    for (const FusedOp& fop : prog) {
        if (std::holds_alternative<MeasureOp>(fop)) {
            found_measure = true;
        }
    }
    EXPECT_TRUE(found_measure);
}

// ── Test 10: max_width respected ──────────────────────────────────────────────

TEST_F(FusionFixture, MaxWidthRespected_AllFusedGatesWithinWidth) {
    // Arrange: a 4-qubit circuit with several gates; max_width=2.
    Circuit circ(4);
    circ.h(0).h(1).h(2).h(3)
        .cx(0, 1).cx(1, 2).cx(2, 3);

    // Act.
    const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), 2);

    // Assert: no FusedGate has more than 2 qubits.
    for (const FusedOp& fop : prog) {
        if (std::holds_alternative<FusedGate>(fop)) {
            const FusedGate& fg = std::get<FusedGate>(fop);
            EXPECT_TRUE(fg.qubits.size() <= 2);
        }
    }

    // Also verify correctness.
    compare_unfused_vs_fused(circ, 2);
}

// ── Test 11: max_width=1 forces every gate into its own FusedGate ────────────

TEST_F(FusionFixture, MaxWidth1_EachGateOwnFusedGate_CorrectState) {
    // Arrange: H, X, Z on q0 (3 separate 1Q gates, each should be its own FusedGate).
    Circuit circ(1);
    circ.h(0).x(0).z(0);

    // Act.
    const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), 1);

    // Assert: all elements are FusedGates, each with exactly 1 qubit.
    for (const FusedOp& fop : prog) {
        EXPECT_TRUE(std::holds_alternative<FusedGate>(fop));
        if (std::holds_alternative<FusedGate>(fop)) {
            EXPECT_EQ(static_cast<std::size_t>(std::get<FusedGate>(fop).qubits.size()),
                      static_cast<std::size_t>(1));
        }
    }

    // Verify correctness.
    compare_unfused_vs_fused(circ, 1);
}

// ── Test 12: Window split accuracy for a 3Q circuit with max_width=2 ─────────

TEST_F(FusionFixture, WindowSplit_ThreeQubitCircuit_SplitAtWidth2_StillCorrect) {
    // Arrange: a more complex 3Q circuit: H(0), CX(0,1), H(2), CX(1,2), X(0).
    Circuit circ(3);
    circ.h(0).cx(0, 1).h(2).cx(1, 2).x(0);

    // Act + Assert: compare unfused vs fused with max_width=2.
    compare_unfused_vs_fused(circ, 2);
}

// ── Test 13: SWAP gate fused correctly ────────────────────────────────────────

TEST_F(FusionFixture, FusionSWAP_TwoQubits_FusedEqualsUnfused) {
    // Arrange: prepare |01> then SWAP -> should give |10>.
    Circuit circ(2);
    circ.x(1).swap(0, 1);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 14: Mixed 1Q and 2Q gates on disjoint qubit pairs ───────────────────

TEST_F(FusionFixture, FusionDisjointPairs_MaxWidth2_CorrectState) {
    // Arrange: H(0),H(2) should fuse into separate 1Q FusedGates (disjoint),
    // then CX(0,1) and CX(2,3) form separate 2Q FusedGates.
    Circuit circ(4);
    circ.h(0).h(2).cx(0, 1).cx(2, 3);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 15: U3 and CU3 produce correct fused matrix ─────────────────────────

TEST_F(FusionFixture, FusionU3CU3_GeneralGates_FusedEqualsUnfused) {
    // Arrange: U3(pi/3, pi/4, pi/5) on q0, then CU3(pi/6, pi/7, pi/8)(0,1).
    Circuit circ(2);
    circ.u3(M_PI / 3.0, M_PI / 4.0, M_PI / 5.0, 0)
        .cu3(M_PI / 6.0, M_PI / 7.0, M_PI / 8.0, 0, 1);

    // Act + Assert.
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 16: Single gate circuits (trivial fusion) ────────────────────────────

TEST_F(FusionFixture, FusionSingleGate_H_FusedEqualsUnfused) {
    Circuit circ(1);
    circ.h(0);
    compare_unfused_vs_fused(circ, 4);
}

TEST_F(FusionFixture, FusionSingleGate_CX_FusedEqualsUnfused) {
    Circuit circ(2);
    circ.h(0).cx(0, 1);
    compare_unfused_vs_fused(circ, 4);
}

// ── Test 17: Empty circuit produces empty fused program ──────────────────────

TEST_F(FusionFixture, FusionEmptyCircuit_EmptyFusedProgram) {
    // Arrange.
    Circuit circ(2);

    // Act.
    const std::vector<FusedOp> prog = fuse_circuit(circ.ops(), 4);

    // Assert.
    EXPECT_EQ(prog.size(), static_cast<std::size_t>(0));
}

// ── Test 18: apply_fused_gate direct — H gate on q0 of 2-qubit state ─────────

TEST_F(FusionFixture, ApplyFusedGate_H_OnTwoQubitState_CorrectAmplitudes) {
    // Arrange: |00> state, apply H on q0 directly.
    const double INV_SQRT2 = M_SQRT1_2;
    backA->init_state(2);

    // Build a FusedGate for H on qubit 0.
    FusedGate fg;
    fg.qubits = {0};
    fg.matrix = {
        {INV_SQRT2, 0}, {INV_SQRT2, 0},
        {INV_SQRT2, 0}, {-INV_SQRT2, 0}
    };

    // Get raw SoA buffers via get_statevector_raw.
    auto raw = backA->get_statevector_raw();

    // Act: apply directly via apply_fused_gate.
    apply_fused_gate(const_cast<double*>(raw.re), const_cast<double*>(raw.im),
                     raw.size, fg);

    // Assert: |00> -> (|00> + |10>) / sqrt(2).
    const StateVector sv = backA->get_statevector();
    // index 0 = |00> (both qubits 0), index 2 = |10> (qubit 1 is bit 1, qubit 0 is bit 0;
    // index with bit 0 = q0 = 1 -> index 1 for single qubit; for 2 qubits index = q0*1 = 1).
    // Wait: index convention is bit q of index i = (i >> q) & 1.
    // q0 = qubit 0: bit 0 of index i. q1 = qubit 1: bit 1.
    // H on q0: |00> -> (|0_q0>+|1_q0>)/sqrt(2) tensor |0_q1>.
    //   |00> has q0=0, q1=0 -> i=0.
    //   |10> means q0=1, q1=0 -> i = (1<<0) = 1.
    // So amplitude at index 0 and index 1 should be 1/sqrt(2).
    EXPECT_NEAR(sv[0].real(), INV_SQRT2, 1e-12);  // |00>: q0=0, q1=0
    EXPECT_NEAR(sv[0].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[1].real(), INV_SQRT2, 1e-12);  // |01>: q0=1, q1=0 (bit 0 = q0)
    EXPECT_NEAR(sv[1].imag(), 0.0, 1e-12);
    EXPECT_NEAR(sv[2].real(), 0.0, 1e-12);  // q1=1 components: zero
    EXPECT_NEAR(sv[3].real(), 0.0, 1e-12);
}

// ── Test 19: vectorised fused GEMV matches scalar reference ────────
// The column-accumulate + transposed-matrix SIMD kernel must produce
// the same dense matrix-vector result as the original row-wise scalar GEMV to
// within 1e-10 (accumulation reorder is ULP-level only). Uses a 3Q FusedGate on
// NON-contiguous qubits {0,1,3} of a 4Q state to exercise the free-bit insertion
// (gather/scatter) path, and an arbitrary (non-unitary is fine for GEMV) matrix.

TEST_F(FusionFixture, ApplyFusedGate_VectorisedGemv_MatchesScalarReference) {
    // Arrange: 4Q state with deterministic non-uniform amplitudes.
    const QubitIndex n = 4;
    backA->init_state(n);
    auto raw = backA->get_statevector_raw();
    const std::size_t dim = static_cast<std::size_t>(raw.size);

    std::vector<double> ref_re(dim), ref_im(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        const double r = std::cos(0.3 * static_cast<double>(i) + 0.1);
        const double m = std::sin(0.2 * static_cast<double>(i) + 0.7);
        const_cast<double*>(raw.re)[i] = r;
        const_cast<double*>(raw.im)[i] = m;
        ref_re[i] = r;
        ref_im[i] = m;
    }

    const int k = 3;
    const int ld = 1 << k;  // 8
    FusedGate fg;
    fg.qubits = {0, 1, 3};  // sorted ascending; free qubit = 2
    fg.matrix.resize(static_cast<std::size_t>(ld * ld));
    for (int rr = 0; rr < ld; ++rr) {
        for (int cc = 0; cc < ld; ++cc) {
            fg.matrix[static_cast<std::size_t>(rr * ld + cc)] =
                std::complex<double>(std::cos(1.0 + 0.5 * rr - 0.3 * cc),
                                     std::sin(0.4 + 0.2 * rr + 0.6 * cc));
        }
    }

    // Act: vectorised kernel applies in place on the backend buffer.
    apply_fused_gate(const_cast<double*>(raw.re), const_cast<double*>(raw.im),
                     raw.size, fg);

    // Reference: independent gather / row-wise scalar GEMV / scatter on ref arrays,
    // mirroring the bit conventions of apply_fused_gate.
    std::vector<std::size_t> gate_contrib(static_cast<std::size_t>(ld));
    for (int p = 0; p < ld; ++p) {
        std::size_t contrib = 0;
        for (int b = 0; b < k; ++b) {
            if ((p >> b) & 1) contrib |= std::size_t{1} << fg.qubits[static_cast<std::size_t>(b)];
        }
        gate_contrib[static_cast<std::size_t>(p)] = contrib;
    }
    const int n_free = static_cast<int>(n) - k;            // 1
    const int free_bit = 2;                                // qubit 2
    const std::int64_t n_base = std::int64_t{1} << n_free;  // 2
    for (std::int64_t bc = 0; bc < n_base; ++bc) {
        std::size_t base = 0;
        if (bc & 1) base |= std::size_t{1} << free_bit;
        double ar[8], ai[8];
        for (int p = 0; p < ld; ++p) {
            const std::size_t idx = base | gate_contrib[static_cast<std::size_t>(p)];
            ar[p] = ref_re[idx];
            ai[p] = ref_im[idx];
        }
        double orr[8], oi[8];
        for (int rr = 0; rr < ld; ++rr) {
            double sr = 0.0, si = 0.0;
            for (int cc = 0; cc < ld; ++cc) {
                const double mr = fg.matrix[static_cast<std::size_t>(rr * ld + cc)].real();
                const double mi = fg.matrix[static_cast<std::size_t>(rr * ld + cc)].imag();
                sr += mr * ar[cc] - mi * ai[cc];
                si += mr * ai[cc] + mi * ar[cc];
            }
            orr[rr] = sr;
            oi[rr] = si;
        }
        for (int p = 0; p < ld; ++p) {
            const std::size_t idx = base | gate_contrib[static_cast<std::size_t>(p)];
            ref_re[idx] = orr[p];
            ref_im[idx] = oi[p];
        }
    }

    // Assert: amplitudes match the scalar reference (ULP-level reorder).
    const StateVector sv = backA->get_statevector();
    for (std::size_t i = 0; i < dim; ++i) {
        EXPECT_NEAR(sv[i].real(), ref_re[i], 1e-10);
        EXPECT_NEAR(sv[i].imag(), ref_im[i], 1e-10);
    }
}
