/// @file basis_gate_translation_pass.hpp
/// @brief Decomposes gates into the device's native basis set.
/// @ingroup transpiler
///
/// BasisGateTranslationPass (translation stage,
/// /). Decomposes logical gates into the device's
/// native basis (DeviceModel.basis_gates, e.g. IBM Eagle {id, rz, sx, x, ecr}):
/// - single-qubit non-basis gates -> ZSX Euler (rz·sx·rz·sx·rz, trivial rz dropped)
/// - cx -> ecr template (rz·sx·ecr·x) when the native 2Q gate is ecr
/// - swap -> cx×3, each cx then translated to the native 2Q gate
/// - gates already in the basis pass through unchanged
/// rz is a virtual (zero-duration, noiseless) frame change on real hardware.
///
/// Placement: runs as the ④ translation stage of the
/// device-aware transpile() pipeline — after ③ routing (so router-inserted SWAPs
/// are also decomposed) and before ⑥ scheduling (so noise/duration attach to
/// native gate names). It is a no-op when the device declares no
/// basis_gates (nothing to translate to). Only basis-preserving optimization
/// (CancelInverse/SwapCancel) may follow, or the native-basis guarantee breaks.

#pragma once

#include "noise/device_model.hpp"
#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

/// Rewrites non-basis 1Q gates as ZSX Euler sequences and non-native 2Q gates via the ECR or CX template.
/// @ingroup transpiler
class BasisGateTranslationPass : public IPass {
public:
    explicit BasisGateTranslationPass(const DeviceModel& device);

    std::string name()    const override;
    // throws: CircuitError — a gate cannot be expressed in the device basis:
    // an arbitrary 2Q/3Q gate with no decomposition rule (KAK is),
    //         or the basis lacks rz/sx/x needed by the ZSX / ecr templates.
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    const DeviceModel* device_  = nullptr;
    bool               changed_ = false;
};

}  // namespace qplayer
