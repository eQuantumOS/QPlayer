// commutation_reorder_pass.hpp — CommutationReorderPass declaration.
// Reorders adjacent commuting gates so that downstream passes (CancelInversePass,
// MergeSingleQubitPass) can act on pairs that were separated by disjoint-qubit
// gates. Boundary ops (Measure, Barrier, If, Idle) are never crossed.
//
// Phase 4 scope: minimal real implementation — adjacent two-op swap on
// disjoint qubits only. Full DAG-based commutation analysis is deferred to
//

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class CommutationReorderPass : public IPass {
public:
    CommutationReorderPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
