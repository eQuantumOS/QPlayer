/// @file merge_single_qubit_unitary_pass.hpp
/// @brief Composes arbitrary 1Q gate runs into a single U3 via matrix multiplication.
/// @ingroup transpiler
///
/// Full-unitary 1Q merge pass declaration.
/// Composes consecutive same-qubit 1-qubit gates into a single U3(theta,phi,lambda)
/// using 2x2 complex matrix multiplication followed by ZYZ / standard U3
/// decomposition.  Unlike MergeSingleQubitPass (pattern table, Clifford-only),
/// this pass handles arbitrary rotation-gate sequences and mixed Clifford+rotation
/// runs.  Global phase is discarded (safe for counts / expectation tests).
///
/// Boundary conditions: multi-qubit gates, Measure, Barrier, If, Idle, Opaque,
/// Reset — identical to MergeSingleQubitPass.
///
/// Only runs of length >= 2 are replaced.  A run of 1 is left unchanged.
/// The pass is registered at optimization_level >= 2, after MergeSingleQubitPass,
/// so Clifford patterns are already collapsed by the earlier pass.
/// extension,.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Handles mixed Clifford and rotation runs by 2x2 composition plus ZYZ decomposition, discarding global phase.
/// @ingroup transpiler
class MergeSingleQubitUnitaryPass : public IPass {
public:
    MergeSingleQubitUnitaryPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
