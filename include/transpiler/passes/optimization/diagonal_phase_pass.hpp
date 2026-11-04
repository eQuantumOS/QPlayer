/// @file diagonal_phase_pass.hpp
/// @brief Accumulates consecutive diagonal-gate phases into one gate.
/// @ingroup transpiler
///
/// DiagonalPhasePass declaration.
/// Accumulates the phases of consecutive same-qubit diagonal gates (Z, S, Sdg,
/// T, Tdg, RZ(θ), U1(λ)) and replaces them with a single gate or removes the
/// sequence when the net phase is 0 (identity). Global phase is ignored.
///
/// Level 2/3 only — not registered in Level 0/1 PassPolicy.
/// Boundary ops (Measure, Barrier, If, Idle) and non-diagonal gates terminate
/// the accumulation window for each qubit. Opaque GateOps pass through unchanged.
///
///

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Folds runs of Z/S/Sdg/T/Tdg/RZ/U1 on one qubit into a single gate, dropping the run entirely at net-zero phase.
/// @ingroup transpiler
class DiagonalPhasePass : public IPass {
public:
    DiagonalPhasePass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
