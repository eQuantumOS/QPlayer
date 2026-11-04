// initial_layout_pass.hpp — InitialLayoutPass (layout stage).
// Records the logical->physical qubit mapping in CircuitMetadata.layout. The
// default is a Trivial identity map (logical i -> physical i); a caller may
// supply layout hints (a permutation indexed by logical qubit) to override it.
// SABRE-style noise-aware placement was deferred and would
// arrive as a future IPass subclass — it is intentionally out of scope here.
//
// The pass does not rewrite op qubit indices; it only stamps the mapping so the
// downstream SwapRouterPass routes against it and the backend can remap
// measurement bitstrings back to logical indices. It also copies the
// device coupling map into CircuitMetadata when one is present.
//
// Registration: runs as the ② layout stage of the device-aware transpile()
// pipeline, executed only when device info is supplied. That
// The pass is also unit-tested standalone here.

#pragma once

#include <optional>
#include <vector>

#include "noise/device_model.hpp"
#include "transpiler/ipass.hpp"
#include "types.hpp"

namespace qplayer {

class Circuit;

class InitialLayoutPass : public IPass {
public:
    // Trivial layout: logical i maps to physical i (default).
    explicit InitialLayoutPass(const DeviceModel& device);

    // Custom layout hints: hints[logical] == physical. run() throws
    // CircuitError when the hint vector size != circuit.num_qubits(), is not a
    // permutation (duplicate physical qubit), or maps beyond the device range.
    InitialLayoutPass(const DeviceModel& device, std::vector<QubitIndex> hints);

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    const DeviceModel*                     device_  = nullptr;
    std::optional<std::vector<QubitIndex>> hints_;
    bool                                   changed_ = false;
};

}  // namespace qplayer
