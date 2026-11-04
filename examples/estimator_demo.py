#!/usr/bin/env python3
"""Estimator / Observable example (Phase 7-I).

Demonstrates the Qiskit-compatible primitives:
  * Observable — a weighted Pauli sum H = Sum(c_i * P_i) (real coeffs, Hermitian).
  * Estimator  — exact <H> via the statevector (no shot noise, no basis-rotation
                 circuits — QPlayer is a simulator).
  * Sampler    — the measurement distribution as a {bitstring: count} dict.

Part 1 prints the Bell-state energy of 0.5*ZZ + 0.5*XX (analytically 1.0).
Part 2 sweeps a 2-qubit product Ry(t) ansatz and prints <ZI+IZ> = 2*cos(t),
a miniature VQE-style energy curve.

Pauli qubit order is Qiskit big-endian: pauli[0] is the highest qubit.

Run:  python estimator_demo.py        (after `pip install qplayer`,
      or with PYTHONPATH=bindings)
"""

import math

import qplayer


def bell_energy() -> None:
    qr = qplayer.QRegister(qplayer.SimType.FullState)
    estimator = qplayer.Estimator(qr)

    # |Phi+> = (|00> + |11>)/sqrt(2) — state prep only, no measurement.
    bell = qplayer.Circuit(2, 2)
    bell.h(0).cx(0, 1)

    obs = qplayer.Observable.from_list([("ZZ", 0.5), ("XX", 0.5)])
    energy = estimator.run(bell, obs)
    print(f"Bell <0.5*ZZ + 0.5*XX> = {energy:.6f}  (exact 1.0)")

    # Sampler on the measured Bell circuit — distribution view of the same state.
    sampler = qplayer.Sampler(qr)
    measured = qplayer.Circuit(2, 2)
    measured.h(0).cx(0, 1).measure(0, 0).measure(1, 1)
    counts = sampler.run(measured, 10000)
    print(f"Bell Sampler counts          = {dict(counts)}")


def vqe_energy_sweep() -> None:
    qr = qplayer.QRegister(qplayer.SimType.FullState)
    estimator = qplayer.Estimator(qr)

    # H = ZI + IZ. For a product Ry(t) ansatz, <ZI> = <IZ> = cos(t).
    h = qplayer.Observable.from_list([("ZI", 1.0), ("IZ", 1.0)])

    print("\n  t (rad)   <H>      2*cos(t)")
    for k in range(7):
        t = k * math.pi / 6.0
        ansatz = qplayer.Circuit(2, 2)
        ansatz.ry(t, 0).ry(t, 1)
        energy = estimator.run(ansatz, h)
        print(f"  {t:6.3f}   {energy:+.4f}   {2.0 * math.cos(t):+.4f}")


def main() -> None:
    bell_energy()
    vqe_energy_sweep()


if __name__ == "__main__":
    main()
