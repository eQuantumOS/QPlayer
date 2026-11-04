"""PennyLane tape -> qplayer.Circuit converter + observable mapping.

Maps PennyLane operations to qplayer.Circuit gates directly (op direct mapping,
shared approach with). Only the gates with an exact qplayer
equivalent are declared "supported"; QPlayerDevice.preprocess decomposes anything
else into this set, so the converter never sees an unsupported op in practice.
Observables are mapped to weighted Pauli strings (qplayer big-endian).
"""
from __future__ import annotations

# Lazy import: reached only via ``import qplayer.pennylane`` (core purity).
import qplayer as _qp

# PennyLane op.name -> exact qplayer.Circuit gate. preprocess() decomposes the rest.
SUPPORTED_OPS = frozenset({
    "Identity", "Hadamard", "PauliX", "PauliY", "PauliZ", "S", "T", "SX",
    "RX", "RY", "RZ", "PhaseShift",
    "CNOT", "CZ", "CY", "SWAP", "Toffoli", "CSWAP", "CRZ", "ControlledPhaseShift",
})


def supported(op) -> bool:
    """Stopping condition for PennyLane's decompose transform."""
    return op.name in SUPPORTED_OPS


class ConversionError(ValueError):
    """Raised when a PennyLane op/observable has no qplayer equivalent."""


def _apply(circ, name, q, p):
    if name == "Identity": circ.id(q[0])
    elif name == "Hadamard": circ.h(q[0])
    elif name == "PauliX": circ.x(q[0])
    elif name == "PauliY": circ.y(q[0])
    elif name == "PauliZ": circ.z(q[0])
    elif name == "S": circ.s(q[0])
    elif name == "T": circ.t(q[0])
    elif name == "SX": circ.sx(q[0])
    elif name == "RX": circ.rx(p[0], q[0])
    elif name == "RY": circ.ry(p[0], q[0])
    elif name == "RZ": circ.rz(p[0], q[0])
    elif name == "PhaseShift": circ.u1(p[0], q[0])
    elif name == "CNOT": circ.cx(q[0], q[1])
    elif name == "CZ": circ.cz(q[0], q[1])
    elif name == "CY": circ.cy(q[0], q[1])
    elif name == "SWAP": circ.swap(q[0], q[1])
    elif name == "CRZ": circ.crz(p[0], q[0], q[1])
    elif name == "ControlledPhaseShift": circ.cu1(p[0], q[0], q[1])
    elif name == "Toffoli": circ.ccx(q[0], q[1], q[2])
    elif name == "CSWAP": circ.cswap(q[0], q[1], q[2])
    else:
        raise ConversionError(
            f"unsupported PennyLane op '{name}'; it should have been decomposed by "
            f"QPlayerDevice.preprocess into {sorted(SUPPORTED_OPS)}")


def to_qplayer_circuit(tape, wire_to_qubit, num_qubits, with_measure):
    """Build a qplayer.Circuit from a (decomposed) PennyLane tape.

    ``with_measure`` appends measure(i, i) on every qubit (for shot sampling);
    omit it for analytic state/expectation evaluation.
    """
    circ = _qp.Circuit(num_qubits, num_qubits)
    for op in tape.operations:
        q = [wire_to_qubit[w] for w in op.wires]
        _apply(circ, op.name, q, [float(x) for x in op.parameters])
    if with_measure:
        for i in range(num_qubits):
            circ.measure(i, i)
    return circ


def obs_to_pauli_terms(obs, wire_to_qubit, num_qubits):
    """Map a PennyLane observable to [(pauli_string, coeff)] in qplayer big-endian.

    Uses the observable's PauliSentence representation (uniform for a single Pauli,
    a tensor product, a Hamiltonian sum, or identity). pauli_string[0] is the
    highest qubit. The empty PauliWord maps to the all-"I" identity string.
    """
    import numpy as np
    sentence = obs.pauli_rep
    if sentence is None:
        raise ConversionError(f"observable {obs} has no Pauli representation")
    terms = []
    for word, coeff in sentence.items():
        chars = ["I"] * num_qubits
        for wire, pauli in word.items():
            chars[num_qubits - 1 - wire_to_qubit[wire]] = pauli
        terms.append(("".join(chars), float(np.real(coeff))))
    return terms
