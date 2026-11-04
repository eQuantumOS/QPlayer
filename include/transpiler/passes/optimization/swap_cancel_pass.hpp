/// @file swap_cancel_pass.hpp
/// @brief Cancels redundant SWAP pairs left by routing.
/// @ingroup transpiler
///
/// SwapCancelPass (optimization stage).
/// Runs in the ⑤ re-optimization stage after SwapRouterPass, while
/// SWAPs are still atomic GateOp{SWAP} (un-decomposed). Removes redundant SWAP
/// pairs that routing introduces: two SWAPs on the same physical pair {a,b}
/// separated only by ops that touch neither a nor b cancel (SWAP·SWAP = I and
/// commutes past disjoint ops). The leftover single-qubit re-optimization
/// (re-applying MergeSingleQubit) is the pipeline's job — transpile() re-runs
/// the Level-1 optimization passes after this one (wired).
///
/// Immediately-adjacent SWAP·SWAP is already handled by CancelInversePass
/// (SWAP is self-inverse there); this pass adds the commutation-aware case that
/// CancelInversePass's strict-adjacency window cannot reach. Cancelling SWAP·SWAP
/// leaves the cumulative permutation unchanged, so the final CircuitMetadata.layout
/// recorded by SwapRouterPass stays correct and is passed through untouched.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Removes two SWAPs on the same physical pair separated only by ops touching neither qubit, reaching the commutation cases strict adjacency cannot.
/// @ingroup transpiler
class SwapCancelPass : public IPass {
public:
    SwapCancelPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
