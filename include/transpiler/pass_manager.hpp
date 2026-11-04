// pass_manager.hpp — Ordered IPass executor.
// Holds passes in registration order. run() iterates and invokes each pass
// in sequence, emitting qplayer::log::debug breadcrumbs around each step
// Circuit value semantics: each pass receives the output of the
// previous pass as its input.

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "transpiler/ipass.hpp"

namespace qplayer {

class Circuit;  // Forward declaration — ipass.hpp already pulls circuit.hpp,
                // but keep the forward declaration as documentation intent.

class PassManager {
public:
    PassManager();
    ~PassManager();

    // Move constructor / assignment needed because unique_ptr<IPass> vector
    // is non-copyable. make_default() returns by value so move must be available.
    PassManager(PassManager&&)            noexcept = default;
    PassManager& operator=(PassManager&&) noexcept = default;

    // Copying a PassManager is disabled -- passes are non-copyable.
    PassManager(const PassManager&)            = delete;
    PassManager& operator=(const PassManager&) = delete;

    // Append a pass to the pipeline. Ownership transfers in.
    void add_pass(std::unique_ptr<IPass> pass);

    // Run every registered pass against the Circuit in registration order.
    // Returns the final transformed Circuit. Each pass receives the output
    // of the previous pass as its input (value chain).
    // Passes may throw CircuitError / ConnectivityError — PassManager does
    // not catch them; the caller (QRegister::transpile) propagates.
    Circuit run(Circuit circuit) const;

    // Factory: returns a PassManager pre-loaded with the Level-1 default
    // passes (CancelInverse → CommutationReorder → MergeSingleQubit).
    // SemanticValidatorPass is NOT included — Transpiler::transpile() inserts
    // it dynamically at position ⓪. Advanced API — general callers should use
    // QRegister::transpile().
    static PassManager make_default();

    // Number of registered passes. Useful for unit-test introspection.
    std::size_t size() const noexcept;

private:
    std::vector<std::unique_ptr<IPass>> passes_;
};

}  // namespace qplayer
