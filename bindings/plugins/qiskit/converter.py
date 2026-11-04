"""QuantumCircuit -> qplayer.Circuit converter via direct op mapping.

Walks ``QuantumCircuit.data`` and maps each instruction's ``operation.name`` to a
qplayer.Circuit gate method directly — no QASM2 text round-trip — so measure /
classical-bit mapping is controlled explicitly and the bit ordering is preserved
(verified identical to Qiskit for a single classical register).
"""
from __future__ import annotations

# Lazy import: this module is only reached through ``import qplayer.qiskit``,
# never by the core ``import qplayer`` (core-purity guarantee).
import qplayer as _qp


class ConversionError(ValueError):
    """Raised when a Qiskit instruction has no qplayer.Circuit equivalent."""


def _idx_maps(qc):
    # find_bit(...).index gives the global (cross-register) bit index, which is
    # the index qplayer.Circuit uses for both qubits and cubits.
    qi = {b: qc.find_bit(b).index for b in qc.qubits}
    ci = {b: qc.find_bit(b).index for b in qc.clbits}
    return qi, ci


def to_qplayer_circuit(qc):
    """Convert a (transpiled) Qiskit ``QuantumCircuit`` to a ``qplayer.Circuit``.

    The circuit is expected to already be in QPlayer's gate set (call
    ``transpile(qc, backend=QPlayerBackend())`` first); any op outside the set
    raises ConversionError so the caller sees the gap explicitly.
    """
    circ = _qp.Circuit(qc.num_qubits, qc.num_clbits)
    qi, ci = _idx_maps(qc)

    for inst in qc.data:
        op = inst.operation
        name = op.name
        q = [qi[b] for b in inst.qubits]
        c = [ci[b] for b in inst.clbits]
        p = [float(x) for x in op.params]

        # 1-qubit fixed
        if name in ("id", "i"):
            circ.id(q[0])
        elif name == "h":
            circ.h(q[0])
        elif name == "x":
            circ.x(q[0])
        elif name == "y":
            circ.y(q[0])
        elif name == "z":
            circ.z(q[0])
        elif name == "s":
            circ.s(q[0])
        elif name == "sdg":
            circ.sdg(q[0])
        elif name == "t":
            circ.t(q[0])
        elif name == "tdg":
            circ.tdg(q[0])
        elif name == "sx":
            circ.sx(q[0])
        elif name == "sxdg":
            circ.sxdg(q[0])
        # 1-qubit parametric
        elif name == "rx":
            circ.rx(p[0], q[0])
        elif name == "ry":
            circ.ry(p[0], q[0])
        elif name == "rz":
            circ.rz(p[0], q[0])
        elif name in ("p", "u1", "phase"):  # PhaseGate == U1 == diag(1, e^{iλ})
            circ.u1(p[0], q[0])
        elif name == "u2":
            circ.u2(p[0], p[1], q[0])
        elif name in ("u", "u3"):  # UGate(θ,φ,λ) == U3
            circ.u3(p[0], p[1], p[2], q[0])
        # 2-qubit
        elif name in ("cx", "cnot"):
            circ.cx(q[0], q[1])
        elif name == "cy":
            circ.cy(q[0], q[1])
        elif name == "cz":
            circ.cz(q[0], q[1])
        elif name == "ch":
            circ.ch(q[0], q[1])
        elif name == "swap":
            circ.swap(q[0], q[1])
        elif name == "crz":
            circ.crz(p[0], q[0], q[1])
        elif name in ("cp", "cu1"):  # CPhaseGate == CU1
            circ.cu1(p[0], q[0], q[1])
        elif name == "cu3":
            circ.cu3(p[0], p[1], p[2], q[0], q[1])
        # 3-qubit
        elif name in ("ccx", "toffoli"):
            circ.ccx(q[0], q[1], q[2])
        elif name in ("cswap", "fredkin"):
            circ.cswap(q[0], q[1], q[2])
        # non-gate
        elif name == "measure":
            circ.measure(q[0], c[0])
        elif name == "reset":
            circ.reset(q[0])
        elif name == "barrier":
            # Pure scheduling/fusion boundary; QPlayer reconstructs its own
            # fusion boundaries, so a transpiled barrier is dropped here.
            pass
        else:
            raise ConversionError(
                f"unsupported Qiskit op '{name}'; transpile against "
                f"QPlayerBackend().target first to decompose into QPlayer's gate set")
    return circ
