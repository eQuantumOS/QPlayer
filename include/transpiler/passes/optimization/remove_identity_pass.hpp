// remove_identity_pass.hpp — RemoveIdentityPass declaration.
// Removes identity (GateType::I) gates from the circuit. All other ops pass
// through unchanged. Boundary ops (Measure, Barrier, If, Idle) and Opaque
// GateOps are preserved as-is.
//
// Level 2/3 only — not registered in Level 0/1 PassPolicy.
//

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class RemoveIdentityPass : public IPass {
public:
    RemoveIdentityPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
