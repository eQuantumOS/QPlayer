// test_runtime_fusion_backends.cpp — runtime Gate Fusion across backends.
//
// put a fused k-qubit GEMV kernel on the GPU, so now runs on
// both FullState paths. Fusion must be invisible in results: turning it on,
// changing its width, or letting the cost model decline a window all
// have to leave the state vector alone up to floating-point reassociation.
//
// TEST_BACKENDS runs each case on CPU, and on GPU too when the build has one
// — that is what covers the new CUDA kernel. The cross-backend case
// at the end is a plain TEST because it drives both procs in one body.

#include "../../framework/test_case.hpp"
#include "../../framework/assertions.hpp"
#include "../../framework/backend_params.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "../../../include/qregister.hpp"

using qplayer::Circuit;
using qplayer::ProcType;
using qplayer::QRegister;
using qplayer::QubitIndex;
using qplayer::SimType;
using qplayer::StateVector;

namespace {

// A circuit whose windows are mostly 1Q work, so the cost model fuses them.
Circuit fusible_circuit() {
    Circuit c(8);
    for (QubitIndex q = 0; q < 8; ++q) c.h(q);
    for (QubitIndex q = 0; q < 8; ++q) c.rz(0.31 * static_cast<double>(q + 1), q);
    for (QubitIndex q = 0; q + 1 < 8; ++q) c.cx(q, q + 1);
    for (QubitIndex q = 0; q < 8; ++q) c.ry(0.17 * static_cast<double>(q + 1), q);
    c.t(0).sx(3).s(5);
    return c;
}

// Controlled-gate heavy: windows here fail the cost check and are
// emitted as individual GateOps, exercising the pass-through branch.
Circuit control_heavy_circuit() {
    Circuit c(8);
    c.h(0).h(1).h(2);
    for (QubitIndex q = 0; q + 2 < 8; ++q) c.ccx(q, q + 1, q + 2);
    for (QubitIndex q = 0; q + 1 < 8; ++q) c.cx(q, q + 1);
    c.swap(0, 7).cswap(1, 2, 3);
    return c;
}

double max_abs_diff(const StateVector& a, const StateVector& b) {
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, std::abs(a[i] - b[i]));
    }
    return m;
}

StateVector run_and_read(ProcType proc, const Circuit& c,
                         bool fusion, std::uint32_t width) {
    QRegister qr(SimType::FullState, proc);
    qr.set_seed(7);
    qr.run(c, 1, fusion, width);
    return qr.get_statevector();
}

}  // namespace

// ── Fusion must not change the state ─────────────────────────────────────────

TEST_BACKENDS(RuntimeFusion, FusionOnOff_SameStatevector) {
    const Circuit c = fusible_circuit();

    const StateVector off = run_and_read(proc, c, false, 4);
    const StateVector on  = run_and_read(proc, c, true, 4);

    // Reassociation only: the two paths multiply the same unitaries in the same
    // order, just grouped differently.
    EXPECT_NEAR(max_abs_diff(off, on), 0.0, 1e-12);
}

TEST_BACKENDS(RuntimeFusion, ControlHeavy_FusionOnOff_SameStatevector) {
    const Circuit c = control_heavy_circuit();

    const StateVector off = run_and_read(proc, c, false, 4);
    const StateVector on  = run_and_read(proc, c, true, 4);

    EXPECT_NEAR(max_abs_diff(off, on), 0.0, 1e-12);
}

TEST_BACKENDS(RuntimeFusion, WidthSweep_AllWidthsAgree) {
    const Circuit     c   = fusible_circuit();
    const StateVector ref = run_and_read(proc, c, false, 4);

    // Widths 1..4 are valid on both backends (the GPU cap is 4).
    for (std::uint32_t w = 1; w <= 4; ++w) {
        EXPECT_NEAR(max_abs_diff(ref, run_and_read(proc, c, true, w)), 0.0, 1e-12);
    }
}

TEST_BACKENDS(RuntimeFusion, OverWidth_Clamped_NoCrashSameResult) {
    const Circuit     c   = fusible_circuit();
    const StateVector ref = run_and_read(proc, c, false, 4);

    // Both backends clamp instead of throwing, because enable_fusion and
    // fusion_max_width are run() defaults — a throw would break default calls.
    // The GPU clamps to 4 and the CPU to 8; either way the result is unchanged.
    for (std::uint32_t w : {std::uint32_t{9}, std::uint32_t{64}, ~std::uint32_t{0}}) {
        EXPECT_NEAR(max_abs_diff(ref, run_and_read(proc, c, true, w)), 0.0, 1e-12);
    }
}

// ── CPU vs GPU on the fused path ─────────────────────────────────────────────
// The GPU kernel rebuilds base indices and gathers amplitudes with its own
// arithmetic; this pins it to the CPU reference rather than only to itself.

TEST(RuntimeFusion, CpuGpuParity_FusedPath) {
    const auto procs = qplayer::test::testable_procs();
    bool has_gpu = false;
    for (ProcType p : procs) has_gpu = has_gpu || (p == ProcType::GPU);
    if (!has_gpu) return;   // CPU-only build: nothing to cross-check

    for (const Circuit& c : {fusible_circuit(), control_heavy_circuit()}) {
        const StateVector cpu = run_and_read(ProcType::CPU, c, true, 4);
        const StateVector gpu = run_and_read(ProcType::GPU, c, true, 4);
        EXPECT_NEAR(max_abs_diff(cpu, gpu), 0.0, 1e-12);
    }
}
