// cancel_inverse.hpp — CancelInversePass placeholder.
// Pairs adjacent inverse gates (H-H, X-X, CX-CX, etc.) and removes them.
// One of the Level-1 passes registered by transpile().

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class CancelInversePass : public IPass {
public:
    CancelInversePass() = default;

    std::string name() const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
