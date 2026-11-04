/// @file remove_idle_qubit_pass.hpp
/// @brief Removes gates acting only on qubits that cannot influence a measurement.
/// @ingroup transpiler
///
/// RemoveIdleQubitPass declaration.
/// Removes GateOps that act exclusively on qubits that are never measured and
/// have no entanglement dependency on measured qubits. The circuit's num_qubits
/// value is preserved — only unreachable GateOps are removed.
///
/// Dependency analysis (Phase 4 — 1-step transitive closure):
/// kept = {q : q appears in any MeasureOp}
/// ∪ {q : q shares a multi-qubit GateOp with any kept qubit}
/// (applied once; deeper closure is a Phase 4+ enhancement)
///
/// Conservative passes-through (unchanged):
/// IfOp bodies, BarrierOp, IdleOp — too complex to analyse safely at this
/// level; they are always emitted to the output.
///
/// Level 2/3 only — not registered in Level 0/1 PassPolicy.
///

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Keeps measured qubits plus their one-step entanglement neighbours and drops gates outside that set; num_qubits is preserved.
/// @ingroup transpiler
class RemoveIdleQubitPass : public IPass {
public:
    RemoveIdleQubitPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
