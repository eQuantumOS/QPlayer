// ipass.hpp — Pass interface.
// Every concrete compiler pass derives from IPass. PassManager owns a vector
// of unique_ptr<IPass> and invokes run() in registration order.
//
// Design: run() takes Circuit by value and returns a transformed Circuit so
// passes can either mutate in place or build a new Circuit without reference
// aliasing issues.

#pragma once

#include <string>

#include "circuit/circuit.hpp"

namespace qplayer {

class IPass {
public:
    virtual ~IPass() = default;

    // Human-readable pass identifier — used by qplayer::log::debug messages
    // and by PassManager diagnostics.
    virtual std::string name() const = 0;

    // Transform the circuit. Takes ownership by value; returns the (possibly
    // modified) Circuit. Implementations that do not modify the circuit must
    // still return the input Circuit unchanged.
    // passes: mutate ops_ and return modified copy.
    // Validation passes: verify and return input unchanged.
    virtual Circuit run(Circuit circuit) = 0;

    // Returns true if the last run() call modified the circuit.
    // PassManager may use this flag for early exit.
    virtual bool changed() const = 0;
};

}  // namespace qplayer
