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

Run:  python python_demo.py        (after `pip install qplayer`,
      or with PYTHONPATH=bindings)
"""

import math

import qplayer

proc = qplayer.ProcType.CPU # CPU, GPU

def part1_bell() -> None:
    """Build H(0)·CX(0,1), measure both qubits, sample the distribution."""
    qr = qplayer.QRegister(qplayer.SimType.FullState, proc)
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
    qr = qplayer.QRegister(qplayer.SimType.FullState, proc)
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
    qr = qplayer.QRegister(qplayer.SimType.FullState, proc)
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
    qr = qplayer.QRegister(qplayer.SimType.FullState)

    # Bell prep only — no measure ops, so the statevector is preserved for
    # expectation_value() (exact, no shot noise; Pauli order is big-endian).
    circ = qplayer.Circuit(2, 2)
    circ.h(0).cx(0, 1)
    qr.run(circ, 1)

    print("Part 4 — expectation values on the Bell state (exact)")
    print(f"  <ZZ> = {qr.expectation_value('ZZ'):+.6f}  (ideal +1)")
    print(f"  <XX> = {qr.expectation_value('XX'):+.6f}  (ideal +1)")
    print(f"  <ZI> = {qr.expectation_value('ZI'):+.6f}  (ideal  0)")


def main() -> None:
    part1_bell()
    part2_ghz()
    part3_rotation()
    part4_expectation()


if __name__ == "__main__":
    main()
