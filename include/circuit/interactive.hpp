/// @file interactive.hpp
/// @brief Interactive-mode facade.
/// @ingroup api
///
///
/// IMPORTANT: This header is included by include/qregister.hpp only. User code
/// must access the Interactive surface through QRegister::interactive — direct
/// inclusion is forbidden by CLAUDE.md and.
/// The private constructor + friend QRegister enforces this at compile time
///

#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "types.hpp"
#include "result.hpp"
#include "circuit/gate_op.hpp"   // GateOp — generic apply_gate() entry point.

namespace qplayer {

/// Forward declaration: Interactive holds a non-owning pointer back to its owner
/// for the CPU switch. qregister.hpp cannot be included here — it is the
/// header that includes this one (CLAUDE.md).
class QRegister;

class SimulatorBackend;  // Forward declaration — owned by QRegister.

/// Immediate-dispatch surface reached as QRegister::interactive.
/// @ingroup api
class Interactive {
public:
    /// ── State lifecycle ────────────────────────────────────────────
    /// initialize: allocate the |0...0> state on the bound backend.
    ///   Interactive supports CPU statevector backends only (FullState CPU,
    ///   SparseState). FullState GPU and TensorNetwork throw SimulatorError
    /// here — use the batch run() path for those.
    /// reset() (no arg): re-prepare |0...0> on the same qubit count.
    /// reset(q): collapse a single qubit to |0> while preserving the rest.
    /// @param num_qubits Width of the session state to allocate.
    /// @throws SimulatorError The bound backend is FullState GPU or
    ///                        TensorNetwork, which have no immediate mode.
    void initialize(QubitIndex num_qubits);

    /// Re-prepares |0...0> on the same qubit count.
    /// @throws SimulatorError No session is active — call initialize() first.
    void reset();

    /// Collapses one qubit to |0> while preserving the other qubits.
    /// @param q Qubit to reset.
    /// @throws SimulatorError No session is active, or @p q is out of range.
    void reset(QubitIndex q);

    /// @name Immediate gate application
    ///
    /// Every builder in this group applies its gate to the live session state at
    /// once — nothing is accumulated into an IR — and returns `*this` so calls
    /// chain. Operands are validated against num_qubits() and angles are in
    /// radians. Operand order is significant: the first operand of a controlled
    /// gate is the control. Each call invalidates any RawStateView previously
    /// handed out, and throws SimulatorError when no session is active.
    /// @{

    /// Hadamard.
    Interactive& h  (QubitIndex q);
    /// Pauli X.
    Interactive& x  (QubitIndex q);
    /// Pauli Y.
    Interactive& y  (QubitIndex q);
    /// Pauli Z.
    Interactive& z  (QubitIndex q);
    /// Phase gate S = diag(1, i).
    Interactive& s  (QubitIndex q);
    /// Inverse phase gate S^dagger.
    Interactive& sdg(QubitIndex q);
    /// T gate = diag(1, exp(i*pi/4)).
    Interactive& t  (QubitIndex q);
    /// Inverse T gate.
    Interactive& tdg(QubitIndex q);

    /// Rotation about X by @p theta.
    Interactive& rx(double theta,             QubitIndex q);
    /// Rotation about Y by @p theta.
    Interactive& ry(double theta,             QubitIndex q);
    /// Rotation about Z by @p theta.
    Interactive& rz(double theta,             QubitIndex q);
    /// Single-parameter phase gate u1(lambda).
    Interactive& u1(double lam,               QubitIndex q);
    /// Two-parameter gate u2(phi, lambda).
    Interactive& u2(double phi, double lam,   QubitIndex q);
    /// General single-qubit gate u3(theta, phi, lambda).
    Interactive& u3(double theta, double phi, double lam, QubitIndex q);

    /// Controlled X (CNOT).
    Interactive& cx  (QubitIndex ctrl, QubitIndex tgt);
    /// Controlled Y.
    Interactive& cy  (QubitIndex ctrl, QubitIndex tgt);
    /// Controlled Z.
    Interactive& cz  (QubitIndex ctrl, QubitIndex tgt);
    /// Exchanges the states of two qubits.
    Interactive& swap(QubitIndex q0,   QubitIndex q1);
    /// iSWAP — a Google native 2Q gate.
    Interactive& iswap     (QubitIndex q0, QubitIndex q1);
    /// Square root of iSWAP.
    Interactive& sqrt_iswap(QubitIndex q0, QubitIndex q1);
    /// fSim(theta, phi) — Google's tunable two-qubit gate.
    Interactive& fsim      (double theta, double phi, QubitIndex q0, QubitIndex q1);
    /// The Sycamore gate, equal to fsim(pi/2, pi/6).
    Interactive& sycamore  (QubitIndex q0, QubitIndex q1);
    /// Toffoli — X on @p tgt when both controls are set.
    Interactive& ccx(QubitIndex c0, QubitIndex c1, QubitIndex tgt);
    /// @}

    /// ── Generic gate entry point ────────────────────────
    /// Covers the full GateType enum so a parsed Circuit IR can be replayed
    /// op-by-op (the typed methods above miss SX/CH/CRZ/CU1/CU3/CSwap/…).
    /// GateType::Reset routes through the backend's apply_reset(). The typed
    /// gate methods are thin wrappers over this.
    /// @param op Gate to apply, including its operands and parameters.
    /// @return *this, so calls chain.
    /// @throws SimulatorError No session is active, or an operand is out of range.
    Interactive& apply_gate(const GateOp& op);

    /// ── Barrier (no-op for execution, fusion boundary) ─────────────
    /// No initializer_list overload — Interactive ops are immediate and we
    /// never accumulate them into an IR list. Pass operand qubits one at a
    /// time via repeated calls when needed.
    /// @return *this, so calls chain.
    Interactive& barrier();

    /// ── Measurement (immediate) ────────────────────────────────────
    /// measure(q): return 0/1, no cubit write.
    /// measure(q, cubit): accumulate into the bound Result (no value return).
    /// measure_all: — index i carries qubit i's outcome (size = num_qubits()).
    /// Measures one qubit and returns its outcome, collapsing the state.
    /// @param q Qubit to measure.
    /// @return The observed bit, 0 or 1.
    /// @throws SimulatorError No session is active, or @p q is out of range.
    int              measure   (QubitIndex q);

    /// Measures one qubit into the bound Result instead of returning the bit.
    /// @param q     Qubit to measure.
    /// @param cubit Destination bit in the accumulated Result.
    /// @throws SimulatorError No session is active, or an index is out of range.
    void             measure   (QubitIndex q, ClassicalBitIndex cubit);

    /// Measures every qubit, collapsing the state.
    /// @return Outcomes indexed by qubit, so index i carries qubit i's bit
    /// (size == num_qubits()).
    /// @throws SimulatorError No session is active.
    std::vector<int> measure_all();

    /// ── State access ─────────────────
    /// Lifetime of RawStateView ends at the next apply_gate / measure / reset
    /// call. SparseState throws SimulatorError on get_statevector_raw().
    /// Dense AoS copy of the session state.
    /// @return Amplitudes indexed by basis state.
    /// @throws SimulatorError No session is active.
    StateVector  get_statevector()                              const;

    /// Borrowed SoA view of the session state — no AoS conversion cost.
    /// @return View invalidated by the next apply_gate / measure / reset call.
    /// @throws SimulatorError No session is active, or the backend is
    ///                        SparseState, which holds no dense array.
    RawStateView get_statevector_raw()                          const;

    /// Probability of one outcome on one qubit, without collapsing the state.
    /// @param q       Qubit to inspect.
    /// @param outcome Outcome to weigh, 0 or 1.
    /// @return The probability in [0, 1].
    /// @throws SimulatorError No session is active, or @p q is out of range.
    double       probability      (QubitIndex q, int outcome)   const;

    /// Expectation value of a Pauli string over the session state.
    /// @param pauli Pauli string in big-endian order (pauli[0] is qubit n-1).
    /// @return The expectation value in [-1, 1].
    /// @throws SimulatorError No session is active.
    double       expectation_value(std::string_view pauli)      const;

    /// Enumerate nonzero-amplitude basis states as (index, amplitude) pairs in
    /// ascending StateIndex order without densifying. Requires an
    /// active session (initialize() called); throws SimulatorError otherwise.
    /// SparseState works up to 128 qubits; TensorNetwork always throws.
    /// @return (basis index, amplitude) pairs in ascending index order.
    /// @throws SimulatorError No session is active, or the backend is
    ///                        TensorNetwork.
    std::vector<std::pair<StateIndex, std::complex<double>>>
            get_statevector_nonzero() const;

    /// ── Accumulated measurement results ────────────────────────────
    /// Accumulation pattern is measure*() + result() / clear_result()
    /// Use QRegister::run for batched IR execution.
    /// Measurement outcomes accumulated by measure(q, cubit) so far.
    /// @return Reference to the accumulated result.
    const Result& result()       const;

    /// Discards the accumulated measurement outcomes, keeping the quantum state.
    void          clear_result();

    /// @return Width of the active session, or 0 when none is active.
    QubitIndex num_qubits() const;
    /// @return true when initialize() has run and no run() has invalidated it.
    bool       is_active()  const;

private:
    /// Only QRegister constructs Interactive. bind_backend() is
    /// called from QRegister's constructor exactly once.
    Interactive() = default;
    void bind_backend(SimulatorBackend* backend);
    /// Set once from QRegister's constructor. initialize() uses it to ask the owner
    /// for a CPU backend when ProcType::Auto resolved to GPU; only
    /// QRegister can do the swap because it owns the unique_ptr. Safe as a raw
    /// pointer: QRegister is neither copyable nor movable (copy deleted, and the
    /// user-declared destructor suppresses the implicit move).
    void bind_owner(QRegister* owner);

    QRegister*        owner_   = nullptr;     // Non-owning — the enclosing QRegister.
    SimulatorBackend* backend_ = nullptr;     // Non-owning — QRegister keeps the unique_ptr.
    bool              active_  = false;
    Result            accumulated_result_;
    /// Qubit count stored at initialize() time. Required by num_qubits() and
    /// measure_all(). Initialised to 0 (default-constructed, inactive).
    QubitIndex        num_qubits_ = 0;
    /// Classical bit accumulator indexed by ClassicalBitIndex cubit. Populated by
    /// measure(q, cubit); consumed by result() to build the MSB-first bitstring.
    /// clear_result() resets to all-zero (not resized — use push_back / assign).
    std::vector<int>  cbits_;

    friend class QRegister;
};

}  // namespace qplayer
