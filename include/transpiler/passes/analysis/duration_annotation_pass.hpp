// duration_annotation_pass.hpp — DurationAnnotationPass (analysis stage).
// Fills each GateOp.duration_ns
// that is still empty from the DeviceModel gate-duration table so the
// downstream IdleNoiseInserterPass (scheduling ⑥) has per-gate timing.
//
// Registration: runs as the first pass of the ⑥ scheduling stage, gated on
// DeviceModel + NoiseModel(T1/T2) both present. That
// PassManager wiring lands with the device-aware transpile() pipeline;
// the pass itself is unit-tested standalone here.

#pragma once

#include "noise/device_model.hpp"
#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class DurationAnnotationPass : public IPass {
public:
    explicit DurationAnnotationPass(const DeviceModel& device);

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    const DeviceModel* device_  = nullptr;
    bool               changed_ = false;
};

}  // namespace qplayer
