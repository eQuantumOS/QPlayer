/// @file transpiler.cpp
/// @brief Transpiler facade implementing QRegister::transpile()
/// delegation (optimization levels 0-3).
///
/// PassPolicy summary:
/// Level 0:  ⓪ SemanticValidatorPass only
/// Level 1:  ⓪ + ① CancelInverse → CommutationReorder → MergeSingleQubit
/// Level 2:  Level 1 + DiagonalPhase → RemoveIdentity → MergeSameGate
/// → PatternReplace → MergeSingleQubitUnitary (full-unitary 1Q merge)
/// Level 3:  Level 2 + RemoveIdleQubit → MergeTwoQubit (2Q + qubit pruning)
/// Level <0 or >3: CircuitError — unknown level.
/// device bound (valid device_json or member DeviceModel): append the
/// ② InitialLayout → ③ SwapRouter → ④ BasisGateTranslation →
/// ⑤ (CancelInverse → SwapCancel → MergeSingleQubit) →
/// ⑤ verify (CouplingValidator) →
/// ⑥ DurationAnnotation (physical space, when a T1/T2 NoiseModel is
/// bound) → truncation (QubitTruncation) → ALAPSchedule (dense
/// space, slot-aware idle) stages.

#include "transpiler/transpiler.hpp"

#include <memory>
#include <string>

#include "circuit/circuit.hpp"
#include "error.hpp"
#include "log.hpp"
#include "noise/device_model.hpp"
#include "noise/noise_model.hpp"
#include "transpiler/pass_manager.hpp"
#include "transpiler/passes/analysis/duration_annotation_pass.hpp"
#include "transpiler/passes/layout/initial_layout_pass.hpp"
#include "transpiler/passes/routing/swap_router_pass.hpp"
#include "transpiler/passes/translation/basis_gate_translation_pass.hpp"
#include "transpiler/passes/scheduling/alap_schedule.hpp"
#include "transpiler/passes/validation/coupling_validator.hpp"
#include "transpiler/passes/optimization/cancel_inverse.hpp"
#include "transpiler/passes/optimization/swap_cancel_pass.hpp"
#include "transpiler/passes/optimization/qubit_truncation_pass.hpp"
#include "transpiler/passes/optimization/commutation_reorder_pass.hpp"
#include "transpiler/passes/optimization/diagonal_phase_pass.hpp"
#include "transpiler/passes/optimization/merge_same_gate_pass.hpp"
#include "transpiler/passes/optimization/merge_single_qubit_pass.hpp"
#include "transpiler/passes/optimization/merge_single_qubit_unitary_pass.hpp"
#include "transpiler/passes/optimization/merge_two_qubit_pass.hpp"
#include "transpiler/passes/optimization/pattern_replace_pass.hpp"
#include "transpiler/passes/optimization/remove_identity_pass.hpp"
#include "transpiler/passes/optimization/remove_idle_qubit_pass.hpp"
#include "transpiler/passes/validation/semantic_validator_pass.hpp"

namespace qplayer {

Transpiler::Transpiler()  = default;
Transpiler::~Transpiler() = default;

Circuit Transpiler::transpile(Circuit            circuit,
                              int                optimization_level,
                              std::string_view   device_json,
                              const DeviceModel* device_fallback,
                              const NoiseModel*  noise_fallback) {
    // Only Level 0..3 are defined. Anything outside that range
    // is a user-side bug; fail loudly rather than silently downgrading.
    if (optimization_level < 0 || optimization_level > 3) {
        throw CircuitError{
            "optimization_level " + std::to_string(optimization_level) +
            " is out of range; supported: 0, 1, 2, 3"};
    }

    // Resolve the effective device (⓪.5 device-entry decision):
    // a valid device_json wins; an empty / invalid one falls back to the member
    // DeviceModel; neither -> device-less compile (skip ②/③/⑤verify/⑥).
    //
    // device_json is a FILE PATH or JSON CONTENT, auto-detected:
    // the first non-whitespace char '{' means JSON content (device JSON is
    // always a top-level object), otherwise a file path. This mirrors
    // load_device_model(path) so callers may pass either form.
    DeviceModel        parsed_device;
    bool               have_parsed = false;
    if (!device_json.empty()) {
        const std::size_t first = device_json.find_first_not_of(" \t\r\n");
        const bool looks_like_json =
            first != std::string_view::npos && device_json[first] == '{';
        try {
            parsed_device = looks_like_json
                ? DeviceModel::from_json_string(std::string(device_json))
                : DeviceModel::from_json_file(std::string(device_json));
            have_parsed = true;
        } catch (const CircuitError& e) {
            // Bad path / malformed JSON: warn (previously a silent drop)
            // and fall back to the member device rather than throwing.
            log::warn(std::string{"transpile: device_json could not be loaded ("} +
                      e.what() +
                      "); falling back to the member DeviceModel if set.");
        }
    }
    const DeviceModel* device = have_parsed     ? &parsed_device
                              : device_fallback ? device_fallback
                                                : nullptr;

    // Build PassManager dynamically from the requested optimization_level.
    // ⓪ SemanticValidatorPass is always prepended (validation-only, changed=false).
    PassManager pm;

    // Stage ⓪: semantic validation — run regardless of optimization_level.
    pm.add_pass(std::make_unique<SemanticValidatorPass>());

    if (optimization_level >= 1) {
        // Stage ①: Level-1 optimization passes.
        pm.add_pass(std::make_unique<CancelInversePass>());
        pm.add_pass(std::make_unique<CommutationReorderPass>());
        pm.add_pass(std::make_unique<MergeSingleQubitPass>());
    }

    if (optimization_level >= 2) {
        // Level 2 adds 1Q safe reductions.
        pm.add_pass(std::make_unique<DiagonalPhasePass>());
        pm.add_pass(std::make_unique<RemoveIdentityPass>());
        pm.add_pass(std::make_unique<MergeSameGatePass>());
        pm.add_pass(std::make_unique<PatternReplacePass>());
        // Full-unitary 1Q merge: compose arbitrary rotation sequences into a
        // single U3.  Runs after the Clifford-pattern passes so those reductions
        // are visible before matrix composition (extension).
        pm.add_pass(std::make_unique<MergeSingleQubitUnitaryPass>());
    }

    if (optimization_level >= 3) {
        // Level 3 adds 2Q merging + qubit pruning.
        // These can affect qubit indices / routing so they stay opt-in.
        pm.add_pass(std::make_unique<RemoveIdleQubitPass>());
        pm.add_pass(std::make_unique<MergeTwoQubitPass>());
    }

    // ── Device-aware stages (② ③ ⑤ ⑥) — only when a device is bound ──
    if (device != nullptr) {
        // ② layout: logical->physical placement (Trivial default).
        pm.add_pass(std::make_unique<InitialLayoutPass>(*device));
        // ③ routing: insert SWAPs to honour the coupling map.
        pm.add_pass(std::make_unique<SwapRouterPass>(*device));

        // ④ translation: decompose logical gates into the device native basis
        // Placed right after routing so router-inserted
        // SWAPs are also decomposed, and before scheduling so noise/duration bind
        // to native gate names. No-op when the device declares no
        // basis_gates. The ⑤ passes below are basis-preserving on native output
        // (CancelInverse only removes pairs; MergeSingleQubit's patterns never
        // match rz/sx/x), so the native-basis guarantee survives them.
        pm.add_pass(std::make_unique<BasisGateTranslationPass>(*device));

        if (optimization_level >= 1) {
            // ⑤ re-optimization: cancel routing-induced redundancy, then re-run
            // the 1Q merge.
            pm.add_pass(std::make_unique<CancelInversePass>());
            pm.add_pass(std::make_unique<SwapCancelPass>());
            pm.add_pass(std::make_unique<MergeSingleQubitPass>());
        }

        // ⑤ verify: reject any residual coupling-map violation.
        // Runs before truncation so 2Q adjacency is checked in physical space
        // (the device coupling map is keyed by physical qubit).
        pm.add_pass(std::make_unique<CouplingValidatorPass>(*device));

        // ⑥ scheduling is split around truncation so device-keyed lookups stay
        // in physical space (handoff). Only wire it when a NoiseModel
        // carrying T1/T2 is bound. SparseState's
        // no-noise constraint is enforced downstream.
        const bool want_idle_noise =
            (noise_fallback != nullptr && noise_fallback->has_t1_t2());

        // DurationAnnotation runs BEFORE truncation: it keys device gate
        // durations by physical qubit, so it must see physical operands. The
        // duration_ns it stamps is a scalar that survives truncation's operand
        // remap unchanged, so idle timing stays correct under a non-identity
        // dense->physical mapping (handoff).
        if (want_idle_noise) {
            pm.add_pass(std::make_unique<DurationAnnotationPass>(*device));
        }

        // truncation: compact referenced physical qubits into dense [0,k) so a
        // small circuit on a wide device runs as 2^k, not 2^(device width)
        // After verify, before idle-noise insertion —
        // no-op when the circuit already occupies a dense qubit set.
        pm.add_pass(std::make_unique<QubitTruncationPass>());

        // ALAPSchedule runs AFTER truncation (dense space) so idle ops are
        // inserted only for the k live qubits — inserting them pre-truncation
        // would mark every device qubit as "used" and defeat the compaction. It
        // schedules the circuit into time slots and materialises each qubit's
        // idle windows as IdleOps. It replaced the per-gate IdleNoiseInserterPass,
        // which was removed in Phase 8-F :
        // parallel gates share a slot, so bystander idle is counted once, not
        // once per foreign gate (supersedes 's per-gate model).
        if (want_idle_noise) {
            pm.add_pass(std::make_unique<ALAPSchedulePass>());
        }
    }

    log::debug("Transpiler: running PassManager with " +
               std::to_string(pm.size()) +
               " pass(es), optimization_level=" +
               std::to_string(optimization_level));

    return pm.run(std::move(circuit));
}

}  // namespace qplayer
