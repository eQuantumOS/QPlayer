#!/usr/bin/env python3
"""Generate fixed-parameter VQE and QAOA QASM circuits for the device-noise
VQE/QAOA comparison.

VQE/QAOA are variational; here a single representative parameter set is frozen
from a fixed seed and baked into the emitted QASM. The harness measures the noise
fidelity of a deep parametric circuit (FakeBackend vs QPlayer), not the
optimization itself, so a frozen (non-optimized) angle set is sufficient and keeps
the circuits static + reproducible without qiskit.

Circuits are device-exclusive by width; they are NOT shared across backends.
Output layout mirrors that rule via subdirs the harness maps per device:

  qasm/manila/vqe_5q.qasm     qasm/manila/qaoa_5q.qasm     (fills the 5q device)
  qasm/oslo/vqe_7q.qasm       qasm/oslo/qaoa_7q.qasm       (fills the 7q device)
  qasm/geneva/vqe_12q.qasm    qasm/geneva/qaoa_12q.qasm    (partial-width, see below)

manila/oslo ansatze fill the whole device. geneva is 27q, and a full-width 27q
per-shot Monte-Carlo noisy simulation is infeasible (~2 GB/statevector x shots),
so geneva instead gets a PARTIAL-width (GENEVA_QUBITS) ansatz: it exercises a
connected chain of geneva's heavy-hex qubits, transpiles onto the device, and
QubitTruncationPass compresses it to just those active qubits so the
distribution tier runs (2^k, k <= --max-sim-qubits). GENEVA_QUBITS is kept at/below
that simulation cap so the tier is never skipped.

Run: python gen_ansatz_qasm.py
Regeneration is deterministic; the committed circuits are the source of truth.
"""
import math
import os

SEED = 1234
REPS = 2   # VQE RealAmplitudes-style repetitions (2 entangling layers)
P = 1      # QAOA layers (standard shallow hardware depth)
# geneva ansatz width: partial-width (< 27q device) so that after transpile +
# QubitTruncationPass the active-qubit count stays <= the harness
# --max-sim-qubits cap (default 14) and the distribution tier runs.
GENEVA_QUBITS = 12
# QAOA angles are FIXED (not seeded): a random beta near 0 collapses the mixer and
# the distribution degenerates to uniform (cost phases do not change computational-
# basis probabilities on their own). This representative moderate pair puts clear
# peaks on the alternating MaxCut solutions, giving noise something to degrade.
QAOA_GAMMA = 0.6
QAOA_BETA = 0.4


# Minimal deterministic LCG so the emitted angles do not depend on numpy version.
# Reproducible from SEED alone, no external dependency.
class _Rng:
    def __init__(self, seed):
        self._s = seed & 0xFFFFFFFFFFFFFFFF

    def uniform(self, lo, hi):
        self._s = (6364136223846793005 * self._s + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        frac = (self._s >> 11) / float(1 << 53)
        return lo + (hi - lo) * frac


def vqe_qasm(n, rng):
    lines = [
        f"// VQE ansatz (RealAmplitudes-style, reps={REPS}): an RY rotation layer",
        f"// followed by reps x (linear CX entangler + RY layer), {n} qubits. Angles",
        f"// are frozen from seed {SEED} (a representative point, not an optimized",
        "// minimum). The two entangling layers accumulate relaxation + 2q gate error",
        "// so the measured distribution departs visibly from the ideal one.",
        "OPENQASM 2.0;",
        'include "qelib1.inc";',
        f"qreg q[{n}];",
        f"creg c[{n}];",
    ]

    def ry_layer():
        for i in range(n):
            lines.append(f"ry({rng.uniform(0.0, 2.0 * math.pi):.6f}) q[{i}];")

    def cx_chain():
        for i in range(n - 1):
            lines.append(f"cx q[{i}],q[{i + 1}];")

    ry_layer()
    for _ in range(REPS):
        cx_chain()
        ry_layer()
    for i in range(n):
        lines.append(f"measure q[{i}] -> c[{i}];")
    return "\n".join(lines) + "\n"


def qaoa_qasm(n, gamma, beta):
    lines = [
        f"// QAOA MaxCut on a line graph (edges 0-1..{n - 2}-{n - 1}), p={P}: Hadamard",
        f"// layer, ZZ cost phase per edge (cx-rz-cx, rz angle -2*gamma so the cost",
        f"// rewards cuts), then an RX mixer. {n} qubits; gamma={gamma}, beta={beta}",
        "// (fixed representative angles). The ideal run peaks on the alternating MaxCut",
        "// solutions; relaxation + gate error + readout spread weight off those peaks.",
        "OPENQASM 2.0;",
        'include "qelib1.inc";',
        f"qreg q[{n}];",
        f"creg c[{n}];",
    ]
    for i in range(n):
        lines.append(f"h q[{i}];")
    for i in range(n - 1):  # cost layer: exp(-i*gamma*C), peaks on max-cut bitstrings
        lines.append(f"cx q[{i}],q[{i + 1}];")
        lines.append(f"rz({-2.0 * gamma:.6f}) q[{i + 1}];")
        lines.append(f"cx q[{i}],q[{i + 1}];")
    for i in range(n):  # mixer: exp(-i*beta*X_i)
        lines.append(f"rx({2.0 * beta:.6f}) q[{i}];")
    for i in range(n):
        lines.append(f"measure q[{i}] -> c[{i}];")
    return "\n".join(lines) + "\n"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    qasm = os.path.join(here, "qasm")
    os.makedirs(os.path.join(qasm, "manila"), exist_ok=True)
    os.makedirs(os.path.join(qasm, "oslo"), exist_ok=True)
    os.makedirs(os.path.join(qasm, "geneva"), exist_ok=True)

    # VQE: independent seed per circuit so regenerating one does not shift others.
    # QAOA: fixed angles (see QAOA_GAMMA/QAOA_BETA), no seed.
    g = GENEVA_QUBITS
    writes = [
        (os.path.join(qasm, "manila", "vqe_5q.qasm"), vqe_qasm(5, _Rng(SEED))),
        (os.path.join(qasm, "oslo", "vqe_7q.qasm"), vqe_qasm(7, _Rng(SEED + 1))),
        (os.path.join(qasm, "geneva", f"vqe_{g}q.qasm"), vqe_qasm(g, _Rng(SEED + 2))),
        (os.path.join(qasm, "manila", "qaoa_5q.qasm"), qaoa_qasm(5, QAOA_GAMMA, QAOA_BETA)),
        (os.path.join(qasm, "oslo", "qaoa_7q.qasm"), qaoa_qasm(7, QAOA_GAMMA, QAOA_BETA)),
        (os.path.join(qasm, "geneva", f"qaoa_{g}q.qasm"), qaoa_qasm(g, QAOA_GAMMA, QAOA_BETA)),
    ]
    for path, text in writes:
        with open(path, "w") as f:
            f.write(text)
        print(f"wrote {os.path.relpath(path, here)}")


if __name__ == "__main__":
    main()
