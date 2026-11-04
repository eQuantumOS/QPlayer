# Device-noise validation harness — `comparison.py`

A standalone console tool that checks **how faithfully QPlayer's noise reproduces
real IBM Quantum hardware**. It runs directly from this directory and needs no
files outside `examples/benchmarks/`.

> This is a report-only validation tool: it does not build, modify, or commit
> anything. It writes stdout tables (single-combination mode) or a markdown
> report plus PNG plots (`--report` mode).

---

## 1. What it compares

For one (fake backend, circuit) pair it produces three targets side by side:

| Target        | What it is                                                                 |
|---------------|----------------------------------------------------------------------------|
| **Ideal**     | noiseless run (QPlayer, no noise model) — the baseline                      |
| **FakeBackend** | `AerSimulator.from_backend(<fake backend>)` — a faithful IBM hardware noise reference |
| **QPlayer**   | QPlayer noise assembled entirely by `NoiseModel.load_device` from the committed device JSON (relaxation + excess depolarizing 1Q/2Q + readout) |

Three validation tiers:

| Tier    | Content                                                                       |
|---------|-------------------------------------------------------------------------------|
| Tier 1  | q0 T1 · T2 · T2* (N/A) · T_phi · readout — IBMQ reported vs QPlayer loaded     |
| Tier 1b | per-qubit T1/T2 round-trip; plus a measured T1/T2 decay sweep fit to `exp(-t/T)` per engine (QPlayer `load_device` vs Aer `thermal_relaxation`) |
| Tier 2  | Ideal / FakeBackend / QPlayer distributions + Hellinger fidelity · TVD (3 pairs) + a headline **QPlayer vs IBM agreement %** |

> **Key number = the `FakeBackend vs QPlayer` Hellinger fidelity** — the closer
> to 1.0, the more faithfully QPlayer reproduces the device noise.
>
> **Agreement %** (per experiment, and collected in the Matrix summary) restates
> that key row as two percentages: **distribution overlap `(1 - TVD)`** — the
> fraction of probability mass the two distributions literally share — and the
> **Hellinger fidelity**. Reported as `overlap% / Hellinger%`. Geneva runs Tier 2
> on its active (truncated) qubits, so it also reports an agreement %.

Every backend is optional: a missing package is reported as a SKIP / NOTE, never
a hard failure. The device JSON is committed, so the QPlayer side runs even
without the fake-backend provider (only the FakeBackend column needs it).

---

## 2. Devices and circuits

**Devices** (committed JSON under `device_json/`, all IBM Falcon `{id,rz,sx,x,cx}`):

| Argument | Qubits | Tier 2 (distribution)                                        |
|----------|-------:|:-------------------------------------------------------------|
| `manila` | 5      | ✓                                                            |
| `oslo`   | 7      | ✓                                                            |
| `geneva` | 27     | ✓ (generic circuits truncated to their active qubits — see below) |

The `device` argument accepts `manila` / `oslo` / `geneva`, or a path to a device
JSON file.

Geneva's device width (27q) exceeds `--max-sim-qubits`, so simulating the whole
device per shot is infeasible. A small circuit mapped onto a 27q device references
only a few physical qubits, so QPlayer's own device-bound **`QubitTruncationPass`
** compresses it onto those active qubits (dense `[0,k)`); QPlayer then
simulates `2^k` and `run()` projects each qubit's physical device noise onto that
range (`project_to_dense`), so every active qubit keeps its own T1/T2 / gate
error / readout. Aer prunes idle qubits internally, so it keeps running the full
transpiled circuit as the reference — the comparison stays faithful. The Matrix
summary shows this as `27 → sim N`. Tier 1 / 1b still report all 27 qubits'
coherence parameters. (manila/oslo are ≤ `--max-sim-qubits`, so they are simulated
directly with no truncation.)

**Circuits**, two groups:

| Group           | Location                                  | Applies to            |
|-----------------|-------------------------------------------|-----------------------|
| shared generic  | `qasm/*.qasm` (bell, ghz3, ghz5, x_relax_q0) | all devices        |
| device-specific | `qasm/<device>/*.qasm` (VQE, QAOA)        | that device only (manila 5q, oslo 7q, geneva 12q) |

Each device's ansatz width matches (manila/oslo) or samples (geneva) its device.
manila/oslo ansatze fill the device (5q / 7q). Geneva's ansatz is **partial-width
(12q, `GENEVA_QUBITS`)** — a full 27q ansatz would fill the device and be
unsimulable, so geneva instead exercises a 12-qubit chain of its heavy-hex, which
transpiles + truncates to 12 active qubits. A wider ansatz spreads its
measured distribution over more outcomes (2^12 = 4096), so the distribution tier
auto-scales the shot count up to `SHOTS_PER_STATE (=16) × 2^(active width)`
(geneva 12q → 65536) to keep the FakeBackend-vs-QPlayer agreement score meaningful
rather than shot-noise-limited; `--shots` / `--ansatz-shots` act as floors, so
manila/oslo (2^5 / 2^7 ≪ that) stay at the requested value. With ~1 shot/outcome a
12q ansatz scored Fake-vs-QPlayer ≈0.55 (sampling floor); at 16×2^12 it recovers to
≈0.96, matching the model's true fidelity.

---

## 3. Setup

### 3.1 QPlayer must be current (noise runs on CPU, no GPU flag needed)

```bash
SO=$(python3 -c "import qplayer,glob,os;d=os.path.dirname(qplayer.__file__);print(glob.glob(d+'/_qplayer*.so')[0])")
NEWEST=$(find src include bindings CMakeLists.txt cmake -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cu' -o -name '*.cuh' \
       -o -name '*.cmake' -o -name 'CMakeLists.txt' \) \
    -printf '%T@\n' | sort -nr | head -1)
[ "$(stat -c %Y "$SO")" -ge "${NEWEST%.*}" ] && echo FRESH || echo STALE
# if STALE:
python3 -m pip install . --no-build-isolation --force-reinstall --no-deps
```

### 3.2 Dependencies (all optional — see `requirements.txt`)

| Target      | Package                                             | If missing                                   |
|-------------|-----------------------------------------------------|----------------------------------------------|
| FakeBackend | `qiskit>=2.0` + `qiskit-aer` + `qiskit-ibm-runtime` | FakeBackend column omitted (Ideal + QPlayer still measured) |
| plots       | `matplotlib` (+ `scipy` for the fit)                | plots skipped (tables kept)                  |
| QPlayer     | `qplayer` (current tree)                            | that combination is SKIPped                  |

---

## 4. Running

### 4.1 Single combination → stdout (optional plots)

```bash
cd examples/benchmarks/noise
python comparison.py manila qasm/bell.qasm --shots 1024 --plot-dir /tmp/noiseplots
python comparison.py oslo   qasm/oslo/vqe_7q.qasm --shots 4096 --seed 12345
python comparison.py geneva qasm/ghz5.qasm # truncated to active qubits
```

| Option             | Default  | Meaning                                                     |
|--------------------|----------|-------------------------------------------------------------|
| `<device>`         | —        | `manila` / `oslo` / `geneva` or a device JSON path          |
| `<circuit.qasm>`   | —        | one logical QASM circuit                                    |
| `--shots`          | 1024     | shot FLOOR; dist tier auto-scales to 16×2^(active width)     |
| `--seed`           | (random) | Monte-Carlo / transpiler seed (printed if random, for replay)|
| `--max-sim-qubits` | 14       | skip the distribution tier above this transpiled width      |
| `--plot-dir DIR`   | (none)   | write `<dev>_T1T2.png` and `<dev>_<circuit>.png` into DIR   |

### 4.2 Full matrix → markdown report + PNG plots (no editing)

```bash
cd examples/benchmarks/noise
python comparison.py --report --shots 1024 --ansatz-shots 4096
```

This sweeps every device × circuit combination and writes, into `result/`:

- `result/noise-validation-<date>-<time>.md` — the full report (environment,
  matrix summary, per-device T1/T2 decay plot, per-combination tiers + plots),
- `result/plots-<date>-<time>/` — `<device>_T1T2.png` and
  `<device>_<circuit>.png` for every measured combination.

The report is generated entirely by the tool; no manual editing is needed.

| Option           | Default    | Meaning                                        |
|------------------|------------|------------------------------------------------|
| `--report`       | off        | run the sweep and write report + plots         |
| `--out-dir`      | `./result` | where the report and plots are written         |
| `--shots`        | 1024       | shot floor for generic circuits (auto-scaled)  |
| `--ansatz-shots` | 4096       | shot floor for ansatz; geneva 12q→16×2^12=65536 |
| `--seed`         | (random)   | one seed for the whole sweep (printed for replay)|

---

## 5. Reading the output

- `## Tier 1` table: if T1/T2 match (IBMQ = QPlayer) and the round-trip
  `max|Δ| = 0`, the device JSON loads exactly.
- `# T1/T2 fit (us)`: QPlayer/IBMQ fitted time constants near the nominal value
  mean the relaxation physics agrees.
- `## Tier 2` agreement: the goal is `FakeBackend vs QPlayer (key)` Hellinger
  fidelity → 1.0. The `ideal vs *` rows show how far each noisy run departs from
  the noiseless distribution. The `# QPlayer vs IBM (FakeBackend) agreement:`
  line below restates the key row as `overlap% / Hellinger%`.
- Geneva prints a `# NOTE:` line stating that `QubitTruncationPass`
  compressed the circuit onto its active qubits, then reports Tier 2 on those.

---

## 6. Regenerating the input data (normally not needed — the committed files are the source of truth)

```bash
# device JSON (re-extract calibration; requires qiskit-ibm-runtime)
python gen_device_json.py [manila|oslo|geneva]
# VQE/QAOA ansatz circuits (no external dependency, deterministic)
python gen_ansatz_qasm.py
```

---

## 7. Notes

- `device_json/` holds the committed calibration; `qasm/` holds the circuits.
- Results are kept under `result/`; each `--report` run uses its own
  `plots-<stamp>/` subdirectory so runs never mix.
- QPlayer's C++ log lines (e.g. `[QPlayer/WARN ]`) go to stderr and do not enter
  the captured report text.
