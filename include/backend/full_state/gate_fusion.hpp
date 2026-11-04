// gate_fusion.hpp — Runtime Gate Fusion for FullState CPU backend.
// Greedy-window algorithm fuses consecutive unitary GateOps into dense k-qubit
// unitaries (k <= max_width). Boundary ops (Measure/Barrier/If/Idle) are never
// crossed.
//
// Public surface:
//   detail::FusedGate      — dense 2^k x 2^k unitary on sorted qubit set
//   detail::FusedOp        — variant holding FusedGate or a passthrough boundary op
//   detail::fuse_circuit() — produce fused program from Op vector
//   detail::apply_fused_gate() — apply one FusedGate to SoA state vector

#pragma once

#include <complex>
#include <cstdint>
#include <variant>
#include <vector>

#include "types.hpp"
#include "circuit/gate_op.hpp"

namespace qplayer {
namespace detail {

// Dense unitary acting on `qubits` (sorted ascending). matrix is row-major
// complex<double> with dimension (1<<k) x (1<<k), k = qubits.size() <= max_width.
// Qubit bit convention: state index bit q = (idx >> q) & 1, matching full_state_cpu.cpp.
// (runtime fusion)
struct FusedGate {
    std::vector<QubitIndex>            qubits;   // sorted ascending, size k
    std::vector<std::complex<double>>  matrix;   // size (1<<k)*(1<<k), row-major
};

// Fused program element: either a dense FusedGate block, a passthrough boundary
// op (Measure/Barrier/If/Idle), or a passthrough GateOp that the backend applies
// via its dedicated kernel instead of a dense GEMV ( — permutation gates
// Swap/CSwap are cheaper through apply_swap_kernel than an 8x8/16x16 fused GEMV).
using FusedOp = std::variant<FusedGate, GateOp, MeasureOp, BarrierOp, IfOp, IdleOp>;

// Greedy-window fusion pass.  max_width caps the union of qubit indices that
// one FusedGate may span.  Boundary ops (Measure/Barrier/If/Idle) and
// non-unitary GateOps (Reset/Opaque) flush the window immediately and are
// passed through as-is (never incorporated into a FusedGate).
//
// Reset or Opaque GateOps inside ops throw SimulatorError{"FullState", ...}
// because matrix-form fusion is undefined for non-unitary ops (+).
//
// The returned vector is consumed by apply_fused_program() in the backend.
std::vector<FusedOp> fuse_circuit(const std::vector<Op>& ops,
                                   std::uint32_t          max_width);

// Apply a single FusedGate to a SoA state vector in-place.
// dim must equal 1 << num_qubits of the state.
// re/im must be 64-byte aligned (same contract as full_state_cpu.cpp).
// Parallelised with #pragma omp parallel for over the 2^(n-k) base indices.
// Dense GEMV is used for correctness; OpenBLAS zgemv upgrade is deferred to
// perf pass.
void apply_fused_gate(double* re, double* im, StateIndex dim,
                      const FusedGate& g);

}  // namespace detail
}  // namespace qplayer
