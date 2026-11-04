#!/usr/bin/env python3
"""QPlayer as a PennyLane device — plugin demo (Phase 7-K).

The QNodes are pure PennyLane; only the device is QPlayer. Three parts:

  Part 1 — Bell sampling via qml.counts().
  Part 2 — VQE-style <H> with H = Z0 + Z1 + 0.5 X0X1 on a Ry ansatz (analytic 1.375).
  Part 3 — parameter-shift gradient of <Z0> w.r.t. an RY angle.

Requires the optional extra:  pip install qplayer[pennylane]   (pennylane >= 0.35)
Run:  python pennylane_plugin_demo.py
"""

import math

import pennylane as qml

# After `pip install qplayer[pennylane]` the device is discoverable by name:
#   dev = qml.device("qplayer.qubit", wires=2)
# Importing the class directly works without the entry point being registered:
from qplayer.pennylane import QPlayerDevice


def part1_counts() -> None:
    dev = QPlayerDevice(wires=2, shots=4096, seed=1)  # or qml.device("qplayer.qubit", ...)

    @qml.qnode(dev)
    def bell():
        qml.Hadamard(0)
        qml.CNOT([0, 1])
        return qml.counts()

    print("Part 1 — Bell counts:", bell())


def part2_vqe_energy() -> None:
    dev = QPlayerDevice(wires=2)  # analytic (shots=None)
    hamiltonian = qml.Z(0) + qml.Z(1) + 0.5 * qml.X(0) @ qml.X(1)

    @qml.qnode(dev)
    def energy(theta):
        qml.RY(theta, 0)
        qml.RY(theta, 1)
        return qml.expval(hamiltonian)

    print(f"Part 2 — <H> at theta=pi/3: {float(energy(math.pi / 3)):.6f}  (analytic 1.375)")


def part3_gradient() -> None:
    dev = QPlayerDevice(wires=1)

    @qml.qnode(dev, diff_method="parameter-shift")
    def expz(x):
        qml.RY(x, 0)
        return qml.expval(qml.Z(0))

    x = qml.numpy.array(0.5, requires_grad=True)
    grad = qml.grad(expz)(x)
    print(f"Part 3 — d<Z>/dx at 0.5: {float(grad):.6f}  (analytic -sin(0.5)={-math.sin(0.5):.6f})")


if __name__ == "__main__":
    part1_counts()
    part2_vqe_energy()
    part3_gradient()
