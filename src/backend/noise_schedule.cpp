/// @file noise_schedule.cpp
/// @brief Canonical op order for noise trajectory loops.

#include "detail/noise_schedule.hpp"

#include "circuit/circuit.hpp"
#include "circuit/gate_op.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace qplayer {
namespace detail {

namespace {

// Qubit indices touched by an op for scheduling purposes.
// GateOp returns its qubits; IdleOp returns {qubit}; others return {}.
std::vector<QubitIndex> sched_qubits(const Op& op) {
    if (const auto* g = std::get_if<GateOp>(&op)) return g->qubits;
    if (const auto* idle = std::get_if<IdleOp>(&op)) return {idle->qubit};
    return {};
}

// A barrier contributes no quantum work on the noisy path; skip it.
bool is_barrier_op(const Op& op) {
    return std::get_if<BarrierOp>(&op) != nullptr;
}

// Boundaries anchor their position: RNG draws (Reset/Measure) or classical
// state (IfOp/Opaque) prevent reordering across them.
bool is_boundary_op(const Op& op) {
    if (std::get_if<MeasureOp>(&op)) return true;
    if (std::get_if<IfOp>(&op))      return true;
    if (const auto* g = std::get_if<GateOp>(&op)) {
        return g->type == GateType::Reset ||
               g->type == GateType::Opaque ||
               g->is_opaque;
    }
    return false;
}

// Greedy scheduler for one boundary-delimited segment: `seg` holds the op
// indices in original order, `qubits[j]` the qubits of op j. Mirrors
// tn_sweep_order() with site = qubit number; `centre` carries
// over between segments exactly as it does there.
void schedule_segment(const std::vector<std::size_t>& seg,
                      const std::vector<std::vector<QubitIndex>>& qubits,
                      std::size_t& centre,
                      std::vector<std::size_t>& out) {
    if (seg.empty()) return;

    std::size_t nq = 0;
    for (std::size_t j : seg)
        for (QubitIndex q : qubits[j]) nq = std::max<std::size_t>(nq, std::size_t{q} + 1);

    // Per-qubit queues in original order: an op is ready when it heads the
    // queue of every qubit it touches, i.e. every earlier op on a shared qubit ran.
    std::vector<std::vector<std::size_t>> queue(nq);
    for (std::size_t j : seg)
        for (QubitIndex q : qubits[j]) queue[q].push_back(j);
    std::vector<std::size_t> head(nq, 0);
    const auto ready = [&](std::size_t j) {
        for (QubitIndex q : qubits[j])
            if (queue[q][head[q]] != j) return false;
        return true;
    };

    std::vector<std::size_t> cand;
    std::vector<char> listed(qubits.size(), 0);
    const auto offer = [&](std::size_t j) {
        if (!listed[j] && ready(j)) { listed[j] = 1; cand.push_back(j); }
    };
    for (std::size_t q = 0; q < nq; ++q)
        if (!queue[q].empty()) offer(queue[q][0]);

    while (!cand.empty()) {
        // One-qubit ops never move the centre, so a ready one runs first; among
        // multi-qubit ops the one nearest the centre wins, the lower original
        // index breaking ties so the order is deterministic.
        std::size_t slot = 0;
        std::size_t best = std::numeric_limits<std::size_t>::max();
        for (std::size_t c = 0; c < cand.size(); ++c) {
            const auto& qs = qubits[cand[c]];
            std::size_t dist = 0;
            if (qs.size() > 1) {
                std::size_t lo = qs[0], hi = qs[0];
                for (QubitIndex q : qs) { lo = std::min<std::size_t>(lo, q); hi = std::max<std::size_t>(hi, q); }
                dist = 1 + (centre < lo ? lo - centre : (centre > hi ? centre - hi : 0));
            }
            if (dist < best || (dist == best && cand[c] < cand[slot])) { best = dist; slot = c; }
        }
        const std::size_t pick = cand[slot];
        cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(slot));
        const auto& qs = qubits[pick];
        if (qs.size() > 1) {
            std::size_t hi = 0;
            for (QubitIndex q : qs) hi = std::max<std::size_t>(hi, q);
            centre = hi;
        }
        out.push_back(pick);
        for (QubitIndex q : qs) ++head[q];
        for (QubitIndex q : qs)
            if (head[q] < queue[q].size()) offer(queue[q][head[q]]);
    }
}

}  // namespace

std::vector<std::size_t> noise_trajectory_canonical_order(const Circuit& circuit) {
    const auto& ops = circuit.ops();
    std::vector<std::vector<QubitIndex>> qubits(ops.size());
    for (std::size_t i = 0; i < ops.size(); ++i) qubits[i] = sched_qubits(ops[i]);

    std::vector<std::size_t> result;
    result.reserve(ops.size());
    std::vector<std::size_t> seg;
    std::size_t centre = 0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        // Barriers change no state on the noisy paths and must not split a
        // segment: circuits with a barrier per layer would lose the reordering
        // entirely.
        if (is_barrier_op(ops[i])) continue;
        if (is_boundary_op(ops[i])) {
            schedule_segment(seg, qubits, centre, result);
            seg.clear();
            result.push_back(i);
            continue;
        }
        // Zero-qubit moveable ops have no ordering constraint; keep them in place
        // relative to the segment start.
        if (qubits[i].empty()) { schedule_segment(seg, qubits, centre, result); seg.clear(); result.push_back(i); continue; }
        seg.push_back(i);
    }
    schedule_segment(seg, qubits, centre, result);
    return result;
}

}  // namespace detail
}  // namespace qplayer
