/// @file pass_manager.cpp
/// @brief PassManager implementation.
///
/// add_pass() / size() manage the pass vector; run() chains Circuit values
/// through each registered pass in order and emits debug breadcrumbs
/// make_default() registers the Level-1 default passes
/// (CancelInverse → CommutationReorder → MergeSingleQubit).

#include "transpiler/pass_manager.hpp"

#include <memory>

#include "circuit/circuit.hpp"
#include "log.hpp"
#include "transpiler/passes/optimization/cancel_inverse.hpp"
#include "transpiler/passes/optimization/commutation_reorder_pass.hpp"
#include "transpiler/passes/optimization/merge_single_qubit_pass.hpp"

namespace qplayer {

PassManager::PassManager()  = default;
PassManager::~PassManager() = default;

void PassManager::add_pass(std::unique_ptr<IPass> pass) {
    passes_.push_back(std::move(pass));
}

Circuit PassManager::run(Circuit circuit) const {
    for (const auto& pass : passes_) {
        log::debug("PassManager: running pass '" + pass->name() + "'");
        circuit = pass->run(std::move(circuit));
        log::debug("PassManager: pass '" + pass->name() +
                   "' done — changed=" +
                   (pass->changed() ? "true" : "false"));
    }
    return circuit;
}

// Returns a PassManager pre-loaded with the Level-1 default passes:
//   CancelInverse → CommutationReorder → MergeSingleQubit
// SemanticValidatorPass is NOT included here because Transpiler::transpile()
// inserts it dynamically at position ⓪ regardless of optimization_level.
// This factory is an advanced API — general callers should use
// QRegister::transpile().
PassManager PassManager::make_default() {
    PassManager pm;
    pm.add_pass(std::make_unique<CancelInversePass>());
    pm.add_pass(std::make_unique<CommutationReorderPass>());
    pm.add_pass(std::make_unique<MergeSingleQubitPass>());
    return pm;
}

std::size_t PassManager::size() const noexcept {
    return passes_.size();
}

}  // namespace qplayer
