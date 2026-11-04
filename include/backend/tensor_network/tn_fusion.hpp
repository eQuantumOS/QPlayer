/// @file tn_fusion.hpp
/// @brief TN gate fusion — consecutive same-target gates merged before execution.
/// @ingroup backend
///
/// Every two-qubit gate on an MPS costs a merge, an SVD and a split, so two
/// consecutive gates on the same pair cost twice what their product costs once.
/// This pass multiplies such runs together before the state is touched. It also
/// turns the op stream into a form the MPS path can execute without re-deciding
/// per op what is supported — unsupported families are rejected here, before any
/// amplitude has been modified.

#pragma once

#include "circuit/gate_op.hpp"
#include "types.hpp"

#include <complex>
#include <vector>

namespace qplayer {
namespace detail {

/// One executable step of the fused stream.
/// @ingroup backend
struct TnStep {
    /// What the step does; barriers vanish here since their only role is to break fusion.
    enum class Kind { Gate, Measure, Reset, If };

    Kind kind = Kind::Gate;

    /// Operand qubits in gate order — 1, 2 or 3 entries for Kind::Gate, the single
    /// reset target for Kind::Reset, and the BODY's operands for Kind::If.
    std::vector<QubitIndex> qubits;

    /// Dense unitary in OPERAND order (4 or 16 entries). Empty for Kind::Measure
    /// and Kind::Reset — Reset has no matrix at all, which is precisely why it
    /// gets its own kind instead of a Gate step. Also empty
    /// for a 3Q Gate step, where `gate` carries the identity instead: the fixed
    /// decomposition is chosen by gate type, so an 8x8 matrix here would be dead
    /// data. For Kind::If it carries the BODY's matrix, resolved
    /// once here rather than per shot, and is empty when `body_kind` says the
    /// body is a Reset.
    std::vector<std::complex<double>> matrix;


    /// Valid when kind == Measure; the collapse happens here on the per-shot
    /// path and is deferred to the end otherwise.
    MeasureOp measure{};

    /// Valid when kind == If: the register and value the body is gated on
    /// The register value is rebuilt per shot from that
    /// shot's classical bits, so only the two scalars travel in the step —
    /// carrying a whole IfOp would put a GateOp in every step of every stream.
    ClassicalRegisterIndex creg       = 0;
    std::uint64_t          cond_value = 0;

    /// Valid when kind == If: Gate or Reset, saying how to run the body. Reusing
    /// Kind keeps "what the body is" and "what a step is" one vocabulary.
    Kind body_kind = Kind::Gate;

    /// Valid when the step is a 3Q gate (`qubits.size() == 3`): which of the IR's
    /// two three-qubit gates it is, so MpsState::apply_3q() can pick the fixed
    /// decomposition. Left at the default for 1Q/2Q steps, where a
    /// fused window has no single gate type to name.
    ///
    /// Declared LAST on purpose: tn_fuse() builds steps with positional aggregate
    /// initialisation, so inserting a field mid-struct would silently reassign
    /// every existing brace list.
    GateType gate = GateType::I;
};

/// Fuses consecutive 1Q runs on one qubit and 2Q runs on one pair.
///
/// A run is broken by anything else touching those qubits — another gate, a
/// measurement, or a barrier — so the pass tracks, per qubit, which step last
/// wrote it. Fusing across such a boundary would reorder operations that do not
/// commute.
///
/// Reset is emitted as its own Kind rather than routed through
/// gate_full_matrix(): it has no unitary, and asking for one used to surface a
/// `[FullState]`-tagged error from a TensorNetwork run.
///
/// Three-qubit gates (`ccx`, `cswap`) are emitted as their own step and break
/// the run on all three operands: they match neither fusion rule, and the
/// backend runs them through MpsState::apply_3q()'s fixed decomposition
/// Gates on FOUR or more qubits do not exist in the IR outside
/// `Opaque`, which is rejected here.
///
/// throws: SimulatorError — an op family the MPS path cannot execute (opaque,
///   gates on 4+ qubits, an opaque IfOp body). Rejecting here means the failure
///   happens before the state has been half-evolved.
/// @ingroup backend
std::vector<TnStep> tn_fuse(const std::vector<Op>& ops);

/// Reorders gate steps so each multi-qubit step lands next to the orthogonality
/// centre left by the previous one. Only steps on disjoint qubits
/// swap places, so the circuit's unitary is unchanged; measure, reset and
/// conditional steps stay where they are and bound the reordering. Without it a
/// brickwork layer change sends the centre back across the whole chain.
/// @param steps          fused stream from tn_fuse()
/// @param site_of_qubit  chain site of each qubit
/// @return the same steps in centre-following order
/// @ingroup backend
std::vector<TnStep> tn_sweep_order(const std::vector<TnStep>& steps,
                                   const std::vector<QubitIndex>& site_of_qubit);

}  // namespace detail
}  // namespace qplayer
