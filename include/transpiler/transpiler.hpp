// transpiler.hpp — Facade behind QRegister::transpile().
// Internally assembles a PassManager based on optimization_level and the
// device_json / DeviceModel fallback. The Transpiler instance is lightweight
// and constructed per call inside QRegister.

#pragma once

#include <string_view>

#include "circuit/circuit.hpp"

namespace qplayer {

class DeviceModel;
class NoiseModel;

class Transpiler {
public:
    Transpiler();
    ~Transpiler();

    // Single compile entry point matching QRegister::transpile() signature
    // device_model / noise_model are optional fallbacks
    // used when device_json is empty / invalid.
    Circuit transpile(Circuit            circuit,
                      int                optimization_level = 1,
                      std::string_view   device_json        = "",
                      const DeviceModel* device_fallback    = nullptr,
                      const NoiseModel*  noise_fallback     = nullptr);
};

}  // namespace qplayer
