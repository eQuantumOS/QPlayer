/// @file gate_op.hpp
/// @brief Op variant node definitions for the Circuit IR
///
/// @ingroup ir
///
/// Every executable node the
/// PassManager and Backend touch lives here. UnitaryMatrix + KrausChannel come
/// from noise/noise_model.hpp to keep the matrix typedef in a single home.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "types.hpp"
#include "noise/noise_model.hpp"   // UnitaryMatrix + KrausChannel SSOT

namespace qplayer {

/// Built-in gate kinds. GateType::Opaque + GateOp::opaque_name covers user-
/// defined gates. The list mirrors the full OpenQASM v2 qelib1.inc spec
/// (Appendix A: 25 gates) plus Reset / Opaque. Built-in U and CX
/// alias onto U3 / CX at parse time. Extending it requires synchronised
/// updates in:
///   - SemanticValidator symbol table (Phase 2)
///   - each Backend's apply_gate() dispatch (Phase 5+)
///   - Python bindings (Phase 7+)
/// describes the full procedure.
enum class GateType {
    // 1-qubit Clifford+T (qelib1: id, x, y, z, h, s, sdg, t, tdg, sx, sxdg)
    // SX = sqrt(X), SXdg = SX dagger. SX*SX = X, SX*SXdg = I.
    I, X, Y, Z, H, S, Sdg, T, Tdg, SX, SXdg,
    // 1-qubit rotations / U gates (qelib1: rx, ry, rz, u1, u2, u3; built-in U aliases U3)
    RX, RY, RZ, U1, U2, U3,
    // 2-qubit (qelib1: cx, cy, cz, ch, swap, crz, cu1, cu3; built-in CX aliases CX)
    CX, CY, CZ, CH, SWAP, CRZ, CU1, CU3,
    /// ECR (Echoed Cross-Resonance) — IBM Eagle native 2-qubit basis gate
    /// Genuine 4x4 unitary (not control+1Q), self-inverse (Hermitian),
    /// not in qelib1.inc: created only via Circuit::ecr() / basis translation, never QASM.
    ECR,
    // Google native 2-qubit basis gates. All genuine 4x4 unitaries,
    // none self-inverse, not in qelib1.inc: created via Circuit::iswap() /
    // sqrt_iswap() / fsim() only, never via OpenQASM v2.
    // iSWAP: [[1,0,0,0],[0,0,i,0],[0,i,0,0],[0,0,0,1]] (Qiskit/Cirq convention).
    // sqrt_iSWAP: [[1,0,0,0],[0,1/sqrt2,i/sqrt2,0],[0,i/sqrt2,1/sqrt2,0],[0,0,0,1]].
    // fSim(theta,phi): [[1,0,0,0],[0,cosθ,-i sinθ,0],[0,-i sinθ,cosθ,0],[0,0,0,e^{-iφ}]].
    // fSim params: op.params[0]=theta, op.params[1]=phi.
    // Sycamore = fSim(pi/2, pi/6) — no separate GateType, uses FSIM with these params.
    ISWAP, SQRT_ISWAP, FSIM,
    // 3-qubit (qelib1: ccx (Toffoli), cswap (Fredkin))
    CCX, CSwap,
    /// Non-unitary primitive — collapse target qubit to |0>. Backend dispatches
    /// through apply_reset(). Listed under GateType so
    /// the Op variant stays at five alternatives.
    Reset,
    /// User-defined opaque (matrix or Kraus, body lives in Circuit::opaque_definitions)
    Opaque,
};

/// Concrete gate application. duration_ns / noise_channels populate after
/// transpile() runs against a DeviceModel + NoiseModel.
/// is_fused / is_opaque flags carry IR-stage state forward into the backend
/// dispatcher so it can skip redundant work (F-OPT).
struct GateOp {
    GateType                  type        = GateType::I;
    std::string               name;             // Canonical lower-case form (e.g. "h", "cx"). Required for opaque GateType::Opaque.
    std::vector<QubitIndex>   qubits;
    std::vector<double>       params;           // Numeric parameters (rotation angles, etc.).
    std::string               opaque_name;      // Non-empty only when type == Opaque.
    std::optional<double>     duration_ns;      // Filled by DurationAnnotationPass (Phase 6).
    std::vector<KrausChannel> noise_channels;   // Filled by noise pipeline (Phase 8).
    bool                      is_fused   = false;
    bool                      is_opaque  = false;
};

/// Measurement of a single qubit into a single cubit. measure_all() expands to
/// num_qubits MeasureOps with cubit = qubit.
struct MeasureOp {
    QubitIndex            qubit = 0;
    ClassicalBitIndex     cubit = 0;
    /// Measurement duration (ns), filled by DurationAnnotationPass when the device
    /// describes a measure time. Scheduling-only: ALAPSchedulePass uses
    /// it so bystander qubits idle during readout; backends ignore it (measurement
    /// is instantaneous in simulation). nullopt keeps JSON round-trip backward-
    /// compatible and aggregate init {qubit, cubit} unchanged. The default member
    /// initializer keeps {qubit, cubit} aggregate init warning-free
    /// (-Wmissing-field-initializers).
    std::optional<double> duration_ns = std::nullopt;
};

/// Barrier — fusion / reordering boundary. Empty qubits list means "all".
struct BarrierOp {
    std::vector<QubitIndex> qubits;
};

/// Idle noise carrier inserted by ALAPSchedulePass at PassManager stage ⑥
/// Single-qubit node; duration_ns drives T1/T2 → Kraus conversion.
/// User-facing Circuit builder has no idle() method — the pass is the only
/// generator.
struct IdleOp {
    QubitIndex qubit       = 0;
    double     duration_ns = 0.0;
};

// Forward declaration so IfOp.body can reference GateOp without recursion.
// + fix IfOp.body to a single GateOp.
// Conditional sequences are realised by repeating IfOp nodes — keeping body
// as a plain GateOp avoids variant<...> recursion and unique_ptr churn.

/// Classical control: execute body when the named ClassicalRegister value
/// (read as unsigned int) equals `value`..
struct IfOp {
    ClassicalRegisterIndex creg  = 0;
    std::uint64_t          value = 0;
    GateOp                 body;
};

/// IR Op variant — five alternatives.
/// Reset is encoded as GateOp{type = GateType::Reset, qubits = {q}} so the
/// variant width stays at the design SSOT and backend dispatch flows through
/// the shared apply_gate() path.
using Op = std::variant<GateOp, MeasureOp, BarrierOp, IfOp, IdleOp>;

/// Marker alternative for OpaqueDefinition.body indicating the opaque was
/// declared (e.g. by the QASM parser) but no implementation has been attached
/// yet. SemanticValidatorPass uses holds_alternative<DeclarationOnly> to reject
/// stub usage and prevent the "silent identity" failure mode.
struct DeclarationOnly {
    bool operator==(const DeclarationOnly&) const noexcept { return true; }
};

/// Upper bound on define_opaque(num_qubits) for the matrix overloads.
///
/// Lives here rather than in circuit.cpp so the backend can bind it to its own
/// kernel bound with a static_assert: gate_fusion.hpp checks
/// kMaxOpaqueQubits <= kMaxFusionWidth, because an opaque is applied through
/// apply_fused_gate() whose amp/out scratch is a fixed 1 << kMaxFusionWidth
/// stack array. The two caps used to be independent constants in separate files
/// that merely happened to agree; raising one alone would have overrun the
/// kernel stack silently.
///
/// The value is 4, not the kernel's 8: registration verifies U†U ≈ I in
/// O(2^3n), so 8 qubits means a 256x256 matrix and ~1.7e7 inner products
/// (measured ~40 ms per define_opaque call). 4 covers the realistic cases —
/// CCX/CSwap-shaped 3-qubit blocks and 4-qubit oracle tiles — and matches the
/// default run(fusion_max_width=4), i.e. the width fusion actually emits.
/// Kraus bodies are further limited to 1-2 qubits by the Kraus kernels.
constexpr QubitIndex kMaxOpaqueQubits = 4;

/// Opaque definition stored on the Circuit itself (+).
/// Body is a 3-variant: declaration-only stub, unitary matrix, or Kraus channel.
/// Validation (dimension match, unitary / trace-preserving, name collisions)
/// runs at define_opaque() / declare_opaque() time.
struct OpaqueDefinition {
    std::string                                                 name;
    QubitIndex                                                  num_qubits = 0;
    std::variant<DeclarationOnly, UnitaryMatrix, KrausChannel>  body       = DeclarationOnly{};
};

}  // namespace qplayer
