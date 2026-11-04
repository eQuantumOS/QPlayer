/// @file merge_single_qubit_pass.hpp
/// @brief Combines consecutive 1Q gates via a conservative pattern table.
/// @ingroup transpiler
///
/// MergeSingleQubitPass declaration.
/// Combines consecutive same-qubit 1-qubit gates into a single gate using a
/// pre-defined pattern table. Patterns not in the table are left unchanged
/// (conservative approach). Rotation gates (RX/RY/RZ/U1/U2/U3) act as
/// boundaries — full matrix composition with rotation params is deferred to a
/// later enhancement. Boundary ops (Measure, Barrier, If, Idle) and Opaque
/// GateOps terminate the merge window.
///
/// Supported patterns (Phase 4 scope):
/// H X H → Z           H Z H → X
/// S S   → Z           Sdg Sdg → Z
/// T T   → S           Tdg Tdg → Sdg
///

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Rewrites table-known Clifford runs (H X H, S S, T T, ...); rotation gates act as boundaries.
/// @ingroup transpiler
class MergeSingleQubitPass : public IPass {
public:
    MergeSingleQubitPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
