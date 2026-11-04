#!/usr/bin/env python3
"""python_demo.py — build circuits with native gate calls, run, and sample.

The companion run_qasm.py drives an OpenQASM v2 *file* through the Batch API.
This demo instead builds every circuit **in code** with the native Circuit
builder (method-chained gate calls like circ.h(0).cx(0, 1)), runs it on the
FullState CPU backend, and reads the measurement counts back — no QASM text
involved.

Four self-contained parts:
  Part 1 — Bell state            h + cx, measure per qubit -> counts.
  Part 2 — GHZ(5) state          chained gates + measure_all().
  Part 3 — biased single qubit   RY(theta) -> tunable sampling distribution.
  Part 4 — expectation value     run a prep circuit (no measure), read <Z>/<ZZ>.

Backend / processor selection follows run_qasm.py: --backend picks the
simulator and --proc the processor. All four parts are noiseless and small, so
every backend can run them.

Run:  python python_demo.py                      (after `pip install qplayer`,
      python python_demo.py --backend sparse      or with PYTHONPATH=bindings)
      python python_demo.py --backend tn --proc cpu
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
# Module scope keeps each part body focused on the API surface it demonstrates.
SIM_TYPE  = qplayer.SimType.FullState
PROC_TYPE = qplayer.ProcType.CPU


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python_demo.py",
        description="Build circuits in code with the native Circuit builder, "
                    "run them, and read counts / expectation values.",
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


def part1_bell() -> None:
    """Build H(0)·CX(0,1), measure both qubits, sample the distribution."""
    qr = qplayer.QRegister(SIM_TYPE, PROC_TYPE)
    qr.set_seed(1234)  # reproducible sampling

    # Circuit(num_qubits, num_cubits); gate calls return the circuit for chaining.
    circ = qplayer.Circuit(2, 2)
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1)

    shots = 10000
    counts = qr.run(circ, shots).counts()
    print("Part 1 — Bell  H(0)·CX(0,1)")
    print(f"  counts: {dict(counts)}")
    print(f"  P(00)={counts.get('00', 0) / shots:.3f}  "
          f"P(11)={counts.get('11', 0) / shots:.3f}  (ideal 0.5 / 0.5)\n")


def part2_ghz(n: int = 5) -> None:
    """Build an n-qubit GHZ state by chaining gates, then measure every qubit."""
    qr = qplayer.QRegister(SIM_TYPE, PROC_TYPE)
    qr.set_seed(1234)

    circ = qplayer.Circuit(n, n)
    circ.h(0)
    for q in range(n - 1):
        circ.cx(q, q + 1)
    circ.measure_all()  # measure qubit i -> cubit i for all qubits

    shots = 10000
    counts = qr.run(circ, shots).counts()
    zeros, ones = "0" * n, "1" * n
    print(f"Part 2 — GHZ({n})  H(0) then CX chain")
    print(f"  counts: {dict(counts)}")
    print(f"  P({zeros})={counts.get(zeros, 0) / shots:.3f}  "
          f"P({ones})={counts.get(ones, 0) / shots:.3f}  (ideal 0.5 / 0.5)\n")


def part3_rotation(theta: float = math.pi / 3) -> None:
    """RY(theta) tilts a single qubit; the sampling split tracks cos^2/sin^2."""
    qr = qplayer.QRegister(SIM_TYPE, PROC_TYPE)
    qr.set_seed(1234)

    circ = qplayer.Circuit(1, 1)
    circ.ry(theta, 0).measure(0, 0)

    shots = 10000
    counts = qr.run(circ, shots).counts()
    print(f"Part 3 — RY(theta={theta:.4f}) on one qubit")
    print(f"  counts: {dict(counts)}")
    print(f"  P(0)={counts.get('0', 0) / shots:.3f} (ideal {math.cos(theta / 2) ** 2:.3f})  "
          f"P(1)={counts.get('1', 0) / shots:.3f} (ideal {math.sin(theta / 2) ** 2:.3f})\n")


def part4_expectation() -> None:
    """Run a state-prep circuit (no measurement) and read exact expectation values."""
    qr = qplayer.QRegister(SIM_TYPE, PROC_TYPE)

    # Bell prep only — no measure ops, so the statevector is preserved for
    # expectation_value() (exact, no shot noise; Pauli order is big-endian).
    circ = qplayer.Circuit(2, 2)
    circ.h(0).cx(0, 1)
    qr.run(circ, 1)

    print("Part 4 — expectation values on the Bell state (exact)")
    print(f"  <ZZ> = {qr.expectation_value('ZZ'):+.6f}  (ideal +1)")
    print(f"  <XX> = {qr.expectation_value('XX'):+.6f}  (ideal +1)")
    print(f"  <ZI> = {qr.expectation_value('ZI'):+.6f}  (ideal  0)")


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
    print(f"python demo: backend={args.backend}, proc={args.proc}\n")

    try:
        part1_bell()
        part2_ghz()
        part3_rotation()
        part4_expectation()
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
