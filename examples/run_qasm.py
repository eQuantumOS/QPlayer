#!/usr/bin/env python3
"""run_qasm.py — Python twin of run_qasm.cpp (Phase 7 bindings +).

Drives an OpenQASM v2 file through the QRegister Batch API:
parse -> optional transpile -> run -> inspect. It exercises the public Python
surface (counts, statevector dump, expectation values, seed, gate fusion,
OpenMP threads, backend selection) so the file doubles as a usage reference and
a quick hands-on runner, matching the C++ example's behaviour and output.

 (Interactive early expose): --interactive replays the circuit op-by-op
via qr.interactive — immediate gate apply + measurement collapse, single
trajectory, no sampling. Rejects IfOp / IdleOp and the tn backend, mirroring
run_qasm.cpp run_interactive() exactly.

Verification here is intentionally lightweight: it only checks that the
measurement counts sum to the shot count and that a recoverable statevector
stays normalised. Golden / expected-value comparison lives in the test suite,
not in this example.

Bindings notes (vs run_qasm.cpp): the Python binding exposes no set_omp_threads
or dump_statevector, so --threads is applied via the OMP_NUM_THREADS environment
variable BEFORE qplayer is imported (OpenMP reads it at library load), and
--statevector calls qr.get_statevector_nonzero() to emit the same
layout as the C++ dump_statevector() (nonzero states only, trailing header)
without the 25-qubit densify cap — SparseState circuits up to 128 qubits are
supported. Note this is not the acceptance oracle .data layout, which is
header-first, dense over all 2^n states, and uses %+.12e.

Circuit diagram: --draw / --draw-ascii print a text diagram
via Circuit.draw(); --draw-image PATH saves a matplotlib image via
Circuit.draw(output="mpl"), which needs the optional 'qplayer[viz]' dependency.

Run:  python run_qasm.py tests/data/qasm/bell.qasm --shots 1000 --seed 1
      python run_qasm.py tests/data/qasm/bell.qasm --interactive --seed 1
      python run_qasm.py tests/data/qasm/bell.qasm --draw
      python run_qasm.py tests/data/qasm/bell.qasm --draw-image bell.png
      (after `pip install qplayer`, or with PYTHONPATH=bindings)
"""

import argparse
import os
import sys
import time

import numpy as np  # bindings return statevectors as numpy arrays; used for the norm check

# Exit codes mirror run_qasm.cpp so shell callers can branch on the outcome.
EXIT_OK     = 0
EXIT_ERROR  = 1  # parse / circuit / verify failure
EXIT_USAGE  = 2  # bad command line (argparse also exits 2)
EXIT_UNSUPP = 3  # NotImplementedError (backend / feature)

_BACKENDS = ("fullstate", "sparse", "tn")
_PROCS = ("cpu", "gpu", "auto")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="run_qasm.py",
        description="Run an OpenQASM v2 circuit through the QRegister Batch API.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("qasm_path", metavar="file.qasm",
                   help="OpenQASM v2 source file")
    p.add_argument("--backend", choices=_BACKENDS, default="fullstate",
                   help="fullstate | sparse | tn (default fullstate)")
    p.add_argument("--proc", choices=_PROCS, default="cpu",
                   help="cpu | gpu | auto processor for FullState (default cpu; "
                        "gpu needs a CUDA build, else warns + CPU fallback; auto picks "
                        "GPU when available. sparse/tn are CPU-only and ignore this. "
                        "--interactive forces cpu)")
    p.add_argument("--shots", type=int, default=1024,
                   help="measurement shots (default 1024)")
    p.add_argument("--seed", type=int, default=None,
                   help="PRNG seed for reproducible sampling")
    # Presence of --opt-level enables transpile (matches run_qasm.cpp).
    p.add_argument("--opt-level", type=int, default=None, dest="opt_level",
                   help="transpile optimization level 0..3 (enables transpile)")
    p.add_argument("--statevector", action="store_true",
                   help="print the final statevector (oracle .data format)")
    p.add_argument("--draw", action="store_true",
                   help="print the circuit as a text diagram (Unicode)")
    p.add_argument("--draw-ascii", action="store_true", dest="draw_ascii",
                   help="print the circuit diagram in ASCII style")
    p.add_argument("--draw-image", default=None, metavar="PATH", dest="draw_image",
                   help="save a matplotlib circuit image to PATH "
                        "(requires the optional 'qplayer[viz]' / matplotlib dependency)")
    p.add_argument("--expect", action="append", default=[], metavar="PAULI",
                   help="print <PAULI> expectation (repeatable, e.g. --expect ZZ)")
    p.add_argument("--no-fusion", action="store_true",
                   help="disable runtime gate fusion")
    p.add_argument("--fusion-width", type=int, default=4, dest="fusion_width",
                   help="gate fusion max width (default 4)")
    p.add_argument("--threads", type=int, default=None,
                   help="OpenMP thread count (applied via OMP_NUM_THREADS)")
    p.add_argument("--interactive", action="store_true",
                   help="replay the circuit op-by-op via qr.interactive "
                        "(immediate gate apply + measurement collapse, single "
                        "trajectory, no sampling). Rejects if/conditional ops "
                        "and the tn backend")
    return p


def format_elapsed(elapsed_ns: float) -> str:
    """Auto-scaled elapsed-time string matching QTimer::getTime() (qtimer.hpp)."""
    us = elapsed_ns / 1.0e3
    ms = elapsed_ns / 1.0e6
    sec = elapsed_ns / 1.0e9
    if sec >= 1.0:
        total = int(sec)
        return f"{total // 3600:02d}:{(total % 3600) // 60:02d}:{total % 60:02d}"
    if ms >= 1.0:
        return f"{ms:.1f} ms"
    if us >= 1.0:
        return f"{us:.1f} us"
    return f"{elapsed_ns:.1f} ns"


def dump_statevector(qr, num_qubits: int) -> None:
    """Print nonzero-amplitude states matching QRegister::dump_statevector() format.

    Uses qr.get_statevector_nonzero() instead of get_statevector() so
    SparseState circuits up to 128 qubits are handled without the 25-qubit
    densify cap. Output format per line:
        [P=%.6f] [%+.8f, %+.8f] |<bits>>
    followed by a trailing header line reporting the emitted state count.
    The basis index is a Python arbitrary-precision int, so format() handles
    128-qubit values losslessly.
    """
    entries = qr.get_statevector_nonzero()
    for idx, amp in entries:
        re, im = amp.real, amp.imag
        p = re * re + im * im
        # MSB->LSB binary string of width num_qubits (Python int handles 128 bits).
        bits = format(idx, f"0{num_qubits}b")
        print(f"[P={p:.6f}] [{re:+.8f}, {im:+.8f}] |{bits}>")
    # Trailing header matches QRegister::dump_statevector() output.
    print(f"============== dump {len(entries)} states =============")


def maybe_draw(circ, args) -> None:
    """Render the circuit diagram per --draw / --draw-ascii / --draw-image.

    Text output uses Circuit.draw(); the image path uses draw(output="mpl"), which
    needs matplotlib (optional 'qplayer[viz]' dependency). A missing matplotlib is
    reported as a hint and is non-fatal so the rest of the run continues.
    """
    if args.draw or args.draw_ascii:
        style = "ascii" if args.draw_ascii else "unicode"
        print("diagram:")
        print(circ.draw(style=style), end="")
    if args.draw_image:
        try:
            circ.draw(output="mpl", filename=args.draw_image)
            print(f"diagram image: saved {args.draw_image}")
        except ImportError as exc:
            print(f"warning: --draw-image needs matplotlib: {exc}", file=sys.stderr)


def run_interactive(qr, circ, args, qplayer) -> int:
    """Replay a parsed circuit op-by-op through the Interactive facade.

    Gates apply immediately and measurements collapse the state on the spot.
    This is a single trajectory, not a sampled distribution. Mirrors
    run_interactive() in run_qasm.cpp exactly.
    """
    qr.interactive.initialize(circ.num_qubits())

    # Walk the raw IR. The Op variant has five alternatives; interactive mode
    # supports gates / measurements / barriers and rejects IfOp + IdleOp.
    for op in circ.ops():
        if isinstance(op, qplayer.GateOp):
            qr.interactive.apply_gate(op)
        elif isinstance(op, qplayer.MeasureOp):
            qr.interactive.measure(op.qubit, op.cubit)
        elif isinstance(op, qplayer.BarrierOp):
            qr.interactive.barrier()
        elif isinstance(op, qplayer.IfOp):
            print(
                "error: --interactive does not support if/conditional "
                "(IfOp) operations; use the batch run() path "
                "",
                file=sys.stderr,
            )
            return EXIT_ERROR
        else:  # IdleOp — only present after the noise pipeline, not raw IR.
            print(
                "error: --interactive encountered an IdleOp "
                "(noise-pipeline node) in the raw circuit; unsupported",
                file=sys.stderr,
            )
            return EXIT_ERROR

    print(f"seed         : {qr.current_seed()}")

    # Output the measured classical register bits (one trajectory). result()
    # carries a single {bitstring: 1} entry when measurements were present;
    # empty when the circuit has no measurements (state-only inspection via
    # --statevector / --expect).
    counts = qr.interactive.result().counts()
    if counts:
        print(f"measured     : {next(iter(counts))}")
    else:
        print("measured     : (no measurement ops)")

    # expectation_value / dump_statevector read the live backend state that the
    # interactive replay just produced (shared backend), so they reflect the
    # post-collapse state.
    for pauli in args.expect:
        print(f"<{pauli}> = {qr.expectation_value(pauli)}")

    if args.statevector:
        # Interactive path: the backend state reflects the post-replay state.
        # get_statevector_nonzero() works on the live backend.
        dump_statevector(qr.interactive, circ.num_qubits())

    return EXIT_OK


def main() -> int:
    args = build_parser().parse_args()

    # --threads has no binding setter (no set_omp_threads); apply it through the
    # standard OpenMP env knob, which the runtime reads when the extension loads.
    # This must happen BEFORE `import qplayer`, so the import is deferred to here.
    if args.threads is not None:
        os.environ["OMP_NUM_THREADS"] = str(args.threads)

    import qplayer

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

    try:
        # Batch mode honours --proc (default cpu; opt-in gpu/auto). Interactive
        # mode runs on CPU statevector backends only, so force CPU
        # there — a gpu/auto choice would resolve to the GPU backend on a GPU
        # host, which interactive.initialize() rejects (warned below).
        proc = qplayer.ProcType.CPU if args.interactive else proc_map[args.proc]
        qr = qplayer.QRegister(backend_map[args.backend], proc)
        if args.seed is not None:
            qr.set_seed(args.seed)

        circ = qr.parse_file(args.qasm_path)
        print(f"circuit: {args.qasm_path}")
        print(f"  backend    : {args.backend}")
        print(f"  proc       : {args.proc}")
        print(f"  qubits     : {circ.num_qubits()}")
        print(f"  cubits     : {circ.total_cubits()}")
        print(f"  operations : {circ.num_ops()}")

        # Interactive mode: replay the raw circuit immediately.
        # Warn on options that have no meaning under single-trajectory execution.
        if args.interactive:
            print("  mode       : interactive (immediate collapse, single trajectory)")
            if args.proc != "cpu":
                print(
                    f"warning: --proc {args.proc} is overridden to cpu in "
                    "--interactive mode (CPU statevector only)",
                    file=sys.stderr,
                )
            if args.shots != 1024:
                print(
                    "warning: --shots is ignored in --interactive mode "
                    "(single trajectory)",
                    file=sys.stderr,
                )
            if args.no_fusion:
                print(
                    "warning: --no-fusion is a no-op in --interactive mode "
                    "(runtime fusion is batch-only)",
                    file=sys.stderr,
                )
            if args.opt_level is not None:
                print(
                    "warning: --opt-level is ignored in --interactive mode; "
                    "the raw circuit is replayed",
                    file=sys.stderr,
                )
            maybe_draw(circ, args)
            return run_interactive(qr, circ, args, qplayer)

        if args.opt_level is not None:
            circ = qr.transpile(circ, args.opt_level)
            print(f"  transpiled : opt-level {args.opt_level} -> {circ.num_ops()} ops")
        maybe_draw(circ, args)

        # Time only the simulation (parse / transpile excluded), the region of
        # interest for a quick perf feel. Lazy sampling defers the
        # nshots draw to counts() below, mirroring the C++ example's timing scope.
        t0 = time.perf_counter_ns()
        r = qr.run(circ, args.shots, not args.no_fusion, args.fusion_width)
        elapsed_ns = time.perf_counter_ns() - t0

        print(f"seed         : {qr.current_seed()}")
        print(f"shots        : {r.shots()}")
        print(f"sim time     : {format_elapsed(elapsed_ns)}")
        print("counts       :")
        total = 0
        # std::map iteration order is sorted by key; sort to match the C++ output.
        for bits, n in sorted(r.counts().items()):
            print(f"  {bits} : {n}")
            total += n

        # Lightweight invariant 1: counts must sum to the shot count.
        if total != r.shots():
            print(f"verify: FAIL counts sum {total} != shots {r.shots()}",
                  file=sys.stderr)
            return EXIT_ERROR

        # Lightweight invariant 2: a recoverable statevector stays normalised.
        # Compute the norm with numpy (np.vdot conjugates the first operand, so
        # vdot(sv, sv) == Σ|amp|²) instead of a pure-Python generator sum: for a
        # wide circuit the densified statevector is 2^n amplitudes (2^27 ≈ 134 M),
        # and a Python-level loop over it runs single-threaded for tens of seconds.
        if r.has_statevector():
            sv = r.statevector()
            norm2 = float(np.vdot(sv, sv).real)
            if abs(norm2 - 1.0) > 1e-9:
                print(f"verify: FAIL |psi|^2 = {norm2} (expected 1)", file=sys.stderr)
                return EXIT_ERROR
        print("verify       : OK (counts sum, norm)")

        for pauli in args.expect:
            print(f"<{pauli}> = {qr.expectation_value(pauli)}")

        if args.statevector:
            dump_statevector(qr, circ.num_qubits())
        return EXIT_OK

    except qplayer.NotImplementedError as e:
        print(f"unsupported: {e}", file=sys.stderr)
        print(f"(hint: backend '{args.backend}' is not available in this "
              f"build/phase; try --backend fullstate)", file=sys.stderr)
        return EXIT_UNSUPP
    except qplayer.QASMParseError as e:
        print(f"parse error: {e}", file=sys.stderr)
        return EXIT_ERROR
    except qplayer.QPlayerError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_ERROR
    except Exception as e:  # noqa: BLE001 — mirror the C++ catch-all on std::exception
        print(f"error: {e}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
