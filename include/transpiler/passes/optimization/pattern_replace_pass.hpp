/// @file pattern_replace_pass.hpp
/// @brief Replaces known single-qubit gate sequences with shorter equivalents.
/// @ingroup transpiler
///
/// PatternReplacePass declaration.
/// Recognises explicit gate-sequence patterns on a single qubit and replaces
/// them with shorter equivalent sequences. Global phase is ignored.
///
/// Phase 4 pattern table (8 patterns):
/// T·T        → S            (2-gate)
/// S·S        → Z            (2-gate)
/// T·T·T·T    → Z            (4-gate)
/// Tdg·Tdg    → Sdg          (2-gate)
/// Sdg·Sdg    → Z            (2-gate)
/// H·H        → I (removed)  (2-gate)
/// X·X        → I (removed)  (2-gate)
/// H·X·H      → Z            (3-gate)
/// H·Z·H      → X            (3-gate)
///
/// Level 2/3 only — not registered in Level 0/1 PassPolicy.
/// Boundary ops (Measure, Barrier, If, Idle) and Opaque GateOps terminate the
/// per-qubit sliding window.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Applies a fixed rewrite table (T-T to S, T-T-T-T to Z, ...), ignoring global phase.
/// @ingroup transpiler
class PatternReplacePass : public IPass {
public:
    PatternReplacePass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
