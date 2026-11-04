# QPlayer Examples

This directory holds runnable usage examples for QPlayer. Each file doubles as a
**usage reference** (it exercises a slice of the public API) and a **hands-on
runner** you can point at your own circuits. The examples ship with the source
repository and are kept lightweight — golden / expected-value verification lives
in the [test suite](../tests/README.md), not here.

There are two kinds of example:

- **C++** — one CLI runner (`run_qasm.cpp`) that links the core static library.
- **Python** — a QASM runner twin plus focused demos for the native builder,
  noise models, primitives, and the Qiskit / PennyLane plugins.

---

## 1. File Structure

| File | Language | Purpose |
|------|----------|---------|
| `run_qasm.cpp` | C++ | CLI: parse an OpenQASM v2 file → optional transpile → run → inspect (counts, statevector, expectation values, gate fusion, threads, backend, circuit diagram). Full public-surface tour. |
| `run_qasm.py` | Python | Python twin of `run_qasm.cpp` — same flags, same output, plus `--draw-image` (matplotlib). |
| `python_demo.py` | Python | Build circuits **in code** with the native `Circuit` builder (`circ.h(0).cx(0,1)`), run on FullState CPU, read counts. Bell / GHZ(5) / biased qubit / expectation value. |
| `noise_demo.py` | Python | One case per noise channel (..07) + a combined case + a device-noise case from a JSON string. Runs each circuit ideal vs. noisy and prints the two distributions side by side. |
| `estimator_demo.py` | Python | Qiskit-compatible primitives: `Observable` (weighted Pauli sum), `Estimator` (exact `<H>`), `Sampler` (counts). Bell energy + a mini VQE energy sweep. |
| `qiskit_plugin_demo.py` | Python | QPlayer as a **Qiskit backend** — `transpile(qc, backend) + backend.run()`, `BackendSamplerV2`, `BackendEstimatorV2`. Circuits stay 100% pure Qiskit. |
| `pennylane_plugin_demo.py` | Python | QPlayer as a **PennyLane device** (`qml.device("qplayer.qubit", ...)`) — sampling, VQE `<H>`, parameter-shift gradient. QNodes stay pure PennyLane. |
| `benchmarks/performance/` | Python | Standalone harness (`comparison.py`) that times the same QASM circuit on QPlayer vs. Qiskit / Cirq / cuQuantum. See its own README. |
| `benchmarks/noise/` | Python | Standalone harness (`comparison.py`) that checks how faithfully QPlayer's device noise reproduces IBM fake backends. See its own README. |

Each file's module docstring / header comment describes its parts in detail —
read the top of any file for specifics. The `benchmarks/` subdirectories are
self-contained report-only tools with their own README each; they are
measurement runners, not part of the CTest suite.

---

## 2. C++ — Compile & Run

The C++ example is **not** built by default. Enable it with the CMake option
`QPLAYER_BUILD_EXAMPLES=ON`; it links the `qplayer_core` static library.

```bash
# From the repository root
cmake -B build -DCMAKE_BUILD_TYPE=Release -DQPLAYER_BUILD_EXAMPLES=ON
cmake --build build --target run_qasm
```

The executable lands at `build/examples/run_qasm`.

```bash
# Bell circuit, 1000 shots, fixed seed
./build/examples/run_qasm tests/data/qasm/bell.qasm --shots 1000 --seed 1
```

When `QPLAYER_BUILD_TESTS=ON` (the default), the example is also registered as
the CTest smoke test `example_run_qasm_bell`, so `ctest --test-dir build`
exercises it automatically.

### CLI options (`run_qasm.cpp`)

| Option | Description |
|--------|-------------|
| `--backend NAME` | `fullstate` \| `sparse` \| `tn` (default `fullstate`) |
| `--proc PROC` | `cpu` \| `gpu` \| `auto` processor for FullState (default `cpu`; `gpu` needs a CUDA build else CPU fallback; `auto` picks GPU when available. `sparse`/`tn` are CPU-only; `--interactive` forces `cpu`) |
| `--shots N` | measurement shots (default 1024) |
| `--seed S` | PRNG seed for reproducible sampling |
| `--opt-level L` | transpile optimization level 0..3 (enables transpile) |
| `--statevector` | print the final statevector (oracle `.data` format) |
| `--draw` | print the circuit as a Unicode text diagram |
| `--draw-ascii` | print the circuit diagram in ASCII style |
| `--expect PAULI` | print `<PAULI>` expectation (repeatable, e.g. `--expect ZZ`) |
| `--no-fusion` | disable runtime gate fusion |
| `--fusion-width W` | gate fusion max width (default 4) |
| `--threads N` | OpenMP thread count |
| `--interactive` | replay the circuit op-by-op via `qr.interactive` (rejects `IfOp` / `IdleOp` and the `tn` backend) |
| `-h`, `--help` | show help |

**Exit codes** (so shell callers can branch): `0` OK · `1` parse/circuit/verify
failure · `2` bad command line · `3` unsupported backend / feature.

---

## 3. Python — Run

The Python examples need the QPlayer Python module. Either install it, or point
`PYTHONPATH` at the in-tree bindings.

```bash
# Option A — install (builds the pybind11 module)
pip install .

# Option B — run against the in-tree bindings without installing
export PYTHONPATH=bindings
```

Then run any demo:

```bash
python examples/python_demo.py
python examples/noise_demo.py
python examples/run_qasm.py tests/data/qasm/bell.qasm --shots 1000 --seed 1
python examples/run_qasm.py tests/data/qasm/bell.qasm --interactive --seed 1
python examples/run_qasm.py tests/data/qasm/bell.qasm --draw
```

`run_qasm.py` mirrors the C++ flags above, with these Python-specific notes:

- `--threads N` is applied via `OMP_NUM_THREADS` **before** `qplayer` is imported
  (OpenMP reads it at library load), since the binding exposes no
  `set_omp_threads`.
- `--statevector` uses `get_statevector_nonzero()` — no 25-qubit
  densify cap, so SparseState circuits up to 128 qubits work.
- `--draw-image PATH` saves a matplotlib diagram (needs the `viz` extra).

### Optional dependencies (extras)

Some demos need optional packages. Install the matching extra:

| Demo | Extra | Install |
|------|-------|---------|
| `qiskit_plugin_demo.py` | `qiskit` (>= 1.1, 1.x or 2.x) | `pip install "qplayer[qiskit]"` |
| `pennylane_plugin_demo.py` | `pennylane` (>= 0.35) | `pip install "qplayer[pennylane]"` |
| `run_qasm.py --draw-image` | `viz` (matplotlib >= 3.5) | `pip install "qplayer[viz]"` |

The core import and `Circuit.draw(output="text")` never need any extra.

---

## 4. Tips

- **Backend availability** depends on how the library was built and the current
  development phase. If a backend is unavailable you'll see a clear message —
  fall back to `--backend fullstate`.
- **Reproducibility**: always pass `--seed` for deterministic sampling. Note
  that GPU parallel-CDF sampling is not bit-identical to CPU.
- **Native builder vs. QASM**: use `python_demo.py` as the template for building
  circuits programmatically; use `run_qasm.{cpp,py}` when you already have QASM
  text/files.
- **Bundled circuits** for quick experiments live in
  [`tests/data/qasm/`](../tests/data/qasm/) (e.g. `bell.qasm`, `ghz_5.qasm`,
  `qft_4.qasm`).
