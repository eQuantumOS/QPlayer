/// @file gate_fusion.hpp
/// @brief Runtime gate fusion for the FullState backends.
/// @ingroup backend
///
/// Runtime Gate Fusion for FullState CPU backend.
/// Greedy-window algorithm fuses consecutive unitary GateOps into dense k-qubit
/// unitaries (k <= max_width). Boundary ops (Measure/Barrier/If/Idle) are never
/// crossed.
///
/// Public surface:
/// detail::FusedGate        — dense 2^k x 2^k unitary on sorted qubit set
/// detail::FusedOp          — variant holding FusedGate or a passthrough boundary op
/// detail::FusionCostModel  — per-backend cost profile gating window emission
/// detail::fuse_circuit()   — produce fused program from Op vector
/// detail::apply_fused_gate() — apply one FusedGate to SoA state vector

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <variant>
#include <vector>

#include "types.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {
namespace detail {

// Hard cap on FusedGate width. The GEMV kernels in gate_fusion.cpp hold the
// local amplitude block in fixed stack scratch sized (1 << kMaxFusionWidth), so
// a wider FusedGate would index past the end of those arrays — the invariant is
// enforced by fuse_circuit(), which clamps its max_width argument to this value
// ( sibling finding — audit #1: fusion_max_width >= 9 smashed the stack
// because nothing checked the user-supplied width).
//
// Note this caps the *window* width, not the arity of one gate: a single gate
// wider than max_width still forms a window of its own size (see fuse_circuit),
// so the effective bound is max(kMaxFusionWidth, widest fusible gate = 3 for
// CCX). Both stay well inside the scratch.
inline constexpr std::uint32_t kMaxFusionWidth = 8;

// An opaque gate is applied through apply_fused_gate(), which writes
// into stack scratch sized 1 << kMaxFusionWidth. fuse_circuit() enforces that
// bound by clamping max_width, but the opaque path bypasses fuse_circuit, so the
// registration cap must stay inside the kernel's reach. Binding the two caps
// here turns "remember to check the other file" into a build error.
static_assert(kMaxOpaqueQubits <= kMaxFusionWidth,
              "kMaxOpaqueQubits exceeds the fused-kernel stack scratch "
              "(1 << kMaxFusionWidth); raise kMaxFusionWidth first");

/// Dense 2^k by 2^k unitary over a sorted qubit set.
/// @ingroup backend
///
/// Dense unitary acting on `qubits` (sorted ascending). matrix is row-major
/// complex<double> with dimension (1<<k) x (1<<k), k = qubits.size() <= max_width.
/// Qubit bit convention: state index bit q = (idx >> q) & 1, matching full_state_cpu.cpp.
/// (runtime fusion)
struct FusedGate {
    std::vector<QubitIndex>            qubits;   // sorted ascending, size k
    std::vector<std::complex<double>>  matrix;   // size (1<<k)*(1<<k), row-major
};

/// One element of a fused program: a dense block, a boundary op, or a gate the
/// backend applies through its own kernel.
///
/// Either a dense FusedGate block, a passthrough boundary op
/// (Measure/Barrier/If/Idle), or a passthrough GateOp that the backend applies
/// via its dedicated kernel instead of a dense GEMV ( — permutation gates
/// Swap/CSwap are cheaper through apply_swap_kernel than an 8x8/16x16 fused GEMV).
using FusedOp = std::variant<FusedGate, GateOp, MeasureOp, BarrierOp, IfOp, IdleOp>;

/// Per-backend cost profile deciding whether a window is worth emitting as a fused gate.
/// @ingroup backend
///
/// Cost profile deciding whether an accumulated window is worth emitting as one
/// dense FusedGate instead of its original GateOps.
///
/// Costs are in "1Q full pass" units: 1.0 = one read-modify-write sweep over the
/// whole state vector. A dedicated kernel only touches the subspace its controls
/// select (apply_controlled_1q skips indices failing the control mask), so a gate
/// with c control bits costs less than a full pass — see gate_kernel_weight() for
/// the measured form. A fused GEMV always sweeps the whole state regardless of
/// width, which is why absorbing controlled gates can cost more than running them
/// separately (measured q26: cx 0.61x, ccx 0.55x — both losses).
///
/// window cost  C(k) = alpha + beta * 2^k
/// alpha         whole-state traffic term, 1.0 by definition
/// beta * 2^k    GEMV compute term: a width-k block does 2^k complex MACs per
/// amplitude against a 1Q kernel's 2, so time grows with 2^k
///
/// beta is host-dependent and much smaller on GPU (compute headroom over HBM
/// bandwidth is far larger there), hence one profile per backend.
///
/// C(k) alone is not enough: composing the window costs host time too, and that
/// cost scales with the gate COUNT while the fusion benefit scales with the STATE
/// SIZE, so the balance depends on num_qubits. Small states make the
/// host term dominant — measured on GPU at 15 qubits it eats 41-70% of a 1Q
/// gate's weight, which is what made 9-19 qubit circuits slower with fusion on.
/// The extra fields below model it; leaving them zero disables the term.
struct FusionCostModel {
    double alpha;   // whole-state traffic cost of one fused window
    double beta;    // per-2^k GEMV compute cost, relative to a 1Q pass

    // Absolute time of one dedicated 1Q kernel pass, used to convert the host
    // composition cost into pass units. Both backends bottom out
    // at a fixed floor for small states — kernel launch latency on GPU, OpenMP
    // parallel-region entry on CPU — and grow with 2^n above it.
    double pass_floor_us   = 0.0;   // fixed cost floor of one pass
    double pass_per_amp_us = 0.0;   // per-amplitude cost in the bandwidth regime

    // Host cost of left_apply_gate() per 2^(2k+arity) work unit (-1
    // gives the work term, the unit price). Host code, so the value
    // is backend-independent; it lives in the profile for locality.
    double compose_unit_us = 0.0;

    // C(k) — device cost of emitting one width-k fused window. k <=
    // kMaxFusionWidth, so the shift stays well inside 32 bits.
    double window_cost(std::uint32_t k) const {
        return alpha + beta * static_cast<double>(std::uint32_t{1} << k);
    }

    // t_pass(n) — see pass_floor_us. ldexp rather than a shift because
    // num_qubits may exceed the width of an integer shift on other backends.
    double pass_time_us(QubitIndex num_qubits) const {
        return std::max(pass_floor_us,
                        pass_per_amp_us * std::ldexp(1.0, static_cast<int>(num_qubits)));
    }

    // h(W, k, n) — host cost of composing a window, in 1Q-pass units.
    // arity_units is sum(2^arity(g)) over the window's gates, accumulated during
    // window growth because 2^(2k) is only known once the width is final.
    // A profile with no pass-time data returns 0: the term is not modelled.
    double compose_cost(std::uint32_t k, double arity_units,
                        QubitIndex num_qubits) const {
        const double t_pass = pass_time_us(num_qubits);
        if (t_pass <= 0.0) return 0.0;
        return compose_unit_us * std::ldexp(arity_units, static_cast<int>(2 * k)) / t_pass;
    }
};

// Host price of one 2^(2k+arity) composition work unit, measured on cx-family
// windows where the 2^(2k) scaling comes out clean (k=2..6 ratios 3.5/3.9/4.0/4.0,
// unit price converging to this value; k=6 predicted 34.0 us vs 33.58 measured).
// Backend-independent: it prices host code. The 1Q-gate family was too noisy to
// fit separately, so 1Q composition is overpriced by roughly 5x — conservative in
// the direction that matters, since it only suppresses fusion on small states
//
inline constexpr double kComposeUnitUs = 2.08e-3;

// FullState CPU profile. beta is back-solved from a q26 measurement: 24 `h`
// gates fused into width-4 windows ran 1.43x faster than unfused, so one window
// costs 2.8 1Q-passes, and C(4) = 1 + beta*16 = 2.8 gives beta = 0.1125.
// Cross-check (not fitted): this predicts C(5)=4.6 and C(6)=8.2 well above
// C(4)=2.8, matching 's independent q27 width sweep where widths 5 and 6
// were slower than 4.
//
// Pass times: a 1Q kernel pass costs 34.0 us up to ~19 qubits —
// OpenMP parallel-region entry, not memory traffic — and 7.10e-5 us per amplitude
// above that. The floor being 13x the GPU's is why the host composition term is
// negligible here and CPU never regressed: at 15 qubits it is 0.031 of a gate's
// weight against the GPU's 0.41.
inline constexpr FusionCostModel kCpuFusionCostModel{1.0, 0.1125,
                                                    34.0, 7.10e-5, kComposeUnitUs};

// FullState GPU profile. beta is far smaller than the CPU's: an
// H100 has ~34 TFLOP/s FP64 against ~3.9 TB/s of HBM, so a width-k window
// (arithmetic intensity 2^k/4 flop/byte) stays bandwidth-bound up to k=5 and the
// GEMV compute term barely registers. Windows are therefore worth taking more
// often on GPU than on CPU, which is the point — the per-gate kernels already
// run at ~78% of HBM peak, so cutting passes is the only lever left.
//
// Pass times: 2.6 us of kernel launch latency dominates up to ~18
// qubits, then 9.43e-6 us per amplitude. That low floor is what made the host
// composition term bite here — see kCpuFusionCostModel.
inline constexpr FusionCostModel kGpuFusionCostModel{1.0, 0.02,
                                                    2.6, 9.43e-6, kComposeUnitUs};

// Greedy-window fusion pass.  max_width caps the union of qubit indices that
// one FusedGate may span; values above kMaxFusionWidth are clamped to it with a
// log::warn (the kernels' stack scratch is sized for that cap).  Boundary ops
// (Measure/Barrier/If/Idle) and
// non-unitary GateOps (Reset/Opaque) flush the window immediately and are
// passed through as-is (never incorporated into a FusedGate).
//
// Reset or Opaque GateOps inside ops throw SimulatorError{"FullState", ...}
// because matrix-form fusion is undefined for non-unitary ops (+).
//
// Each accumulated window is emitted as a dense FusedGate only when it pays for
// itself under `cost`: sum of per-gate weights > C(k) + h(W, k, num_qubits)
// Otherwise the window's original GateOps are passed
// through in order, so the backend runs them on their dedicated kernels — which
// also skips the composition, recovering ~90% of the host cost (measured).
// The check runs at flush time, when the final width is known; it does not
// change which gates land in which window.
//
// num_qubits is the circuit's qubit count: the fusion benefit scales with the
// state size while the host composition cost scales with the gate count, so the
// verdict genuinely depends on it. Passing a profile with no
// pass-time data drops the host term and restores pure behaviour.
//
// The returned vector is consumed by apply_fused_program() in the backend.
std::vector<FusedOp> fuse_circuit(const std::vector<Op>& ops,
                                   std::uint32_t          max_width,
                                   QubitIndex             num_qubits,
                                   const FusionCostModel& cost = kCpuFusionCostModel);

// Cost weight of running `g` on its dedicated backend kernel, in 1Q-pass units
// Exposed for the cost-model unit tests.
double gate_kernel_weight(const GateOp& g);

// Apply a single FusedGate to a SoA state vector in-place.
// dim must equal 1 << num_qubits of the state.
// re/im must be 64-byte aligned (same contract as full_state_cpu.cpp).
// Parallelised with #pragma omp parallel for over the 2^(n-k) base indices.
// Dense GEMV is used for correctness; OpenBLAS zgemv upgrade is deferred to
// perf pass.
void apply_fused_gate(double* re, double* im, StateIndex dim,
                      const FusedGate& g);

// Dense 2^k x 2^k unitary of a GateOp in OPERAND order: local bit b selects
// op.qubits[b] (`cx q[2], q[0]` -> bit 0 is the control q[2]).
//
// Exported for the MPS backend, which needs the same matrices but a
// different application path. A second gate-matrix table would be free to drift
// from this one, and a divergence shows up as wrong amplitudes rather than as a
// build error — so both backends read from here.
//
// Throws SimulatorError for non-unitary types (Reset, Opaque); callers must
// dispatch those first.
std::vector<std::complex<double>> gate_full_matrix(const GateOp& op);

// Build a FusedGate from a dense k-qubit matrix given in OPERAND order, for
// callers that bypass fuse_circuit() — opaque gates.
//
// `qubits` is the operand list as written (`mygate q[2], q[0]` -> {2, 0}); the
// matrix's local bit b corresponds to qubits[b], matching the GateOp convention
// that gate_full_matrix() uses. apply_fused_gate() instead requires qubits
// sorted ascending, so the matrix axes must be permuted to agree — getting that
// backwards silently swaps operands (the ctrl/tgt trap documented at
// gate_fusion.cpp fuse_circuit()). This helper does both steps together.
//
// `matrix` is row-major of size (1<<k)*(1<<k) where k == qubits.size().
// Throws SimulatorError when k exceeds kMaxFusionWidth (apply_fused_gate's
// stack scratch is sized 1 << kMaxFusionWidth and does not check), when qubits
// repeat, or when the matrix size does not match k.
FusedGate make_dense_gate(const std::vector<QubitIndex>&           qubits,
                          const std::vector<std::complex<double>>& matrix);

}  // namespace detail
}  // namespace qplayer
