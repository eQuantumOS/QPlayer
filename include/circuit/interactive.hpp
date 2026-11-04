// interactive.hpp — Interactive-mode facade.
//
// IMPORTANT: This header is included by include/qregister.hpp only. User code
// must access the Interactive surface through QRegister::interactive.
// The private constructor + friend QRegister enforces this at compile time

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

class SimulatorBackend;  // Forward declaration — owned by QRegister.

class Interactive {
public:
    // ── State lifecycle ────────────────────────────────────────────
    // initialize: allocate the |0...0> state on the bound backend.
    //   Interactive supports CPU statevector backends only (FullState CPU,
    //   SparseState). FullState GPU and TensorNetwork throw SimulatorError
    // here — use the batch run() path for those.
    // reset() (no arg): re-prepare |0...0> on the same qubit count.
    // reset(q): collapse a single qubit to |0> while preserving the rest.
    void initialize(QubitIndex num_qubits);
    void reset();
    void reset(QubitIndex q);

    // ── Single-qubit gates (immediate dispatch, chainable) ─────────
    Interactive& h  (QubitIndex q);
    Interactive& x  (QubitIndex q);
    Interactive& y  (QubitIndex q);
    Interactive& z  (QubitIndex q);
    Interactive& s  (QubitIndex q);
    Interactive& sdg(QubitIndex q);
    Interactive& t  (QubitIndex q);
    Interactive& tdg(QubitIndex q);

    Interactive& rx(double theta,             QubitIndex q);
    Interactive& ry(double theta,             QubitIndex q);
    Interactive& rz(double theta,             QubitIndex q);
    Interactive& u1(double lam,               QubitIndex q);
    Interactive& u2(double phi, double lam,   QubitIndex q);
    Interactive& u3(double theta, double phi, double lam, QubitIndex q);

    // ── Two-qubit gates ────────────────────────────────────────────
    Interactive& cx  (QubitIndex ctrl, QubitIndex tgt);
    Interactive& cy  (QubitIndex ctrl, QubitIndex tgt);
    Interactive& cz  (QubitIndex ctrl, QubitIndex tgt);
    Interactive& swap(QubitIndex q0,   QubitIndex q1);

    // Google native 2Q gates. Immediate dispatch, same as other 2Q gates.
    // iswap/sqrt_iswap: no params. fsim(theta,phi): params forwarded to GateOp.
    // sycamore: fsim(pi/2, pi/6) convenience wrapper.
    Interactive& iswap     (QubitIndex q0, QubitIndex q1);
    Interactive& sqrt_iswap(QubitIndex q0, QubitIndex q1);
    Interactive& fsim      (double theta, double phi, QubitIndex q0, QubitIndex q1);
    Interactive& sycamore  (QubitIndex q0, QubitIndex q1);

    // ── Three-qubit gate ───────────────────────────────────────────
    Interactive& ccx(QubitIndex c0, QubitIndex c1, QubitIndex tgt);

    // ── Generic gate entry point ────────────────────────
    // Covers the full GateType enum so a parsed Circuit IR can be replayed
    // op-by-op (the typed methods above miss SX/CH/CRZ/CU1/CU3/CSwap/…).
    // GateType::Reset routes through the backend's apply_reset(). The typed
    // gate methods are thin wrappers over this.
    Interactive& apply_gate(const GateOp& op);

    // ── Barrier (no-op for execution, fusion boundary) ─────────────
    // No initializer_list overload — Interactive ops are immediate and we
    // never accumulate them into an IR list. Pass operand qubits one at a
    // time via repeated calls when needed.
    Interactive& barrier();

    // ── Measurement (immediate) ────────────────────────────────────
    // measure(q): return 0/1, no cubit write.
    // measure(q, cubit): accumulate into the bound Result (no value return).
    // measure_all: — index i carries qubit i's outcome (size = num_qubits()).
    int              measure   (QubitIndex q);
    void             measure   (QubitIndex q, ClassicalBitIndex cubit);
    std::vector<int> measure_all();

    // ── State access ─────────────────
    // Lifetime of RawStateView ends at the next apply_gate / measure / reset
    // call. SparseState throws SimulatorError on get_statevector_raw().
    StateVector  get_statevector()                              const;
    RawStateView get_statevector_raw()                          const;
    double       probability      (QubitIndex q, int outcome)   const;
    double       expectation_value(std::string_view pauli)      const;

    // Enumerate nonzero-amplitude basis states as (index, amplitude) pairs in
    // ascending StateIndex order without densifying. Requires an
    // active session (initialize() called); throws SimulatorError otherwise.
    // SparseState works up to 128 qubits; TensorNetwork always throws.
    std::vector<std::pair<StateIndex, std::complex<double>>>
            get_statevector_nonzero() const;

    // ── Accumulated measurement results ────────────────────────────
    // Accumulation pattern is measure*() + result() / clear_result()
    // Use QRegister::run for batched IR execution.
    const Result& result()       const;
    void          clear_result();

    // ── Utility ───────────────────────────────────────────────────
    QubitIndex num_qubits() const;
    bool       is_active()  const;

private:
    // Only QRegister constructs Interactive. bind_backend() is
    // called from QRegister's constructor exactly once.
    Interactive() = default;
    void bind_backend(SimulatorBackend* backend);

    SimulatorBackend* backend_ = nullptr;     // Non-owning — QRegister keeps the unique_ptr.
    bool              active_  = false;
    Result            accumulated_result_;
    // Qubit count stored at initialize() time. Required by num_qubits() and
    // measure_all(). Initialised to 0 (default-constructed, inactive).
    QubitIndex        num_qubits_ = 0;
    // Classical bit accumulator indexed by ClassicalBitIndex cubit. Populated by
    // measure(q, cubit); consumed by result() to build the MSB-first bitstring.
    // clear_result() resets to all-zero (not resized — use push_back / assign).
    std::vector<int>  cbits_;

    friend class QRegister;
};

}  // namespace qplayer
