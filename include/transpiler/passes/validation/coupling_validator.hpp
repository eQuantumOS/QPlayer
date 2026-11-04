// coupling_validator.hpp — coupling-map validator pass
// Throws qplayer::ConnectivityError when a
// 2Q gate operand pair is absent from the device coupling map. Skipped when
// no DeviceModel is bound to the transpile() call.

#pragma once

#include "transpiler/ipass.hpp"
#include "noise/device_model.hpp"

namespace qplayer {

class Circuit;

class CouplingValidatorPass : public IPass {
public:
    explicit CouplingValidatorPass(const DeviceModel& device);

    std::string name() const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    const DeviceModel* device_  = nullptr;
    bool               changed_ = false;
};

}  // namespace qplayer
