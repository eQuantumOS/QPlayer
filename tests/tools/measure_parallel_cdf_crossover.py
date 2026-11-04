#!/usr/bin/env python3
"""Measure serial vs parallel CDF crossover for perform_lazy_sampling().

Because kParallelSampleThreshold is a process-level static (resolved once from
QPLAYER_PARALLEL_SAMPLE_THRESHOLD), each (n, threshold) combination must run
in a fresh subprocess so the env var is read at static-init time.

For each qubit count n in the sweep, the circuit is all-H + measure_all (uniform
superposition) so the CDF spans the full range with no degenerate sparsity.

Timing:
  - execute() is excluded.  counts() latency is the sample time.
  - warm-up: 1 run discarded; then 5 runs, median taken.
  - subprocess overhead (Python start + import) is identical for both conditions
    and cancels out in the serial vs parallel comparison.

Two conditions per n:
  serial — threshold = 2^40 (effectively infinity -> always serial)
  parallel — threshold = 0 (below any dim -> always parallel)

Usage (from repo root):
    PYTHONPATH=bindings python3 \
        tests/tools/measure_parallel_cdf_crossover.py
"""

import os
import sys
import subprocess
import json
import statistics

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
PYTHON_PATH = os.path.join(REPO_ROOT, 'bindings', 'python')

NSHOTS = 1024
SEED   = 20260616
N_REPS = 5  # median over 5 runs after 1 warm-up

NS = [16, 17, 18, 19, 20, 21, 22, 24, 26]

FORCE_SERIAL   = str(1 << 40)   # effectively infinity -> always serial
FORCE_PARALLEL = "2048"          # >1024 guard, below any measured dim -> always parallel

# Inline worker script run in each subprocess.
WORKER_SCRIPT = r"""
import sys, os, time, statistics, json

sys.path.insert(0, "{python_path}")
import qplayer
from qplayer import QRegister, Circuit, SimType, ProcType

n      = int(sys.argv[1])
nshots = int(sys.argv[2])
seed   = int(sys.argv[3])
n_reps = int(sys.argv[4])

def build_circuit(n):
    circ = Circuit(n, n)
    for q in range(n):
        circ.h(q)
    circ.measure_all()
    return circ

circ = build_circuit(n)
times_ms = []

for rep in range(n_reps + 1):
    qr = QRegister(SimType.FullState, ProcType.CPU)
    qr.set_seed(seed)
    result = qr.run(circ, nshots)

    t0 = time.perf_counter()
    _ = result.counts()
    t1 = time.perf_counter()

    if rep > 0:
        times_ms.append((t1 - t0) * 1000.0)

print(json.dumps({{"median_ms": statistics.median(times_ms), "all_ms": times_ms}}))
""".format(python_path=PYTHON_PATH)


def measure_counts_ms(n: int, threshold_str: str) -> float:
    """Run a fresh subprocess with the given threshold and return median counts() ms."""
    env = os.environ.copy()
    env["QPLAYER_PARALLEL_SAMPLE_THRESHOLD"] = threshold_str
    env["PYTHONPATH"] = PYTHON_PATH

    proc = subprocess.run(
        [sys.executable, "-c", WORKER_SCRIPT, str(n), str(NSHOTS), str(SEED), str(N_REPS)],
        capture_output=True, text=True, env=env, timeout=120
    )
    if proc.returncode != 0:
        print(f"  [ERROR n={n} thr={threshold_str}] stderr: {proc.stderr[:500]}", file=sys.stderr)
        return float('nan')
    try:
        result = json.loads(proc.stdout.strip())
        return result["median_ms"]
    except Exception as e:
        print(f"  [PARSE ERROR n={n}] stdout={proc.stdout!r} err={e}", file=sys.stderr)
        return float('nan')


def main() -> None:
    print(f"\nParallel CDF crossover sweep — nshots={NSHOTS}, seed={SEED}, reps={N_REPS} (median, subprocess isolation)")
    print(f"\n{'n':>3}  {'states':>10}  {'serial_ms':>10}  {'parallel_ms':>12}  {'winner':>8}  {'ratio':>8}")
    print("-" * 68)

    rows = []
    for n in NS:
        dim = 1 << n
        sys.stdout.write(f"  measuring n={n} (dim={dim:,})...\r")
        sys.stdout.flush()
        serial_ms   = measure_counts_ms(n, FORCE_SERIAL)
        parallel_ms = measure_counts_ms(n, FORCE_PARALLEL)

        if parallel_ms < serial_ms:
            winner  = "parallel"
            ratio   = serial_ms / parallel_ms
            ratio_s = f"{ratio:.2f}x"
        else:
            winner  = "serial"
            ratio   = parallel_ms / serial_ms
            ratio_s = f"{ratio:.2f}x"

        print(f"{n:>3}  {dim:>10,}  {serial_ms:>10.2f}  {parallel_ms:>12.2f}  {winner:>8}  {ratio_s:>8}")
        rows.append((n, dim, serial_ms, parallel_ms, winner, ratio))

    # Find first n where parallel wins.
    crossover_n = None
    for n, dim, s, p, winner, _ in rows:
        if winner == "parallel":
            crossover_n = n
            break

    # Find first n where parallel wins *and stays winning* for all subsequent ns.
    stable_crossover_n = None
    for i, (n, dim, s, p, winner, _) in enumerate(rows):
        if winner == "parallel" and all(r[4] == "parallel" for r in rows[i:]):
            stable_crossover_n = n
            break

    print()
    print(f"First crossover (parallel beats serial once): n={crossover_n}")
    print(f"Stable crossover (parallel wins from here on): n={stable_crossover_n}")

    # Choose conservative default: first stable crossover, one step up for safety.
    chosen_n   = stable_crossover_n if stable_crossover_n is not None else None
    if chosen_n is not None:
        # Threshold = 2^chosen_n means parallel activates for dim > 2^chosen_n.
        # Use 2^(chosen_n - 1) as threshold so parallel activates AT chosen_n.
        # But must be > 1024 (Golden Clifford 10q).
        threshold = max(1 << (chosen_n - 1), 1 << 10)
        threshold_pow = chosen_n - 1
        print(f"\nDecision: first stable parallel win at n={chosen_n} (dim={1<<chosen_n:,})")
        print(f"  Conservative default threshold = 2^{threshold_pow} = {threshold:,}")
        print(f"  (parallel activates for dim > {threshold:,}, i.e. n > {threshold_pow})")
        print(f"  Must be > 1024 (Golden Clifford): {threshold} > 1024 -> {'OK' if threshold > 1024 else 'FAIL'}")
    else:
        print("\nNo stable crossover found — recommend keeping high threshold (serial-only).")


if __name__ == "__main__":
    main()
