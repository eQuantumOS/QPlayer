// idle_noise_inserter_pass.hpp — IdleNoiseInserterPass (scheduling stage).
// For every timed GateOp, inserts an
// IdleOp{q, duration_ns} for each qubit not participating in that gate, so the
// backend can apply T1/T2 relaxation on idle qubits
// Per-gate model (ALAP deferred).
//
// Registration: scheduling stage, after DurationAnnotationPass, gated on
// SimulatorBackend::supports_noise() && NoiseModel::has_t1_t2().
// That PassManager wiring lands with the device-aware transpile() pipeline
// the pass itself is unit-tested standalone here. The backend
// IdleOp Kraus application (apply_idle) is activated with the Monte Carlo
// trajectory machinery.

#pragma once

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;

class IdleNoiseInserterPass : public IPass {
public:
    IdleNoiseInserterPass() = default;

    std::string name()    const override;
    Circuit     run(Circuit circuit) override;
    bool        changed() const override;

private:
    bool changed_ = false;
};

}  // namespace qplayer
