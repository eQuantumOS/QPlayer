/// @file tn_fusion.cpp
/// @brief TN gate fusion pass.
///
/// Scans the op stream once, keeping for each qubit the index of the step that
/// last wrote it. A new gate can be folded into that step only when the step
/// covers exactly the same operands — otherwise something in between touched a
/// shared qubit and the two gates do not commute.

#include "backend/tensor_network/tn_fusion.hpp"

#include "backend/full_state/gate_fusion.hpp"
#include "error.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace qplayer {
namespace detail {

namespace {

using Cx = std::complex<double>;

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
constexpr const char* kBackend = "TensorNetwork";

// out = lhs * rhs for dense n x n matrices in row-major order. `lhs` is the
// later gate: quantum circuits apply left to right, matrices compose right to left.
std::vector<Cx> matmul(const std::vector<Cx>& lhs, const std::vector<Cx>& rhs, std::size_t n) {
    std::vector<Cx> out(n * n, Cx{0, 0});
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < n; ++k) {
            const Cx a = lhs[i * n + k];
            if (a == Cx{0, 0}) continue;
            for (std::size_t j = 0; j < n; ++j) out[i * n + j] += a * rhs[k * n + j];
        }
    }
    return out;
}

// Rewrites a 4x4 given in operand order (q0, q1) into operand order (q1, q0) by
// swapping the two local bits of every row and column index. Needed so that
// `cx a,b` followed by `cz b,a` still fuses — the pair is the same, only the
// operand order differs.
std::vector<Cx> swap_operand_order(const std::vector<Cx>& m) {
    const auto flip = [](std::size_t i) { return ((i & 1u) << 1) | ((i >> 1) & 1u); };
    std::vector<Cx> out(16, Cx{0, 0});
    for (std::size_t r = 0; r < 4; ++r) {
        for (std::size_t c = 0; c < 4; ++c) out[flip(r) * 4 + flip(c)] = m[r * 4 + c];
    }
    return out;
}

}  // namespace

std::vector<TnStep> tn_fuse(const std::vector<Op>& ops) {
    std::vector<TnStep> steps;
    // Per qubit: index of the step that last wrote it, or kNone. This is what
    // makes "consecutive" mean "nothing touched these qubits in between".
    std::vector<std::size_t> last_step;

    const auto ensure = [&last_step](QubitIndex q) {
        if (q >= last_step.size()) last_step.resize(q + 1, kNone);
    };
    const auto break_run = [&last_step](QubitIndex q) {
        if (q < last_step.size()) last_step[q] = kNone;
    };

    for (const Op& op : ops) {
        if (const auto* gate = std::get_if<GateOp>(&op)) {
            if (gate->is_opaque || gate->type == GateType::Opaque) {
                throw SimulatorError{kBackend, "opaque gates are not supported on TensorNetwork yet"};
            }
            if (gate->qubits.size() > 3) {
                throw SimulatorError{kBackend,
                    "gates on more than 3 qubits are not supported on TensorNetwork yet"};
            }
            if (gate->qubits.empty()) continue;

            if (gate->type == GateType::Reset) {
                // Non-unitary: measure, then X on a |1> outcome. It has no matrix,
                // so it never reaches gate_full_matrix() -- which is what used to
                // report a [FullState] error from a TensorNetwork run.
                // A fusion boundary for the same reason MeasureOp is: fusing a gate
                // across it would move that gate to the wrong side of a collapse.
                const QubitIndex q = gate->qubits[0];
                ensure(q);
                steps.push_back(TnStep{TnStep::Kind::Reset, {q}, {}, {}});
                break_run(q);
                continue;
            }

            if (gate->qubits.size() == 3) {
                // Neither fusion rule covers a 3Q gate (same-qubit 1Q run /
                // same-pair 2Q run), so it gets its own step and ends the run on
                // all three operands -- the same treatment Reset and IfOp get.
                // Placed BEFORE gate_full_matrix() on purpose: apply_3q() picks
                // its fixed decomposition from `gate`, so building the 8x8 here
                // would be work thrown away.
                for (QubitIndex q : gate->qubits) ensure(q);
                TnStep step;
                step.kind   = TnStep::Kind::Gate;
                step.qubits = gate->qubits;
                step.gate   = gate->type;
                steps.push_back(std::move(step));
                for (QubitIndex q : gate->qubits) break_run(q);
                continue;
            }

            std::vector<Cx> matrix = gate_full_matrix(*gate);
            for (QubitIndex q : gate->qubits) ensure(q);

            if (gate->qubits.size() == 1) {
                const QubitIndex q = gate->qubits[0];
                const std::size_t prev = last_step[q];
                if (prev != kNone && steps[prev].kind == TnStep::Kind::Gate &&
                    steps[prev].qubits.size() == 1) {
                    steps[prev].matrix = matmul(matrix, steps[prev].matrix, 2);
                    continue;
                }
                steps.push_back(TnStep{TnStep::Kind::Gate, {q}, std::move(matrix), {}});
                last_step[q] = steps.size() - 1;
                continue;
            }

            const QubitIndex a = gate->qubits[0];
            const QubitIndex b = gate->qubits[1];
            const std::size_t prev = last_step[a];
            // Both operands must come from the SAME previous step: if they differ,
            // some other gate sits between the two on one of the wires.
            if (prev != kNone && prev == last_step[b] && steps[prev].kind == TnStep::Kind::Gate &&
                steps[prev].qubits.size() == 2) {
                // Same pair, possibly written the other way round: rewrite the
                // newcomer into the existing step's operand order before folding.
                if (steps[prev].qubits[0] != a) matrix = swap_operand_order(matrix);
                steps[prev].matrix = matmul(matrix, steps[prev].matrix, 4);
                continue;
            }
            steps.push_back(TnStep{TnStep::Kind::Gate, {a, b}, std::move(matrix), {}});
            last_step[a] = steps.size() - 1;
            last_step[b] = steps.size() - 1;
            continue;
        }

        if (const auto* meas = std::get_if<MeasureOp>(&op)) {
            ensure(meas->qubit);
            steps.push_back(TnStep{TnStep::Kind::Measure, {}, {}, *meas});
            break_run(meas->qubit);
            continue;
        }

        if (const auto* barrier = std::get_if<BarrierOp>(&op)) {
            // A barrier emits nothing; its whole purpose is to end runs. An empty
            // qubit list means "all" (gate_op.hpp).
            if (barrier->qubits.empty()) {
                std::fill(last_step.begin(), last_step.end(), kNone);
            } else {
                for (QubitIndex q : barrier->qubits) break_run(q);
            }
            continue;
        }

        if (const auto* cond = std::get_if<IfOp>(&op)) {
            // A conditional cannot be folded into anything: whether its body runs
            // is decided per shot from that shot's classical bits, so it is a
            // fusion boundary on the body's operands.
            //
            // The body's matrix is resolved HERE, once, rather than per shot --
            // and resolving it here is also what rejects an opaque body before
            // any amplitude moves, the way the unconditional path does.
            const GateOp& body = cond->body;
            if (body.qubits.empty()) continue;
            if (body.is_opaque || body.type == GateType::Opaque) {
                throw SimulatorError{kBackend,
                    "opaque gates are not supported on TensorNetwork yet"};
            }
            // A 3Q body stays rejected: unlike an unconditional 3Q gate it would
            // have to run its decomposition under a per-shot guard, and the
            // step carries ONE resolved matrix rather than a sequence.
            // Deliberate scope limit, not an oversight.
            if (body.qubits.size() > 2) {
                throw SimulatorError{kBackend,
                    "conditional gates on more than 2 qubits are not supported on "
                    "TensorNetwork yet"};
            }

            TnStep step;
            step.kind       = TnStep::Kind::If;
            step.qubits     = body.qubits;
            step.creg       = cond->creg;
            step.cond_value = cond->value;
            if (body.type == GateType::Reset) {
                step.body_kind = TnStep::Kind::Reset;
            } else {
                step.body_kind = TnStep::Kind::Gate;
                step.matrix    = gate_full_matrix(body);
            }
            steps.push_back(std::move(step));
            for (QubitIndex q : body.qubits) { ensure(q); break_run(q); }
            continue;
        }

        if (const auto* idle = std::get_if<IdleOp>(&op)) {
            // IdleOp dispatch slot. Without a noise model it changes
            // nothing, so no step is emitted — but it IS a fusion boundary
            // and fusing across it would later reorder
            // gates around the relaxation channel Phase 10 applies here.
            break_run(idle->qubit);
            continue;
        }
    }

    return steps;
}

std::vector<TnStep> tn_sweep_order(const std::vector<TnStep>& steps,
                                   const std::vector<QubitIndex>& site_of_qubit) {
    std::vector<TnStep> out;
    out.reserve(steps.size());

    // Segments between non-gate steps are scheduled independently: a measure,
    // reset or conditional keeps its exact position.
    std::size_t begin = 0;
    std::size_t centre = 0;
    while (begin < steps.size()) {
        if (steps[begin].kind != TnStep::Kind::Gate) {
            out.push_back(steps[begin++]);
            continue;
        }
        std::size_t end = begin;
        while (end < steps.size() && steps[end].kind == TnStep::Kind::Gate) ++end;

        // Per-qubit queues of the segment's steps in original order: a step is
        // ready when it heads the queue of every qubit it touches, which is
        // exactly "every earlier step on a shared qubit has run".
        std::vector<std::vector<std::size_t>> queue(site_of_qubit.size());
        for (std::size_t i = begin; i < end; ++i)
            for (QubitIndex q : steps[i].qubits) queue[q].push_back(i);
        std::vector<std::size_t> head(site_of_qubit.size(), 0);
        const auto ready = [&](std::size_t i) {
            for (QubitIndex q : steps[i].qubits)
                if (queue[q][head[q]] != i) return false;
            return true;
        };
        // Only heads can become ready, so the candidate list stays at most one
        // step per qubit and a segment costs O(steps * qubits), not O(steps^2).
        std::vector<std::size_t> cand;
        std::vector<bool> listed(end - begin, false);
        const auto offer = [&](std::size_t i) {
            if (!listed[i - begin] && ready(i)) { listed[i - begin] = true; cand.push_back(i); }
        };
        for (std::size_t q = 0; q < queue.size(); ++q)
            if (!queue[q].empty()) offer(queue[q][0]);
        while (!cand.empty()) {
            // One-qubit steps never move the centre, so a ready one runs first;
            // among multi-qubit steps the one nearest the centre wins, the lower
            // original index breaking ties so the order is deterministic.
            std::size_t slot = cand.size();
            std::size_t best = std::numeric_limits<std::size_t>::max();
            for (std::size_t c = 0; c < cand.size(); ++c) {
                const TnStep& st = steps[cand[c]];
                std::size_t dist = 0;
                if (st.qubits.size() > 1) {
                    std::size_t lo = site_of_qubit[st.qubits[0]], hi = lo;
                    for (QubitIndex q : st.qubits) {
                        lo = std::min<std::size_t>(lo, site_of_qubit[q]);
                        hi = std::max<std::size_t>(hi, site_of_qubit[q]);
                    }
                    dist = 1 + (centre < lo ? lo - centre : (centre > hi ? centre - hi : 0));
                }
                if (dist < best || (dist == best && cand[c] < cand[slot])) { best = dist; slot = c; }
            }
            const std::size_t pick = cand[slot];
            cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(slot));
            const TnStep& chosen = steps[pick];
            if (chosen.qubits.size() > 1) {
                std::size_t hi = 0;
                for (QubitIndex q : chosen.qubits) hi = std::max<std::size_t>(hi, site_of_qubit[q]);
                centre = hi;   // a split, walked or not, leaves the centre on its right site
            }
            out.push_back(chosen);
            for (QubitIndex q : chosen.qubits) ++head[q];
            for (QubitIndex q : chosen.qubits)
                if (head[q] < queue[q].size()) offer(queue[q][head[q]]);
        }
        begin = end;
    }
    return out;
}

}  // namespace detail
}  // namespace qplayer
