/// @file tn_measure.hpp
/// @brief Mid-circuit measurement collapse and Reset on an MPS chain.
/// @ingroup backend
///
/// Deferring measurement to the end of the circuit is exact only while nothing
/// follows it. Once a gate touches a measured qubit again the collapse is
/// observable — `h q; measure q; h q; measure q` gives an unbiased second bit
/// with collapse and a constant 0 without it — so a circuit that reuses a
/// measured qubit has to project the state where the measurement happens.
///
/// On an MPS that projection is cheap in the way that matters: a measurement is
/// the Kraus channel {|0><0|, |1><1|}, both operators act on ONE site, and a
/// one-site operator cannot create entanglement. So no SVD runs, no bond grows,
/// and nothing is added to the truncation ledger. The cost is the probability
/// read, which needs the rest of the chain summed out (sections A, B).

#pragma once

#include "backend/tensor_network/mps.hpp"

#include <random>

namespace qplayer {
namespace detail {

/// Born-rule collapse of one site: draws an outcome, projects, renormalises.
///
/// `site` is a SITE index, not a qubit index — the backend owns the qubit->site
/// permutation and hands site space in, as it does for every other chain
/// primitive. The returned value is the physical outcome (0 or 1); any readout
/// confusion is the caller's business, because flips the reported
/// classical bit and not the state.
///
/// The projector is applied as |b><b| / sqrt(p_b), so the chain keeps the norm
/// it had going in rather than being forced to 1 — truncation may already have
/// moved it off 1 and this routine has no business hiding that.
/// Exactly one uniform is consumed, so a fixed seed reproduces the collapse
///
/// throws: SimulatorError — site out of range, or both outcomes have zero
///   weight, which means the chain diverged rather than that the draw was unlucky.
int mps_measure_site(MpsState& state, std::size_t site, std::mt19937_64& rng);

/// Reset one site to |0>: collapse, then flip a |1> outcome back.
///
/// Same definition the statevector backends use, so "reset" means one thing
/// across the project. It is STOCHASTIC even though its result is not — the
/// collapse is a real measurement, and on an entangled site it decides what the
/// partners are left holding, which is why a reset circuit belongs on the
/// per-shot path rather than being evolved once and sampled.
///
/// `site` is a SITE index. Consumes exactly one uniform, like the measurement it
/// is built, and the X costs no bond dimension.
/// throws: SimulatorError — see mps_measure_site().
void mps_reset_site(MpsState& state, std::size_t site, std::mt19937_64& rng);

}  // namespace detail
}  // namespace qplayer
