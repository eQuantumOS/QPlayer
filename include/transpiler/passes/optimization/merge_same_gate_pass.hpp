// merge_same_gate_pass.hpp — MergeSameGatePass declaration.
// Merges consecutive same-kind, same-qubit 1Q gates into an identity or a
// simpler gate. Self-inverse pairs (X·X, Y·Y, Z·Z, H·H) collapse to identity.
// Rotation gates of the same kind (RZ·RZ, U1·U1) accumulate their angles.
// Other same-kind pairs use a fixed phase table (S·S→Z, T·T→S, etc.).
//
// Level 2/3 only — not registered in Level 0/1 PassPolicy.
// Boundary ops (Measure, Barrier, If, Idle) and Opaque GateOps terminate the
// merge window for each qubit. Mixed-kind pairs are not merged by this pass.
//

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class MergeSameGatePass : public IPass {
public:
    MergeSameGatePass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
