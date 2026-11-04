/// @file coupling_validator.hpp
/// @brief Rejects two-qubit gates whose operand pair is absent from the device coupling map.
/// @ingroup transpiler
///
/// coupling-map validator pass
/// Throws qplayer::ConnectivityError when a
/// 2Q gate operand pair is absent from the device coupling map. Skipped when
/// no DeviceModel is bound to the transpile() call.

#pragma once

#include "transpiler/ipass.hpp"
#include "noise/device_model.hpp"

namespace qplayer {

class Circuit;

/// Throws ConnectivityError on a 2Q gate that violates the bound DeviceModel's coupling map; a no-op when no device is bound.
/// @ingroup transpiler
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
