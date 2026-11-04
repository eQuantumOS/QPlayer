/// @file noise_schedule.hpp
/// @brief Canonical op-execution order for noise trajectory loops.
/// @ingroup core
///
/// All three noisy backends (FullState CPU, FullState GPU, TensorNetwork) share
/// one order so per-trajectory RNG streams are consumed identically, preserving
/// bit-for-bit count identity across backends.

#pragma once

#include <cstddef>
#include <vector>

namespace qplayer {

class Circuit;  // forward declaration to avoid pulling in the full header

namespace detail {

/// Computes the canonical execution order for a noise trajectory loop.
///
/// Returns a permutation of op indices from circuit.ops() — a subset
/// (BarrierOp entries are excluded) — such that:
///   - MeasureOp, IfOp, GateType::Reset, GateType::Opaque stay in their
///     original positions relative to one another (fusion boundaries).
///   - Remaining GateOp and IdleOp entries are rescheduled within each
///     boundary-delimited segment by the same greedy rule as tn_sweep_order():
///     per-qubit order is preserved, 1-qubit ops go first, multi-qubit ops
///     are ordered by distance to the running centre (qubit index identity),
/// ties broken by the original index.
///   - Gate-attached noise channels move with their gate automatically
///     because each backend applies them immediately after the gate.
///
/// Computed once before a parallel trajectory region; all trajectories share
/// the result read-only.
std::vector<std::size_t> noise_trajectory_canonical_order(const Circuit& circuit);

}  // namespace detail
}  // namespace qplayer
