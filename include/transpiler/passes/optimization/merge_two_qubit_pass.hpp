// merge_two_qubit_pass.hpp — MergeTwoQubitPass declaration.
// Merges consecutive same-kind, same-qubit-pair 2Q GateOps into an identity
// or a simpler gate. Self-inverse pairs (CX, CY, CZ, CH, SWAP, CCX, CSwap)
// cancel to identity. Rotation variants (CRZ, CU1) accumulate their angles.
//
// Qubit-pair order is significant: CX(0,1)·CX(0,1) cancels, but
// CX(0,1)·CX(1,0) does NOT (different gate on the circuit graph).
//
// Level 2/3 only — not registered in Level 0/1 PassPolicy.
// Boundary ops (Measure, Barrier, If, Idle) and 1Q GateOps between a 2Q gate
// and its candidate partner terminate the merge window for that qubit pair.
// Opaque GateOps pass through unchanged.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class MergeTwoQubitPass : public IPass {
public:
    MergeTwoQubitPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
