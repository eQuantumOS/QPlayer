/// @file swap_router_pass.hpp
/// @brief Inserts SWAP gates so every two-qubit gate acts on a coupling-adjacent pair.
/// @ingroup transpiler
///
/// SwapRouterPass (routing stage).
/// Inserts atomic SWAP gates so every two-qubit gate acts on a
/// coupling-adjacent physical pair, using a BFS shortest-path greedy strategy
/// (SABRE-style routing deferred). Consumes the logical->physical layout that
/// InitialLayoutPass stamped into CircuitMetadata.layout and updates it
/// as SWAPs permute qubits, recording the final mapping for the backend.
///
/// Execution model: the routed circuit runs in PHYSICAL space, so the
/// pass rewrites the qubit operands of EVERY op (GateOp incl. Reset/SWAP,
/// BarrierOp, IfOp body, IdleOp, and MeasureOp) through the running layout.
/// Rewriting a measure to its qubit's current physical home means logical qubit
/// q's outcome lands in its classical bit, so results are logical-indexed with
/// no separate backend remap (realised in the transpiler). Using
/// the layout at the measure's position keeps mid-circuit measurement correct
/// under later SWAPs.
///
/// Registration: runs as the ③ routing stage of the device-aware transpile()
/// pipeline, after ② layout, only when device info is supplied. That
/// PassManager wiring lands; the pass is unit-tested standalone here.

#pragma once

#include "noise/device_model.hpp"
#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// BFS shortest-path greedy router; rewrites every op into physical space and updates the running layout as SWAPs permute qubits.
/// @ingroup transpiler
class SwapRouterPass : public IPass {
public:
    explicit SwapRouterPass(const DeviceModel& device);

    std::string name()    const override;
    // throws: CircuitError — coupling graph cannot route a 2Q gate (the two
    //         physical qubits lie in disconnected components).
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    const DeviceModel* device_  = nullptr;
    bool               changed_ = false;
};

}  // namespace qplayer
