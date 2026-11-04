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

Backend / processor selection follows run_qasm.py: --backend picks the
simulator and --proc the processor. Both parts are noiseless and small, so
every backend can run them.

Run:  python estimator_demo.py                      (after `pip install qplayer`,
      python estimator_demo.py --backend sparse      or with PYTHONPATH=bindings)
      python estimator_demo.py --backend tn --proc cpu
"""

import argparse
import math
import sys

import qplayer

_BACKENDS = ("fullstate", "sparse", "tn")
_PROCS = ("cpu", "gpu", "auto")

# Exit codes mirror run_qasm.py so shell callers can branch on the outcome.
EXIT_OK     = 0
EXIT_ERROR  = 1  # circuit / runtime failure
EXIT_USAGE  = 2  # bad command line (argparse also exits 2)
EXIT_UNSUPP = 3  # backend does not support a part

# Simulator / processor the parts run on; set by main() from the command line.
# Module scope keeps each part body focused on the primitive it demonstrates.
SIM_TYPE  = qplayer.SimType.FullState
PROC_TYPE = qplayer.ProcType.CPU


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="estimator_demo.py",
        description="Demonstrate the Qiskit-compatible Observable / Estimator / "
                    "Sampler primitives.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--backend", choices=_BACKENDS, default="fullstate",
                   help="fullstate | sparse | tn (default fullstate)")
    p.add_argument("--proc", choices=_PROCS, default="cpu",
                   help="cpu | gpu | auto processor (default cpu; gpu needs a "
                        "CUDA build, else warns + CPU fallback; auto picks GPU "
                        "when available). sparse is CPU-only; tn accepts an "
                        "explicit gpu to offload SVD")
    return p


def bell_energy() -> None:
    qr = qplayer.QRegister(SIM_TYPE, PROC_TYPE)
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
    qr = qplayer.QRegister(SIM_TYPE, PROC_TYPE)
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


def main() -> int:
    global SIM_TYPE, PROC_TYPE
    args = build_parser().parse_args()

    backend_map = {
        "fullstate": qplayer.SimType.FullState,
        "sparse":    qplayer.SimType.SparseState,
        "tn":        qplayer.SimType.TensorNetwork,
    }
    proc_map = {
        "cpu":  qplayer.ProcType.CPU,
        "gpu":  qplayer.ProcType.GPU,
        "auto": qplayer.ProcType.Auto,
    }
    SIM_TYPE  = backend_map[args.backend]
    PROC_TYPE = proc_map[args.proc]
    print(f"estimator demo: backend={args.backend}, proc={args.proc}\n")

    try:
        bell_energy()
        vqe_energy_sweep()
    except qplayer.NotImplementedError as e:
        print(f"unsupported: {e}", file=sys.stderr)
        print(f"(hint: backend '{args.backend}' is not available in this "
              f"build; try --backend fullstate)", file=sys.stderr)
        return EXIT_UNSUPP
    except qplayer.QPlayerError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_ERROR
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
