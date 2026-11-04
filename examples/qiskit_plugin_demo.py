#!/usr/bin/env python3
"""QPlayer as a Qiskit backend — plugin demo (Phase 7-J).

The circuits and workflow stay 100% pure Qiskit; only the backend object is
QPlayer. Three parts:

  Part 1 — transpile(qc, backend) + backend.run().result().get_counts() (Bell).
  Part 2 — BackendSamplerV2(backend=...) sampling (primitive auto-provided).
  Part 3 — BackendEstimatorV2(backend=...) VQE-style <H> on a Ry ansatz
           (H = ZI + IZ + 0.5 XX, analytic 1.375; shot-based estimate).

Requires the optional extra:  pip install qplayer[qiskit]   (qiskit >= 1.1, 1.x or 2.x)
Run:  python qiskit_plugin_demo.py
"""

import math

from qiskit import QuantumCircuit, transpile
from qiskit.primitives import BackendEstimatorV2, BackendSamplerV2
from qiskit.quantum_info import SparsePauliOp

from qplayer.qiskit import QPlayerProvider


def part1_backend_run() -> None:
    backend = QPlayerProvider().get_backend()  # proc="gpu" to use the GPU backend
    qc = QuantumCircuit(2, 2)
    qc.h(0)
    qc.cx(0, 1)
    qc.measure([0, 1], [0, 1])

    tqc = transpile(qc, backend=backend, seed_transpiler=1)
    counts = backend.run(tqc, shots=4096, seed_simulator=1).result().get_counts()
    print("Part 1 — Bell counts:", counts)


def part2_sampler() -> None:
    backend = QPlayerProvider().get_backend()
    qc = QuantumCircuit(3, 3)
    qc.h(0)
    qc.cx(0, 1)
    qc.cx(1, 2)
    qc.measure(range(3), range(3))

    sampler = BackendSamplerV2(backend=backend)
    sampler.options.seed_simulator = 1
    counts = sampler.run([qc], shots=4096).result()[0].data.c.get_counts()
    print("Part 2 — GHZ sampler counts:", counts)


def part3_estimator() -> None:
    backend = QPlayerProvider().get_backend()
    ansatz = QuantumCircuit(2)
    ansatz.ry(math.pi / 3, 0)
    ansatz.ry(math.pi / 3, 1)
    hamiltonian = SparsePauliOp.from_list([("ZI", 1.0), ("IZ", 1.0), ("XX", 0.5)])

    estimator = BackendEstimatorV2(backend=backend)
    estimator.options.seed_simulator = 1
    energy = estimator.run([(ansatz, hamiltonian)], precision=0.005).result()[0].data.evs
    print(f"Part 3 — <H> = {float(energy):.4f}  (analytic 1.375)")


if __name__ == "__main__":
    part1_backend_run()
    part2_sampler()
    part3_estimator()
