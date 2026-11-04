/// @file cancel_inverse.hpp
/// @brief Cancels adjacent inverse gate pairs.
/// @ingroup transpiler
///
/// Pairs adjacent inverse gates (H-H, X-X, CX-CX, etc.) and removes them.
/// One of the Level-1 optimization passes registered by transpile().

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Removes adjacent self-inverse pairs such as H-H, X-X and CX-CX; one of the Level-1 passes.
/// @ingroup transpiler
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
