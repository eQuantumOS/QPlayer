#!/usr/bin/env python3
"""Performance comparison harness for QPlayer.

=============================================================================
PLAIN-LANGUAGE OVERVIEW (read this first)
=============================================================================
Goal: measure how fast QPlayer simulates a quantum circuit, and compare it
against the other well-known simulators on the SAME circuit.

How it works, in four ideas:

  1. TOOLS ("adapters"). Each simulator we compare is wrapped in one small
     class -- an "adapter" -- with the same four steps: setup, compile, evolve,
     sample. Qiskit, Cirq(qsim), cuQuantum, and QPlayer each get one adapter.
     To add or drop a tool, add or remove its adapter in the ADAPTERS list.

  2. FOUR TIMED STEPS. Every run is split so you can see WHERE the time goes:
        compile  -- read the QASM file + optimize the circuit,
        execute  -- actually evolve the quantum state,
        sample   -- draw the measurement shots (the counts),
        total    -- the three added together.
     Compare tools on TOTAL: the split is not identical across tools (some fold
     sampling into execute -- see the section-boundary caveat far below).

  3. FAIR TIMING. A few warm-up runs are thrown away, then the median of a few
     timed repeats is reported (so one unlucky run does not skew the result).

  4. CLEAN ISOLATION. Each tool is measured in its OWN fresh subprocess, so one
     tool's threading runtime cannot slow down the next tool's measurement.

Everything is optional and best-effort: a tool that is not installed, or a GPU
that is not present, is simply marked "skip" -- the harness never crashes.

=============================================================================
TECHNICAL DETAIL
=============================================================================
Runs the SAME QASM circuit on the available statevector simulators and reports
wall-clock time per section so their relative performance can be compared:

  - Qiskit        Aer, method="statevector"          (CPU + GPU)
  - Cirq          qsim backend (``qsimcirq``)         (CPU + GPU)
  - QPlayer       this project's Python bindings       (CPU + GPU)
  - cuQuantum     NVIDIA cuStateVec via ``cuquantum``  (GPU only)

Cirq is measured through Google's qsim backend (qsimcirq): its native pure-
Python ``cirq.Simulator`` is far too slow for the 20+ qubit circuits here, so
qsim is the representative high-performance Cirq path. The Cirq/gpu cell prefers
cuStateVec (``gpu_mode=1``) when qsim is built from source with CUQUANTUM_ROOT
set, falling back to qsim's native CUDA kernels (``gpu_mode=0``) otherwise, and
skipping only if no GPU module is built at all. cuQuantum (cuStateVec) is a
GPU-only statevector backend, so its CPU cell is always skipped.

Comparison matrix:
    target(N) x processor(CPU, GPU);  optimize and fusion are FIXED ON.
The printed table reports all four median sections (compile / execute /
sampling / total) in ms per (processor, target). Cross-tool comparison should
use the TOTAL column -- the per-section split is asymmetric across tools (see
the section-boundary caveat below).

Each backend is an OPTIONAL dependency: a missing import, or an unsupported axis
value (e.g. GPU not present), is recorded as "skip" -- never a hard failure. The
matrix is therefore measured over whatever subset of targets/axes is actually
available on the host.

Process isolation: each adapter is measured in its OWN fresh subprocess
(run_matrix spawns one worker per adapter via ``--worker``). Backend libraries
are imported lazily, so a worker hosts exactly one backend's OpenMP runtime.
This eliminates the single-process artifact where Qiskit Aer's vendored libgomp
(a SECOND OpenMP runtime, distinct from QPlayer's system libgomp) left many idle
threads busy-spinning on all cores after Qiskit's run, starving the next-measured
tool's freshly-forked threads and making its median jump run-to-run.

Timing -- per-section breakdown:
  - a few warm-up iterations are executed and discarded,
  - the median of N timed repeats is reported,
  - each repeat is split into three TIMED sections plus their total:
        compile  -- parse QASM + transpile/optimize the circuit,
        execute  -- state-vector evolution (the run() boundary),
        sample   -- extract measurement counts (shot sampling),
        total    -- compile + execute + sample.
  - engine/simulator construction and the GPU-availability probe are done once
    per cell in an untimed "setup" step (and may mark the cell skip).

Section-boundary caveat (per tool API shape): only QPlayer and cuQuantum
separate evolution from sampling cleanly. QPlayer's run() performs evolution and
counts() performs the shot sampling (lazy sampling), so its execute and sample
sections are genuinely distinct. Qiskit Aer folds shot sampling into its
run()/result() call, so its "execute" section already includes sampling and the
"sample" section measures only the cheap counts retrieval. Cirq/qsim likewise
folds shot sampling into run(). cuQuantum evolves the state with apply_matrix
(execute) and draws shots through the cuStateVec sampler API (sample), so its two
sections are genuinely distinct. Compare sections within a tool; across tools
prefer the total.

The circuit MUST contain at least one ``measure`` instruction; a measurement-
free QASM yields no counts and is skipped entirely (not measured).

Usage:
    # single circuit -> stdout table (quick check)
    python comparison.py CIRCUIT.qasm [--shots N] [--repeats N] [--procs cpu gpu]

    # sweep the standard set -> markdown report + PNG plots (no editing needed)
    python comparison.py --report [--qasm-dir DIR] [--shots N] [--repeats N]
"""

from __future__ import annotations

import argparse
import glob
import importlib
import io
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from contextlib import redirect_stdout
from datetime import datetime
from typing import Optional


# =============================================================================
# SECTION 1 -- Loading optional backend libraries
# -----------------------------------------------------------------------------
# Heavy backend libraries are imported LAZILY and cached (not at module load).
# This is what makes the per-adapter subprocess isolation (run_matrix below)
# effective: a QPlayer worker never imports qiskit, and vice-versa, so each
# worker process hosts exactly ONE backend's OpenMP runtime. The main process
# may still load both libs to probe availability -- only worker purity matters,
# since workers are fresh interpreters.
_LIB_CACHE: dict = {}


def import_optional(name: str):
    """Import ``name`` on first use and cache it (None if unavailable).

    Returning None instead of raising is what lets every backend stay OPTIONAL:
    a missing library becomes a "skip", never a crash.
    """
    if name not in _LIB_CACHE:
        try:
            _LIB_CACHE[name] = importlib.import_module(name)
        except Exception:  # ImportError, or a broken install
            _LIB_CACHE[name] = None
    return _LIB_CACHE[name]


# The harness pins its Qiskit test environment to 2.x: Qiskit 2.x + GPU requires
# a source-built qiskit-aer 0.17.x (prebuilt qiskit-aer-gpu wheels stopped at
# 0.15.1, which is 1.x-only and breaks on import under Qiskit 2.x). A 1.x install
# is rejected so it cannot silently produce non-comparable numbers.
_QISKIT_MIN_MAJOR = 2


def _qiskit_major():
    """Major version of the installed qiskit, or None if not importable."""
    qk = import_optional("qiskit")
    if qk is None:
        return None
    try:
        return int(qk.__version__.split(".", 1)[0])
    except Exception:
        return None


# =============================================================================
# SECTION 2 -- Timing one run (compile / execute / sample), median of repeats
# =============================================================================
SECTIONS = ("compile", "execute", "sample")  # ordered timed sections + total


def timed_sections(adapter, context: dict, repeats: int, warmup: int = 3):
    """Time the compile/execute/sample sections of ``adapter`` per repeat.

    ``context`` is the (untimed) setup context; it is shallow-copied per iteration so
    each repeat compiles, evolves and samples from clean state (the engine object
    inside ``context`` is shared across repeats -- only its per-run products differ).

    Returns (medians, counts) where ``medians`` maps each section name and
    "total" to its median seconds; and ``counts`` is the final repeat's
    measurement dict.
    """
    def one_iteration():
        # Run the three steps back to back, timing each with a stopwatch.
        iter_context = dict(context)          # fresh copy so repeats stay clean
        t_start = time.perf_counter()
        iter_context = adapter.compile(iter_context)
        t_after_compile = time.perf_counter()
        iter_context = adapter.evolve(iter_context)
        t_after_evolve = time.perf_counter()
        counts = adapter.sample(iter_context)
        t_after_sample = time.perf_counter()
        return counts, (t_after_compile - t_start,
                        t_after_evolve - t_after_compile,
                        t_after_sample - t_after_evolve)

    # Warm-up runs are executed but their times are thrown away.
    for _ in range(warmup):
        one_iteration()

    # Collect each section's time across all timed repeats, then take the median.
    section_times = {section: [] for section in SECTIONS}
    totals = []
    counts = None
    for _ in range(repeats):
        counts, (compile_s, execute_s, sample_s) = one_iteration()
        section_times["compile"].append(compile_s)
        section_times["execute"].append(execute_s)
        section_times["sample"].append(sample_s)
        totals.append(compile_s + execute_s + sample_s)

    medians = {section: statistics.median(section_times[section])
               for section in SECTIONS}
    medians["total"] = statistics.median(totals)
    return medians, counts


class Skip(Exception):
    """Raised by an adapter when a (target, axis) combination is unavailable.

    A raised Skip becomes a "skip" cell in the table -- e.g. "GPU not present"
    or "this tool is CPU-only". It is expected, not an error.
    """


# =============================================================================
# SECTION 3 -- Reading a circuit's shape straight from its QASM text
# =============================================================================

def circuit_info(qasm_path: str) -> Optional[dict]:
    """Backend-independent QASM scan -> circuit metadata for the report.

    Returns a dict with qubits / clbits / gate_ops / measure_ops / barrier_ops /
    reset_ops / gate_counts (per-gate histogram), or None if the file cannot be
    read. Counting is backend-independent so it also works for circuits that a
    given backend's parser cannot load.
    """
    try:
        with open(qasm_path, encoding="utf-8") as fh:
            raw = fh.read()
    except OSError:
        return None

    text = re.sub(r"//[^\n]*", " ", raw)                # line comments
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)  # block comments

    qubits = clbits = 0
    measure_ops = barrier_ops = reset_ops = 0
    gate_counts: dict[str, int] = {}
    for stmt in text.split(";"):
        stmt = stmt.strip()
        if not stmt:
            continue
        head = re.match(r"([A-Za-z_][A-Za-z0-9_]*)", stmt)
        if not head:
            continue
        name = head.group(1)
        if name in ("OPENQASM", "include", "gate", "opaque"):
            continue
        if name == "qreg" or name == "creg":
            dim = re.search(r"\[(\d+)\]", stmt)
            n = int(dim.group(1)) if dim else 0
            if name == "qreg":
                qubits += n
            else:
                clbits += n
            continue
        if name == "measure":
            measure_ops += 1
            continue
        if name == "barrier":
            barrier_ops += 1
            continue
        if name == "reset":
            reset_ops += 1
            continue
        gate_counts[name] = gate_counts.get(name, 0) + 1

    return {
        "qubits": qubits,
        "clbits": clbits,
        "gate_ops": sum(gate_counts.values()),
        "measure_ops": measure_ops,
        "barrier_ops": barrier_ops,
        "reset_ops": reset_ops,
        "gate_counts": gate_counts,
    }


def gate_histogram(info: dict, top: int = 5) -> str:
    """Render the top-N gate histogram string (e.g. 'cx×22, h×1')."""
    items = sorted(info["gate_counts"].items(), key=lambda kv: (-kv[1], kv[0]))
    return ", ".join(f"{g}×{n}" for g, n in items[:top]) or "(none)"


def format_circuit_info(qasm_path: str, info: dict) -> str:
    """Render the circuit-info block printed above the comparison table."""
    lines = [
        f"# Circuit info -- {qasm_path}",
        f"  qubits={info['qubits']}  clbits={info['clbits']}  "
        f"gate ops={info['gate_ops']}  measure={info['measure_ops']}  "
        f"barrier={info['barrier_ops']}  reset={info['reset_ops']}",
        f"  gate types: {gate_histogram(info, top=99)}",
    ]
    return "\n".join(lines)


# =============================================================================
# SECTION 4 -- The tools we compare (one "adapter" class per simulator)
# -----------------------------------------------------------------------------
# This is the heart of the file. Each simulator (Qiskit, Cirq/qsim, cuQuantum,
# QPlayer) is wrapped in ONE class with the SAME five methods, so the harness can
# treat every tool identically and time the four steps the same way. To add a new
# tool, copy one of these classes and add it to the ADAPTERS list at the bottom.
#
# The five methods each adapter provides:
#   available()  -> bool       Is this tool installed at all?
#   setup(qasm, proc, optimize, fusion, shots) -> context   UNTIMED, once per cell:
#                              build the engine + probe the device. May raise Skip
#                              (e.g. no GPU) to mark the cell "skip".
#   compile(context) -> context    TIMED step 1: read the QASM + optimize/transpile.
#   evolve(context)  -> context    TIMED step 2: evolve the quantum state (the run).
#   sample(context)  -> counts     TIMED step 3: draw the measurement shots.
#
# ``context`` is a plain dict that carries state between the steps. setup() fills
# it; each of compile/evolve/sample receives a FRESH copy each repeat and threads
# its product back into it (e.g. context["tqc"], context["result"]).


class QiskitAdapter:
    """IBM Qiskit's Aer statevector simulator (CPU + GPU). Aer folds the shot
    sampling into its run(), so its 'execute' section already includes sampling."""

    name = "Qiskit"

    def available(self) -> bool:
        if import_optional("qiskit_aer") is None:
            return False
        major = _qiskit_major()
        if major is None:
            return False
        if major < _QISKIT_MIN_MAJOR:  # harness pinned to Qiskit 2.x
            print(f"[Qiskit] found qiskit {import_optional('qiskit').__version__} but the "
                  f"benchmark harness is pinned to >= {_QISKIT_MIN_MAJOR}.0; "
                  f"skipping the Qiskit adapter.", file=sys.stderr)
            return False
        return True

    @staticmethod
    def _load_qasm(path: str):
        # Qiskit 2.x always exposes qasm2.load. The default builtin set omits
        # qelib1 extensions (swap, cswap, sx, ...); LEGACY_CUSTOM_INSTRUCTIONS
        # supplies them so circuits using those gates still parse.
        from qiskit import qasm2
        return qasm2.load(path, custom_instructions=qasm2.LEGACY_CUSTOM_INSTRUCTIONS)

    def setup(self, qasm, proc, optimize, fusion, shots):
        device = "GPU" if proc == "gpu" else "CPU"
        try:
            sim = import_optional("qiskit_aer").AerSimulator(
                method="statevector",
                device=device,
                fusion_enable=bool(fusion),
            )
        except Exception as exc:  # device unsupported by this Aer build
            raise Skip(f"AerSimulator(device={device}) unavailable: {exc}")

        if device == "GPU":
            # Aer silently falls back to CPU if no GPU; verify the device is real.
            try:
                avail = set(sim.available_devices())
            except Exception:
                avail = set()
            if "GPU" not in avail:
                raise Skip("Aer GPU device not available")

        return {"sim": sim, "qasm": qasm, "optimize": optimize, "shots": shots}

    def compile(self, context):
        from qiskit import transpile as qk_transpile
        qc = self._load_qasm(context["qasm"])
        # optimize OFF -> optimization_level 0, ON -> 2 (level 2 makes Qiskit's
        # optimization comparable to QPlayer's).
        level = 2 if context["optimize"] else 0
        context["tqc"] = qk_transpile(qc, context["sim"], optimization_level=level,
                                  seed_transpiler=1234)
        return context

    def evolve(self, context):
        # Aer folds shot sampling into run()/result(); the sample section below
        # then only retrieves the already-computed counts.
        context["result"] = context["sim"].run(
            context["tqc"], shots=context["shots"], seed_simulator=1234).result()
        return context

    def sample(self, context):
        return dict(context["result"].get_counts())


class QPlayerAdapter:
    """This project (CPU + GPU). Uses lazy sampling: run() does the evolution and
    counts() does the shot sampling, so its 'execute' and 'sample' sections are
    genuinely separate (unlike Qiskit/Cirq)."""

    name = "QPlayer"

    def available(self) -> bool:
        return import_optional("qplayer") is not None

    def setup(self, qasm, proc, optimize, fusion, shots):
        _qp = import_optional("qplayer")
        proc_type = (
            _qp.ProcType.GPU if proc == "gpu" else _qp.ProcType.CPU
        )
        # NOTE: QRegister falls back to CPU with a warning if GPU is requested
        # without CUDA (no exception). On a CPU-only host a "gpu" row therefore
        # measures the CPU backend; interpret GPU rows accordingly.
        qr = _qp.QRegister(_qp.SimType.FullState, proc_type)
        qr.set_seed(1234)  # reproducible sampling
        return {"qr": qr, "qasm": qasm, "optimize": optimize,
                "fusion": bool(fusion), "shots": shots}

    def compile(self, context):
        circ = context["qr"].parse_file(context["qasm"])
        # optimize OFF -> optimization_level 0, ON -> 3 (max passes).
        level = 3 if context["optimize"] else 0
        context["circ"] = context["qr"].transpile(circ, optimization_level=level)
        return context

    def evolve(self, context):
        # enable_fusion applies inside run(); lazy sampling defers the actual
        # shot sampling to counts(), so this section is evolution only.
        context["result"] = context["qr"].run(
            context["circ"], context["shots"], enable_fusion=context["fusion"])
        return context

    def sample(self, context):
        # First counts() call triggers the lazy shot sampling.
        return dict(context["result"].counts())


class CirqAdapter:
    """Google Cirq measured through its high-performance qsim backend (qsimcirq),
    CPU + GPU. Cirq's pure-Python simulator is too slow for 20+ qubits, so qsim is
    the representative Cirq path. qsim folds shot sampling into run() (like Aer)."""

    name = "Cirq"

    def available(self) -> bool:
        return import_optional("cirq") is not None and import_optional("qsimcirq") is not None

    @staticmethod
    def _custatevec_built() -> bool:
        # gpu_mode=1 routes evolution AND shot sampling through NVIDIA cuStateVec,
        # whose sampler is O(shots) instead of qsim's native O(2^n) multi-shot
        # path. Only present when qsim is built from source with CUQUANTUM_ROOT
        # set (the PyPI wheel ships native CUDA only).
        q = import_optional("qsimcirq")
        try:
            return q._load_qsim_custatevec() is not None
        except Exception:
            return False

    @staticmethod
    def _gpu_native_built() -> bool:
        # gpu_mode=0 uses qsim's hand-written CUDA kernels. State evolution is
        # fast, but the multi-shot sampler scans the full 2^n distribution once
        # per run() (independent of shot count), so it is the fallback only when
        # cuStateVec is unavailable.
        q = import_optional("qsimcirq")
        try:
            return q._load_qsim_gpu() is not None
        except Exception:
            return False

    def setup(self, qasm, proc, optimize, fusion, shots):
        import_optional("cirq")  # probe importability early
        qsimcirq = import_optional("qsimcirq")
        # qsim has no separate transpiler/optimizer, so the `optimize` axis has
        # no qsim analogue; only `fusion` maps to a knob: max_fused_gate_size.
        # ON -> 4 (matches QPlayer's fusion width), OFF -> 1 (no fusion).
        max_fuse = 4 if fusion else 1
        if proc == "gpu":
            if self._custatevec_built():
                opts = qsimcirq.QSimOptions(use_gpu=True, gpu_mode=1,
                                            max_fused_gate_size=max_fuse)
            elif self._gpu_native_built():
                opts = qsimcirq.QSimOptions(use_gpu=True, gpu_mode=0,
                                            max_fused_gate_size=max_fuse)
            else:
                raise Skip("qsim GPU module not built (CPU-only wheel)")
        else:
            opts = qsimcirq.QSimOptions(use_gpu=False,
                                        max_fused_gate_size=max_fuse)
        sim = qsimcirq.QSimSimulator(qsim_options=opts, seed=1234)
        return {"sim": sim, "qasm": qasm, "shots": shots}

    def compile(self, context):
        # compile section = parse QASM into a cirq circuit. qsim does its gate
        # translation/fusion lazily inside run(); there is no separate transpile
        # step to time here (unlike Qiskit/QPlayer).
        from cirq.contrib.qasm_import import circuit_from_qasm
        with open(context["qasm"], encoding="utf-8") as fh:
            text = fh.read()
        # cirq's QASM importer does not recognise `barrier`; it is a pure
        # scheduling/fusion boundary with no effect on the state vector, so
        # stripping it leaves the simulated result unchanged.
        text = re.sub(r"\bbarrier\b[^;]*;", "", text)
        context["circuit"] = circuit_from_qasm(text)
        return context

    def evolve(self, context):
        # qsim folds shot sampling into run() (like Aer); the sample section
        # below only aggregates the already-drawn measurement bits.
        context["result"] = context["sim"].run(context["circuit"], repetitions=context["shots"])
        return context

    def sample(self, context):
        # Combine every measurement key's per-shot bits into one bitstring/shot.
        import numpy as np
        meas = context["result"].measurements
        if not meas:
            return {}
        keys = sorted(meas.keys())
        allbits = np.concatenate([meas[k] for k in keys], axis=1)
        counts: dict[str, int] = {}
        for row in allbits:
            key = "".join(str(int(b)) for b in row)
            counts[key] = counts.get(key, 0) + 1
        return counts


class CuQuantumAdapter:
    """NVIDIA cuQuantum cuStateVec GPU statevector adapter (GPU-only).

    Parses QASM with cirq (barrier-stripped) and runs state-vector evolution +
    shot sampling on the GPU through cuquantum.bindings.custatevec.

    Bit-ordering rule: cirq.unitary uses big-endian qubit order (op.qubits[0] is
    the most-significant qubit in the matrix index), and cuStateVec apply_matrix
    also treats targets[0] as the MSB of the matrix index, while the state vector
    itself is little-endian (qubit i -> bit i). Reversing the targets list
    relative to op.qubits keeps the qubit_i -> bit_i mapping intact, which makes
    the sampled bitstring keys match Qiskit's convention."""

    name = "cuQuantum"

    def available(self) -> bool:
        return (
            import_optional("cupy") is not None
            and import_optional("cuquantum") is not None
            and import_optional("cuquantum.bindings") is not None
        )

    def setup(self, qasm, proc, optimize, fusion, shots):
        # cuStateVec is GPU-only; a CPU request has no analogue here.
        if proc == "cpu":
            raise Skip("cuStateVec is GPU-only")
        cp = import_optional("cupy")
        if cp is None:
            raise Skip("cupy not available")
        cuq = import_optional("cuquantum")
        if cuq is None:
            raise Skip("cuquantum not available")
        try:
            n_devices = cp.cuda.runtime.getDeviceCount()
        except Exception as exc:
            raise Skip(f"CUDA runtime probe failed: {exc}")
        if n_devices == 0:
            raise Skip("No CUDA-capable GPU found")

        # Create the cuStateVec library handle once; reuse it across repeats.
        from cuquantum.bindings import custatevec as csv
        handle = csv.create()
        return {
            "handle": handle, "qasm": qasm, "shots": shots,
            # Pre-resolve enum ints once so compile/evolve/sample stay clean.
            "sv_dtype": int(cuq.cudaDataType.CUDA_C_64F),
            "compute_type": int(cuq.ComputeType.COMPUTE_64F),
            "mat_layout": int(csv.MatrixLayout.ROW),
            "sampler_output": int(csv.SamplerOutput.RANDNUM_ORDER),
        }

    def compile(self, context):
        # TIMED: parse QASM via cirq, extract (matrix, targets) per gate op and
        # record the measured-qubit ordering for the sampler.
        import numpy as np
        cirq = import_optional("cirq")
        if cirq is None:
            raise RuntimeError("cirq not available for QASM parsing")
        circuit_from_qasm = importlib.import_module(
            "cirq.contrib.qasm_import").circuit_from_qasm

        with open(context["qasm"], encoding="utf-8") as fh:
            raw = fh.read()
        # cirq's QASM importer does not recognise `barrier` (a no-op boundary).
        qasm_text = re.sub(r"\bbarrier\b[^;]*;", "", raw)
        circ = circuit_from_qasm(qasm_text)

        # Sort qubits by name (q_0, q_1, ...) so index == QASM register index.
        all_qubits = sorted(circ.all_qubits(), key=lambda q: q.name)
        qubit_to_idx = {q: i for i, q in enumerate(all_qubits)}

        gate_ops = []        # (matrix complex128, targets[int])
        meas_map: dict = {}  # classical_bit_idx -> qubit_idx
        for moment in circ:
            for op in moment:
                if isinstance(op.gate, cirq.MeasurementGate):
                    # cirq encodes "measure q[k] -> c[j]" as key 'c_j'; parse j.
                    parts = op.gate.key.rsplit("_", 1)
                    try:
                        clbit_idx = int(parts[1]) if len(parts) == 2 \
                            else len(meas_map)
                    except ValueError:
                        clbit_idx = len(meas_map)
                    for q in op.qubits:
                        meas_map[clbit_idx] = qubit_to_idx[q]
                else:
                    mat = cirq.unitary(op).astype(np.complex128)
                    # Reverse to reconcile cirq's big-endian gate matrix with
                    # cuStateVec's little-endian state vector (see class doc).
                    targets = [qubit_to_idx[q] for q in reversed(op.qubits)]
                    gate_ops.append((mat, targets))

        context["gate_ops"] = gate_ops
        context["n_qubits"] = len(all_qubits)
        context["meas_map"] = meas_map
        return context

    def evolve(self, context):
        # TIMED: allocate a fresh |0...0> state vector and apply every gate.
        cp = import_optional("cupy")
        from cuquantum.bindings import custatevec as csv
        handle = context["handle"]
        n_qubits = context["n_qubits"]
        sv_dtype = context["sv_dtype"]
        compute_type = context["compute_type"]
        mat_layout = context["mat_layout"]

        sv = cp.zeros(2 ** n_qubits, dtype=cp.complex128)
        sv[0] = 1.0 + 0j
        for mat_np, targets in context["gate_ops"]:
            mat_gpu = cp.asarray(mat_np)
            # Workspace is 0 for standard 1-4Q gates; query to stay correct for
            # any larger matrix.
            ws_size = csv.apply_matrix_get_workspace_size(
                handle, sv_dtype, n_qubits, mat_gpu.data.ptr, sv_dtype,
                mat_layout, 0, len(targets), 0, compute_type)
            ws_ptr = 0
            if ws_size > 0:
                ws_ptr = cp.zeros(ws_size, dtype=cp.uint8).data.ptr
            csv.apply_matrix(
                handle, sv.data.ptr, sv_dtype, n_qubits,
                mat_gpu.data.ptr, sv_dtype, mat_layout,
                0,                       # adjoint = False
                targets, len(targets),
                [], [], 0,               # controls already folded into matrix
                compute_type, ws_ptr, ws_size)
        context["sv"] = sv
        return context

    def sample(self, context):
        # TIMED: draw shots from the evolved state via the cuStateVec sampler.
        cp = import_optional("cupy")
        import numpy as np
        from cuquantum.bindings import custatevec as csv
        handle = context["handle"]
        sv = context["sv"]
        n_qubits = context["n_qubits"]
        sv_dtype = context["sv_dtype"]
        meas_map = context["meas_map"]
        shots = context["shots"]

        # bit_ordering[i] = qubit measured into classical bit c[i]; the sampler
        # result integer then has bit i == c[i].
        n_clbits = max(meas_map.keys()) + 1 if meas_map else n_qubits
        bit_ordering = [meas_map.get(i, i) for i in range(n_clbits)]

        sampler, ws_size = csv.sampler_create(
            handle, sv.data.ptr, sv_dtype, n_qubits, shots)
        ws_ptr = 0
        if ws_size > 0:
            ws_ptr = cp.zeros(ws_size, dtype=cp.uint8).data.ptr
        csv.sampler_preprocess(handle, sampler, ws_ptr, ws_size)

        # Reproducible host RNG (matches the seeded-sampling policy).
        rand_nums = np.random.default_rng(1234).random(shots).astype(np.float64)
        bit_strings = np.zeros(shots, dtype=np.int64)
        csv.sampler_sample(
            handle, sampler, bit_strings.ctypes.data,
            bit_ordering, n_clbits, rand_nums, shots, context["sampler_output"])
        csv.sampler_destroy(sampler)

        # Integer bit i == c[i]; format(val, '0Nb') is MSB-first = c[n-1]..c[0],
        # which matches Qiskit's counts-key convention.
        counts: dict = {}
        for val in bit_strings:
            key = format(int(val), f"0{n_clbits}b")
            counts[key] = counts.get(key, 0) + 1
        return counts


# Active comparison set: Qiskit, Cirq(qsim), cuQuantum, QPlayer.
# cuQuantum is cuStateVec (GPU-only -- its CPU cell always skips). print_table
# and the speedup table derive their columns from ADAPTERS, so adding/removing
# an adapter here is automatically reflected in both tables and in the plots.
ADAPTERS = [QiskitAdapter(), CirqAdapter(), CuQuantumAdapter(), QPlayerAdapter()]
TOOL_NAMES = [a.name for a in ADAPTERS]


# =============================================================================
# SECTION 5 -- Running the comparison (one isolated subprocess per tool)
# -----------------------------------------------------------------------------
# optimize/fusion are fixed ON (the sweep is collapsed to the default-best
# configuration; see module docstring).
_OPTIMIZE = 1
_FUSION = True
_RESULT_MARKER = "@@CELL_RESULTS@@"   # worker -> orchestrator stdout protocol


def measure_adapter(adapter, qasm: str, shots: int, repeats: int, procs):
    """Measure one adapter across ``procs`` IN-PROCESS. Returns list of cells.

    Run by the isolated worker (one fresh interpreter per adapter) so that only
    this adapter's backend library -- and therefore only its OpenMP runtime -- is
    ever loaded in the measuring process."""
    cells = []
    for proc in procs:
        cell = {"target": adapter.name, "proc": proc,
                "optimize": "ON" if _OPTIMIZE else "OFF",
                "fusion": "ON" if _FUSION else "OFF"}
        try:
            context = adapter.setup(qasm, proc, _OPTIMIZE, _FUSION, shots)
            medians, _counts = timed_sections(adapter, context, repeats)
            cell.update(status="ok",
                        compile=medians["compile"], execute=medians["execute"],
                        sample=medians["sample"], total=medians["total"])
        except Skip as exc:
            cell.update(status="skip", reason=str(exc))
        except Exception as exc:  # noqa: BLE001 - report, don't crash
            cell.update(status="error", reason=f"{type(exc).__name__}: {exc}")
        cells.append(cell)
    return cells


def _run_adapter_worker(name: str, qasm: str, shots: int, repeats: int, procs):
    """Spawn a fresh interpreter that measures only adapter ``name`` and return
    its result cells. Isolation guarantee: the worker imports just one backend
    (lazy imports + single --worker target), so no foreign OpenMP runtime ever
    co-resides with the one under measurement."""
    import json
    cmd = [sys.executable, __file__, qasm, "--worker", name,
           "--shots", str(shots), "--repeats", str(repeats),
           "--procs", *procs]
    proc_run = subprocess.run(cmd, capture_output=True, text=True)
    # Parse the marker line; the worker prints exactly one. stderr (e.g. the
    # QPlayer GPU-fallback warning) is left out of stdout so JSON stays clean.
    for line in proc_run.stdout.splitlines():
        if line.startswith(_RESULT_MARKER):
            return json.loads(line[len(_RESULT_MARKER):])
    # Worker died before emitting results -- surface an error cell per proc.
    tail = (proc_run.stderr or proc_run.stdout or "").strip().splitlines()[-3:]
    reason = f"worker exited {proc_run.returncode}: {' | '.join(tail)}"
    return [{"target": name, "proc": p, "status": "error", "reason": reason}
            for p in procs]


def run_matrix(qasm: str, shots: int, repeats: int, procs):
    """Return result records -- one isolated worker subprocess per adapter."""
    records = []
    for adapter in ADAPTERS:
        if not adapter.available():
            records.append(
                {"target": adapter.name, "status": "skip",
                 "reason": "package not installed"}
            )
            continue
        records.extend(
            _run_adapter_worker(adapter.name, qasm, shots, repeats, procs))
    return records


# =============================================================================
# SECTION 6 -- Printing the results table and the QPlayer speed-up tables
# =============================================================================

def print_table(records, qasm, shots, repeats, procs):
    """Per-section table: rows = processors, grouped columns per target.

    Each target shows its four median sections (compile / execute / sampling /
    total) in ms. A cell that could not be measured on a target (package
    missing, unsupported processor, or runtime error) shows '-' across that
    target's whole group. Cross-tool comparison should use the TOTAL column
    (and the speedup table below) -- the per-section split is asymmetric across
    tools (QPlayer separates execute/sampling via lazy sampling, while Qiskit Aer
    folds sampling into execute; see module docstring).
    """
    targets = TOOL_NAMES
    # Display labels (left->right) and the record key each maps to. The internal
    # "sample" section is shown as "sampling" per the report format.
    sections = (("compile", "compile"), ("execute", "execute"),
                ("sampling", "sample"), ("total", "total"))

    # (target, proc) -> the full result record, plus a list of skip/error notes.
    record_by_cell = {}
    notes = []
    for record in records:
        if record.get("status") == "ok":
            record_by_cell[(record["target"], record["proc"])] = record
        elif record.get("status") in ("skip", "error"):
            where = f"{record['target']}/{record['proc']}" if "proc" in record \
                else f"{record['target']} (all)"
            notes.append(f"  {where}: {record['status']} -- {record.get('reason', '')}")

    print(f"\n# QPlayer performance comparison -- {qasm} "
          f"(shots={shots}, repeats={repeats}, optimize=ON, fusion=ON)")
    print("# per-section median ms; '-' = not available; "
          "compare across tools on TOTAL (sections asymmetric, see docstring)")

    proc_w = 8                       # width of the leading proc column
    sec_w = 10                       # width per section column
    group_w = sec_w * len(sections)  # width of one target's column group

    # Top header: target names centred over their section groups.
    top = f"{'proc':<{proc_w}}" + "".join(f"{t:^{group_w}}" for t in targets)
    # Sub header: the four section labels repeated under each target.
    sub = f"{'':<{proc_w}}" + "".join(
        "".join(f"{label:>{sec_w}}" for label, _ in sections)
        for _ in targets)
    print()
    print(top)
    print(sub)
    print("-" * len(sub))

    for proc in procs:
        row = f"{proc:<{proc_w}}"
        for target in targets:
            record = record_by_cell.get((target, proc))
            for _label, key in sections:
                seconds = record.get(key) if record is not None else None
                text = f"{seconds * 1e3:.3f}" if seconds is not None else "-"
                row += f"{text:>{sec_w}}"
        print(row)

    if notes:
        print("\n# Skipped / unavailable")
        for note in notes:
            print(note)

    _print_speedup(records, procs)


_ANSI_RED = "\033[31m"
_ANSI_RESET = "\033[0m"


def _speedup_cell(qp, other):
    """Format one QPlayer-vs-other total-time ratio (QPlayer's perspective).

    Returns (text, is_slower):
      QPlayer faster/equal -> "xN"   (N = other / qp >= 1; bigger = more ahead),
      QPlayer slower       -> "-xN"  (N = qp / other > 1; the leading '-' marks it),
      unmeasurable cell    -> ("-", False).
    """
    if not other or not qp:
        return "-", False
    if other >= qp:
        return f"x{other / qp:.2f}", False
    return f"-x{qp / other:.2f}", True


def _print_speedup(records, procs):
    """Per processor, show QPlayer's TOTAL-time performance vs the other tools as
    two tables:

      [1] Ratio     -- QPlayer total / other total ("Nx" suffix).
                       <1 = QPlayer faster, >1 = slower (the raw ratio).
      [2] QPlayer's -- xN = QPlayer N x faster, -xN = N x slower. On a TTY the
          perspective  slower ("-xN") cells are printed red; when captured to a
                       file isatty() is false so the text stays plain -- the
                       leading '-' still marks it.
    """
    ok_records = [r for r in records if r.get("status") == "ok"]
    if not ok_records:
        return
    total_by_cell = {(r["target"], r["proc"]): r["total"] for r in ok_records}
    use_color = sys.stdout.isatty()
    proc_w = 10
    col_w = 14
    other_tools = tuple(t for t in TOOL_NAMES if t != "QPlayer")

    def _header():
        h = f"{'proc':<{proc_w}}" + "".join(f"{'vs ' + t:>{col_w}}" for t in other_tools)
        print(h)
        print("-" * len(h))

    # [1] raw ratio: QPlayer total / other total (<1 = faster, >1 = slower)
    print("\n# [1] Relative speedup on TOTAL time "
          "(QPlayer total / other total; <1 = QPlayer faster, >1 = slower)\n")
    _header()
    for proc in procs:
        qplayer_total = total_by_cell.get(("QPlayer", proc))
        if qplayer_total is None:
            continue
        row = f"{proc:<{proc_w}}"
        for tool in other_tools:
            other_total = total_by_cell.get((tool, proc))
            text = f"{qplayer_total / other_total:.2f}x" if other_total else "-"
            row += f"{text:>{col_w}}"
        print(row)

    # [2] QPlayer's perspective: xN faster / -xN slower (slower = red on TTY)
    print("\n# [2] Relative performance on TOTAL time "
          "(QPlayer's perspective; xN = QPlayer N x faster, -xN = N x slower)\n")
    _header()
    for proc in procs:
        qplayer_total = total_by_cell.get(("QPlayer", proc))
        if qplayer_total is None:
            continue
        row = f"{proc:<{proc_w}}"
        for tool in other_tools:
            text, slower = _speedup_cell(qplayer_total, total_by_cell.get((tool, proc)))
            padded = f"{text:>{col_w}}"
            if slower and use_color:
                padded = f"{_ANSI_RED}{padded}{_ANSI_RESET}"
            row += padded
        print(row)


def measure_one(qasm, shots, repeats, procs):
    """Run one circuit and return (records, captured_stdout_text).

    Captures exactly what the single-circuit console mode prints (backends line,
    circuit info, comparison table, speedup tables) so the report can embed it
    verbatim.
    """
    info = circuit_info(qasm)
    buf = io.StringIO()
    with redirect_stdout(buf):
        available = [a.name for a in ADAPTERS if a.available()]
        print(f"Available backends: {', '.join(available) or '(none)'}")
        if info is not None:
            print("\n" + format_circuit_info(qasm, info))
        records = run_matrix(qasm, shots, repeats, procs)
        print_table(records, qasm, shots, repeats, procs)
    return records, buf.getvalue()


# =============================================================================
# SECTION 7 -- Environment info (recorded at the top of the report)
# =============================================================================


def _version(mod_name, attr="__version__"):
    """Return a module's version string, or None if not importable."""
    mod = import_optional(mod_name)
    if mod is None:
        return None
    return getattr(mod, attr, "?")


def _cpu_model():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _gpu_model():
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=10)
        names = [n.strip() for n in out.stdout.splitlines() if n.strip()]
        if not names:
            return "none detected"
        uniq = {}
        for n in names:
            uniq[n] = uniq.get(n, 0) + 1
        return ", ".join(f"{n} ×{c}" if c > 1 else n for n, c in uniq.items())
    except Exception:
        return "none detected"


def _qplayer_so_mtime():
    qp = import_optional("qplayer")
    if qp is None:
        return None
    try:
        d = os.path.dirname(qp.__file__)
        so = glob.glob(os.path.join(d, "_qplayer*.so"))
        if so:
            return datetime.fromtimestamp(os.path.getmtime(so[0])) \
                .strftime("%Y-%m-%d %H:%M:%S")
    except Exception:
        pass
    return None


def collect_env(shots, repeats):
    """Gather host / library environment rows for the report table."""
    rows = []
    rows.append(("host", f"{platform.system()} {platform.release()} "
                         f"{platform.machine()}"))
    rows.append(("cpu", _cpu_model()))
    rows.append(("gpu", _gpu_model()))
    rows.append(("qiskit", _version("qiskit") or "not installed"))
    cirq_v = _version("cirq")
    qsim_v = _version("qsimcirq")
    rows.append(("cirq / qsimcirq",
                 f"{cirq_v or '-'} / {qsim_v or '-'}"))
    cupy_v = _version("cupy")
    cuq_v = _version("cuquantum")
    rows.append(("cupy / cuquantum",
                 f"{cupy_v or '-'} / {cuq_v or '-'} (cuStateVec, GPU only)"))
    qp_v = _version("qplayer")
    so_mtime = _qplayer_so_mtime()
    qp_cell = qp_v or "not installed"
    if so_mtime:
        qp_cell += f" (.so built {so_mtime})"
    rows.append(("qplayer", qp_cell))
    rows.append(("shots / repeats",
                 f"{shots} / {repeats} (warm-up discarded, median reported)"))
    return rows


# =============================================================================
# SECTION 8 -- Plots (grouped bars, log-scale total time)
# -----------------------------------------------------------------------------
# Okabe-Ito colorblind-safe palette; QPlayer emphasised (black edge, full alpha).
_COLOR = {"Qiskit": "#0072B2", "Cirq": "#E69F00",
          "cuQuantum": "#009E73", "QPlayer": "#D55E00"}


def _geomean(vals):
    import math
    vals = [v for v in vals if v and v > 0]
    return math.exp(sum(map(math.log, vals)) / len(vals)) if vals else None


def _plot(proc, circuits, out_path, shots):
    """Render one grouped-bar figure for a processor. circuits is a list of
    {key, qubits, cpu{tool:total_ms}, gpu{tool:total_ms}}."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import LogLocator, NullFormatter

    rows = sorted([c for c in circuits
                   if any(c[proc].get(t) for t in TOOL_NAMES)],
                  key=lambda c: (c["qubits"], c["key"]))
    if not rows:
        print(f"  (no {proc} data -- skipped {out_path})")
        return False
    draw_order = TOOL_NAMES[:-1] + ["QPlayer"] if "QPlayer" in TOOL_NAMES \
        else TOOL_NAMES
    tools = [t for t in draw_order if any(c[proc].get(t) for c in rows)]
    keys = [c["key"] for c in rows]
    labels = [f"{k.split('_', 1)[-1]}\n{c['qubits']}q"
              for k, c in zip(keys, rows)] + ["GEOMEAN"]
    n_groups = len(rows) + 1
    n_tools = len(tools)
    bar_w = 0.8 / max(n_tools, 1)

    fig, ax = plt.subplots(figsize=(15.0, 5.2))
    x = range(n_groups)
    series_by, offs_by, gmax = {}, {}, 0.0
    for i, tool in enumerate(tools):
        s = [c[proc].get(tool) for c in rows]
        s.append(_geomean(s))
        offs = [xi - 0.4 + bar_w * (i + 0.5) for xi in x]
        series_by[tool], offs_by[tool] = s, offs
        gmax = max(gmax, max((v for v in s if v), default=0.0))
        emph = tool == "QPlayer"
        ax.bar(offs, s, width=bar_w, label=tool, color=_COLOR.get(tool, "#888"),
               edgecolor="black", linewidth=(0.9 if emph else 0.3),
               alpha=(1.0 if emph else 0.92), zorder=3)

    # Annotate every competitor bar with QPlayer's speed-up on that circuit.
    qp = series_by.get("QPlayer")
    if qp:
        for tool in tools:
            if tool == "QPlayer":
                continue
            for g in range(n_groups):
                tv, qv = series_by[tool][g], qp[g]
                if not tv or not qv:
                    continue
                r = tv / qv
                faster = r >= 1.0
                text = f"x{r:.1f}" if faster else f"-x{1.0 / r:.1f}"
                ax.annotate(text, xy=(offs_by[tool][g], tv), xytext=(0, 3),
                            textcoords="offset points", ha="center",
                            va="bottom", fontsize=8.5,
                            color=("black" if faster else "#D55E00"),
                            rotation=90, zorder=4)

    ax.set_yscale("log")
    ax.set_ylim(bottom=0.1, top=max(gmax * 6, 1.0))
    ax.set_ylabel("Total time (ms, log scale)", fontsize=11)
    ax.set_title(f"Quantum circuit simulation -- {proc.upper()} "
                 f"(shots={shots}, optimize+fusion ON; lower is better)",
                 fontsize=12, fontweight="bold")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=8.5)
    ax.set_xlim(-0.6, n_groups - 0.4)
    ax.axvline(n_groups - 1.5, color="gray", linestyle=":", linewidth=0.8,
               zorder=1)
    ax.yaxis.set_major_locator(LogLocator(base=10))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.grid(axis="y", which="major", linestyle="-", alpha=0.30, zorder=0)
    ax.grid(axis="y", which="minor", linestyle=":", alpha=0.15, zorder=0)
    ax.set_axisbelow(True)
    ax.legend(ncol=max(n_tools, 1), fontsize=10, loc="upper left",
              framealpha=0.9, edgecolor="gray")
    fig.text(0.995, 0.005,
             "Bar labels: QPlayer's speed-up vs that tool on each circuit "
             "(rightmost = geomean over all circuits;  xN = QPlayer N x faster, "
             "-xN = N x slower, shown red)",
             ha="right", va="bottom", fontsize=8, style="italic", color="#444")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out_path}")
    return True


def generate_plots(circuits, out_dir, stamp, shots):
    """Render cpu.png / gpu.png into out_dir/plots-<stamp>/. Returns the list of
    (proc, relative_path) that were actually produced (empty if matplotlib is
    missing or there is no data)."""
    try:
        import matplotlib  # noqa: F401
    except Exception:
        print("matplotlib not installed (optional); skipping figures.",
              file=sys.stderr)
        return []
    plot_dir = os.path.join(out_dir, f"plots-{stamp}")
    os.makedirs(plot_dir, exist_ok=True)
    produced = []
    for proc in ("cpu", "gpu"):
        out_path = os.path.join(plot_dir, f"{proc}.png")
        if _plot(proc, circuits, out_path, shots):
            produced.append((proc, f"plots-{stamp}/{proc}.png"))
    return produced


def _records_to_circuit(key, info, records):
    """Fold a circuit's result records into {key, qubits, cpu{}, gpu{}} totals
    (ms) for plotting."""
    entry = {"key": key, "qubits": info["qubits"], "cpu": {}, "gpu": {}}
    for r in records:
        if r.get("status") == "ok" and r.get("proc") in ("cpu", "gpu"):
            entry[r["proc"]][r["target"]] = r["total"] * 1e3
    return entry


# =============================================================================
# SECTION 9 -- Full sweep: run every circuit, write the markdown report
# =============================================================================

def run_report(qasm_dir, shots, repeats, procs, out_dir):
    """Sweep every *.qasm in qasm_dir, write a markdown report + PNG plots.

    Fully deterministic: no manual editing of the output is required.
    """
    stamp = datetime.now().strftime("%Y-%m-%d-%H%M")
    date_human = datetime.now().strftime("%Y-%m-%d %H:%M")
    qasm_files = sorted(glob.glob(os.path.join(qasm_dir, "*.qasm")))
    if not qasm_files:
        print(f"No .qasm circuits found in {qasm_dir}", file=sys.stderr)
        return 1

    available = [a.name for a in ADAPTERS if a.available()]
    if "QPlayer" not in available:
        print("QPlayer is not importable; nothing to compare against. "
              "Install the qplayer package first.", file=sys.stderr)
        return 1

    print(f"Backends available: {', '.join(available)}")
    print(f"Sweeping {len(qasm_files)} circuit(s) from {qasm_dir}\n")

    summary = []      # (name, info, tested_bool)
    per_circuit = []  # (name, info, stdout_text)
    plot_circuits = []
    for qasm in qasm_files:
        name = os.path.basename(qasm)
        info = circuit_info(qasm)
        if info is None:
            print(f"[skip] {name}: cannot read file")
            summary.append((name, {"qubits": 0, "clbits": 0, "gate_ops": 0,
                                    "measure_ops": 0, "gate_counts": {}}, False))
            continue
        if info["measure_ops"] == 0:
            print(f"[skip] {name}: no measure instruction")
            summary.append((name, info, False))
            per_circuit.append((name, info, "SKIP: no measure instruction; "
                                            "counts-based comparison impossible."))
            continue
        print(f"[run ] {name}  (qubits={info['qubits']})")
        records, text = measure_one(qasm, shots, repeats, procs)
        summary.append((name, info, True))
        per_circuit.append((name, info, text))
        plot_circuits.append(
            _records_to_circuit(name[:-len(".qasm")], info, records))

    os.makedirs(out_dir, exist_ok=True)
    plots = generate_plots(plot_circuits, out_dir, stamp, shots)

    report_path = os.path.join(out_dir, f"performance-comparison-{stamp}.md")
    _write_report(report_path, date_human, qasm_dir, shots, repeats,
                  summary, per_circuit, plots)
    print(f"\nReport written: {report_path}")
    return 0


def _write_report(path, date_human, qasm_dir, shots, repeats,
                  summary, per_circuit, plots):
    lines = []
    add = lines.append  # add(text) appends one line to the markdown report
    add(f"# Performance comparison report -- {date_human}")
    add("")
    add("> Tool: `comparison.py`")
    add("> Compared: Qiskit (statevector) · Cirq (qsim) · "
        "cuQuantum (cuStateVec, GPU only) · QPlayer")
    add("> Matrix: processor (CPU · GPU) × tool (4); optimize and fusion always ON")
    add("> Metric: 4 sections (compile · execute · sampling · total) median ms -- "
        "cross-tool comparison uses TOTAL")
    add(f"> Circuits under test: `{qasm_dir}` ({sum(1 for _, _, t in summary if t)}"
        f" of {len(summary)} measured)")
    add("")
    add("## Environment")
    add("| Item | Value |")
    add("|------|-------|")
    add(f"| datetime | {date_human} |")
    for key, value in collect_env(shots, repeats):
        add(f"| {key} | {value} |")
    add("")
    add("## Circuit summary")
    add("| Circuit | Qubits | Clbits | Gate ops | measure | Top gates | Tested |")
    add("|---------|-------:|-------:|---------:|--------:|-----------|:------:|")
    for name, info, tested in summary:
        add(f"| {name} | {info['qubits']} | {info['clbits']} | "
            f"{info['gate_ops']} | {info['measure_ops']} | "
            f"{gate_histogram(info)} | {'✓' if tested else 'SKIP'} |")
    add("")
    add("## Per-circuit results")
    for name, info, text in per_circuit:
        add("")
        add(f"### {name}")
        add(f"- circuit: qubits {info['qubits']} · clbits {info['clbits']} · "
            f"gate ops {info['gate_ops']} · measure {info['measure_ops']} · "
            f"gate types {gate_histogram(info)}")
        add(f"- command: `python comparison.py qasm/{name} "
            f"--shots {shots} --repeats {repeats} --procs cpu gpu`")
        add("```text")
        add(text.rstrip("\n"))
        add("```")
    add("")
    add("## Plots")
    if plots:
        for proc, rel in plots:
            add(f"### {proc.upper()}")
            add(f"![{proc} total time]({rel})")
            add("")
    else:
        add("_No plots produced (matplotlib not installed or no data)._")
    add("")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


# =============================================================================
# SECTION 10 -- Command-line entry point
# =============================================================================

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("qasm", nargs="?", default=None,
                        help="path to a single QASM circuit (console mode)")
    parser.add_argument("--report", action="store_true",
                        help="sweep --qasm-dir and write a markdown report + PNG "
                             "plots into --out-dir")
    parser.add_argument("--qasm-dir", default=None,
                        help="directory of circuits for --report "
                             "(default: ./qasm next to this script)")
    parser.add_argument("--out-dir", default=None,
                        help="report/plot output directory "
                             "(default: ./result next to this script)")
    parser.add_argument("--shots", type=int, default=1024)
    parser.add_argument("--repeats", type=int, default=3,
                        help="timed repeats (warm-up excluded); median reported")
    parser.add_argument("--procs", nargs="+", default=["cpu", "gpu"],
                        choices=["cpu", "gpu"])
    # Internal: when set, this is an isolated measurement worker for one adapter.
    # The orchestrator (run_matrix) spawns it; users never pass --worker directly.
    parser.add_argument("--worker", default=None, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))

    # Worker mode: measure ONLY the named adapter in this fresh process and emit
    # the result cells as a single JSON marker line (subprocess isolation).
    if args.worker is not None:
        import json
        adapter = next((a for a in ADAPTERS if a.name == args.worker), None)
        if adapter is None or not adapter.available():
            cells = [{"target": args.worker, "proc": p, "status": "skip",
                      "reason": "package not installed"} for p in args.procs]
        else:
            cells = measure_adapter(adapter, args.qasm, args.shots,
                                    args.repeats, args.procs)
        print(_RESULT_MARKER + json.dumps(cells))
        return 0

    # Report mode: sweep the circuit directory and emit md + PNG.
    if args.report:
        qasm_dir = args.qasm_dir or os.path.join(here, "qasm")
        out_dir = args.out_dir or os.path.join(here, "result")
        return run_report(qasm_dir, args.shots, args.repeats,
                          args.procs, out_dir)

    # Console mode: a single circuit -> stdout table.
    if args.qasm is None:
        parser.error("provide a CIRCUIT.qasm, or use --report to sweep the set")

    info = circuit_info(args.qasm)
    if info is not None and info["measure_ops"] == 0:
        print(f"SKIP: '{args.qasm}' has no measure instruction; "
              f"counts-based comparison is not possible. Nothing measured.",
              file=sys.stderr)
        return 0

    available = [a.name for a in ADAPTERS if a.available()]
    if not available:
        print("No comparison backend installed (qiskit / cirq / cuquantum / "
              "qplayer). Nothing to measure.", file=sys.stderr)
        return 1
    print(f"Available backends: {', '.join(available)}")
    if info is not None:
        print("\n" + format_circuit_info(args.qasm, info))

    records = run_matrix(args.qasm, args.shots, args.repeats, args.procs)
    print_table(records, args.qasm, args.shots, args.repeats, args.procs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
