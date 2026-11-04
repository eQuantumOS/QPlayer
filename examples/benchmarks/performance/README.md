# Performance comparison harness — `comparison.py`

A standalone console tool that measures the **wall-clock time** of running the
same QASM circuit on QPlayer and other statevector simulators, so their relative
performance can be compared. It runs directly from this directory and needs no
files outside `examples/benchmarks/`.

> This is a report-only measurement tool: it does not build, modify, or commit
> anything. It writes stdout tables (single-circuit mode) or a markdown report
> plus PNG plots (`--report` mode).

---

## 1. What it measures

The same QASM circuit is run on up to four backends and each run is split into
four timed sections, reported as the median in milliseconds:

| Section  | Meaning                          |
|----------|----------------------------------|
| compile  | parse QASM + transpile/optimize  |
| execute  | state-vector evolution (`run()`) |
| sampling | extract measurement counts       |
| total    | compile + execute + sampling     |

| Backend   | How it is measured                    | CPU | GPU |
|-----------|---------------------------------------|:---:|:---:|
| Qiskit    | Aer, `method="statevector"`           |  ✓  |  ✓  |
| Cirq      | qsim backend (`qsimcirq`)             |  ✓  |  ✓  |
| cuQuantum | NVIDIA cuStateVec (`cuquantum`)       |  —  |  ✓  |
| QPlayer   | this project's Python bindings        |  ✓  |  ✓  |

- **optimize and fusion are always ON.**
- **Compare tools using the `total` column only.** Only QPlayer and cuQuantum
  cleanly separate `execute` from `sampling`; Qiskit Aer and Cirq/qsim fold
  shot sampling into `execute`, so the per-section split is asymmetric across
  tools. The `compile`/`execute`/`sampling` breakdown is for within-tool analysis.
- cuQuantum is GPU-only, so its CPU cell always shows `-`. Any GPU cell also
  shows `-` on a host without a GPU or without a GPU-capable build of that tool.

Each backend is optional: a missing package, or an unsupported processor, is
recorded as `skip` — never a hard failure. Whatever subset is installed on the
host gets measured.

---

## 2. Setup

### 2.1 QPlayer must be current

Measure with a binary that matches the current source tree. If the installed
package is older than the tree, reinstall it first:

```bash
# compare the installed .so timestamp against the newest source file
SO=$(python3 -c "import qplayer,glob,os;d=os.path.dirname(qplayer.__file__);print(glob.glob(d+'/_qplayer*.so')[0])")
NEWEST=$(find src include bindings CMakeLists.txt cmake -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cu' -o -name '*.cuh' \
       -o -name '*.cmake' -o -name 'CMakeLists.txt' \) \
    -printf '%T@\n' | sort -nr | head -1)
[ "$(stat -c %Y "$SO")" -ge "${NEWEST%.*}" ] && echo FRESH || echo STALE

# if STALE, reinstall (on a CUDA host, keep GPU ON so GPU rows are not CPU fallback)
python3 -m pip install . --no-build-isolation --force-reinstall --no-deps \
    -C cmake.define.QPLAYER_BUILD_GPU=ON      # drop this flag if there is no CUDA
```

### 2.2 Comparison backends (all optional)

Install only the ones you want to compare against. See `requirements.txt` for
version notes (Qiskit pinned to 2.x; GPU Aer needs a source build):

```bash
pip install qiskit>=2.0 qiskit-aer   # Qiskit (CPU)
pip install cirq qsimcirq            # Cirq / qsim
pip install matplotlib               # PNG plots for --report
# cuQuantum (GPU): cupy-cuda12x + cuquantum-python-cu12
```

---

## 3. Running

### 3.1 Single circuit → stdout table (quick check)

```bash
cd examples/benchmarks/performance
python comparison.py qasm/q23_ghz_state.qasm --shots 1024 --repeats 5 --procs cpu gpu
```

| Option           | Default   | Meaning                                       |
|------------------|-----------|-----------------------------------------------|
| `CIRCUIT.qasm`   | —         | one QASM circuit to benchmark                 |
| `--shots`        | 1024      | sampling shot count                           |
| `--repeats`      | 3         | timed repeats (warm-up discarded; median)     |
| `--procs`        | `cpu gpu` | processors to measure (`cpu`, `gpu`, or both) |

### 3.2 Full sweep → markdown report + PNG plots (no editing)

```bash
cd examples/benchmarks/performance
python comparison.py --report --shots 1024 --repeats 5 --procs cpu gpu
```

This measures every `qasm/*.qasm` circuit and writes, into `result/`:

- `result/performance-comparison-<date>-<time>.md` — the full report
  (environment, circuit summary, per-circuit tables, embedded plots),
- `result/plots-<date>-<time>/cpu.png` and `gpu.png` — grouped-bar figures of
  total time per circuit, with QPlayer's speed-up annotated on each bar.

The report is generated entirely by the tool; no manual editing is needed.

| Option        | Default          | Meaning                                    |
|---------------|------------------|--------------------------------------------|
| `--report`    | off              | run the sweep and write report + plots     |
| `--qasm-dir`  | `./qasm`         | directory of circuits to sweep             |
| `--out-dir`   | `./result`       | where the report and plots are written     |

Drop your own `*.qasm` files into `qasm/` (or point `--qasm-dir` elsewhere) to
extend the set. Circuits without a `measure` instruction are skipped.

---

## 4. Reading the output

Single-circuit stdout order:
1. `Available backends` — which backends were importable.
2. `# Circuit info` — qubits, clbits, gate ops, measure count, gate histogram.
3. The **4-section comparison table** — rows = processor, columns = tool × 4
   sections (ms).
4. `# Skipped / unavailable` — reason for any unmeasured cell.
5. `# [1] Relative speedup` — QPlayer total ÷ other total (`<1` = QPlayer faster).
6. `# [2] Relative performance` — QPlayer's perspective (`xN` = N× faster,
   `-xN` = N× slower; slower cells are red on a terminal).

In `--report` mode the same per-circuit blocks are embedded in the markdown
report, followed by the CPU/GPU plots.

---

## 5. Notes

- `qasm/` holds the standard circuit set (9–28 qubits).
- Results are kept under `result/`; each `--report` run uses its own
  `plots-<stamp>/` subdirectory so runs never mix.
- `--worker` is an internal flag used for per-backend subprocess isolation
  (each backend is measured in its own fresh interpreter so their OpenMP
  runtimes never contend). Do not pass it directly.
