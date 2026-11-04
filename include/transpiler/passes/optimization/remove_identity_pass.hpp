/// @file remove_identity_pass.hpp
/// @brief Removes identity gates from the circuit.
/// @ingroup transpiler
///
/// RemoveIdentityPass declaration.
/// Removes identity (GateType::I) gates from the circuit. All other ops pass
/// through unchanged. Boundary ops (Measure, Barrier, If, Idle) and Opaque
/// GateOps are preserved as-is.
///
/// Level 2/3 only — not registered in Level 0/1 PassPolicy.
///

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Drops GateType::I ops and passes every other op through unchanged.
/// @ingroup transpiler
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
