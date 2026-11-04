# QPlayer Test Suite

This directory holds the QPlayer verification code: the in-house test framework,
the test scenarios that exercise the library, and the fixed input data they run
against. It ships with the **source repository** (it is excluded from the PyPI
sdist), so this document is aimed at anyone who clones the repo to run, extend,
or contribute tests.

QPlayer uses a **self-implemented** test framework — no GoogleTest, Catch2, or
pytest. Everything below is built and run with CMake / CTest and the standard
library only.

---

## 1. Directory Structure

The tree is organized into four categories — **engine**, **scenarios**,
**data**, and **utilities**.

```
tests/
├── framework/            # (engine)     the test framework itself — no test cases
│   ├── test_case.hpp     #   TEST() / TEST_F() registration macros
│   ├── assertions.hpp    #   EXPECT_* / ASSERT / FAIL macros
│   ├── test_runner.hpp   #   registry, glob filter, result aggregation
│   ├── test_runner.cpp
│   └── test_main.cpp     #   main() for the qplayer_tests executable
│
├── scenario/             # (code)       the actual tests
│   ├── unit/             #   per-class / per-function isolated tests (test_<module>.cpp)
│   │   └── test_passes/  #     optimizer Pass unit tests
│   ├── integration/      #   end-to-end pipeline tests (parse → transpile → run)
│   ├── acceptance/       #   QASM circuit acceptance runner
│   └── python/           #   Python-binding tests (stdlib unittest)
│
├── data/                 # (input)      fixed test inputs, never generated at runtime
│   ├── qasm/             #   acceptance QASM circuits + expected/*.json baselines
│   ├── golden/           #   golden measurement-distribution baselines (4 circuits)
│   └── device_json/      #   device-noise model fixtures (e.g. willow_like.json)
│
├── tools/                # (utilities)  standalone dev tools, NOT part of CTest
│   ├── dump_statevector.cpp
│   └── measure_parallel_cdf_crossover.py
│
└── CMakeLists.txt        # builds the qplayer_tests + qplayer_acceptance targets
```

| Category | Directory | Purpose |
|----------|-----------|---------|
| Engine | `framework/` | The test framework. Contains no test cases — only macros and the runner. |
| Scenario | `scenario/unit/` | One module in isolation (a class or function). File per module: `test_<module>.cpp`. |
| Scenario | `scenario/integration/` | Multiple modules together — full pipeline, cross-backend, golden checks. |
| Scenario | `scenario/acceptance/` | Runs fixed QASM circuits and compares against stored expected distributions. |
| Scenario | `scenario/python/` | Exercises the pybind11 bindings and the Qiskit/PennyLane plugins. |
| Data | `data/qasm`, `data/golden`, `data/device_json` | Immutable input files. Paths are injected into the test binaries at build time. |
| Utilities | `tools/` | Standalone developer utilities (state dumps, crossover measurement). Not registered with CTest. |

---

## 2. Building and Running the Tests

### 2.1 Build

Tests are built by default (`QPLAYER_BUILD_TESTS=ON`). From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces two CTest-registered executables under `build/tests/`:

| Executable | What it runs |
|------------|--------------|
| `qplayer_tests` | All C++ unit + integration + golden cases in a single binary. |
| `qplayer_acceptance` | The QASM acceptance runner. |

To build without tests: `cmake -S . -B build -DQPLAYER_BUILD_TESTS=OFF`.
To include GPU test cases, configure with the GPU build enabled
(`-DQPLAYER_BUILD_GPU=ON`); GPU cases are skipped automatically when no CUDA
device is present.

### 2.2 Run via CTest

```bash
ctest --test-dir build --output-on-failure
```

Run a single target:

```bash
ctest --test-dir build -R qplayer_tests
ctest --test-dir build -R qplayer_acceptance
```

The runner exits `0` **iff** only passed + expected-fail results exist, so CTest
uses the process exit code directly (no output-regex matching).

#### Controlling log verbosity

CTest registers each test *binary* as a single entry, so the default summary
shows just two lines (`qplayer_tests`, `qplayer_acceptance`). The individual
`[ RUN ] / [ OK ]` cases — and the `AcceptanceRunner` summary — are printed by
the binaries themselves and are captured by CTest, but `--output-on-failure`
surfaces a binary's log **only when it fails**. To always see the per-case log,
raise the verbosity level:

| Command | What you see |
|---------|--------------|
| `ctest --test-dir build` | Summary only (2 binaries pass/fail) |
| `ctest --test-dir build --output-on-failure` | Summary + a binary's full log **only if it fails** |
| `ctest --test-dir build -V` | Summary + **every** binary's full console log (all `[ RUN ]/[ OK ]` lines, acceptance summary) |
| `ctest --test-dir build -VV` | `-V` plus CTest-internal detail (command line, environment) |

```bash
ctest --test-dir build -V                    # full per-case log for both binaries
ctest --test-dir build -V -R qplayer_tests    # verbose, one binary only
ctest --test-dir build -V -R qplayer_acceptance
```

In `-V` output each line is prefixed with its test index (`1:` = `qplayer_tests`,
`2:` = `qplayer_acceptance`). If a passed binary's captured log ever looks
truncated, lift the cap with `--test-output-size-passed 0`. The same per-case
log is available by running a binary directly.

Backend-parameterized cases appear with a `_CPU` / `_GPU` suffix
the `-V` output: a CPU-only build runs the `_CPU` cases only, while a CUDA build
(`-DQPLAYER_BUILD_GPU=ON`) with a visible GPU runs **both** `_CPU` and `_GPU`
variants —.

### 2.3 Run a subset of C++ cases

The `qplayer_tests` binary accepts a glob filter, useful during development:

```bash
build/tests/qplayer_tests --filter=FullState.*                       # one suite
build/tests/qplayer_tests --filter=FullState.BellState_MeasureBothQubits_CollapsesCorrectly  # one case
```

### 2.4 Run the Python tests

The Python tests use the standard-library `unittest` module — **pytest is not a
dependency**. They require the built `qplayer` extension: either `pip install .`
into the active environment, or rely on the fallback in each test that inserts
`bindings/` onto `sys.path` when `qplayer` is not already importable.

```bash
python tests/scenario/python/test_primitives.py
python tests/scenario/python/test_draw.py
```

Plugin tests (`test_qiskit_*.py`, `test_pennylane_*.py`) additionally need the
matching optional dependency (`pip install .[qiskit]` or `.[pennylane]`); they
skip themselves when the dependency is absent.

---

## 3. Adding a New Test Case

### 3.1 A C++ unit test (the common case)

1. **Create the file** `tests/scenario/unit/test_<module>.cpp`. Include the
   framework and the code under test with the correct relative depth (from
   `scenario/unit/`, the framework is two levels up and `include/` is three):

   ```cpp
   #include "../../framework/test_case.hpp"
   #include "../../framework/assertions.hpp"

   #include "../../../include/qregister.hpp"

   using qplayer::QRegister;
   using qplayer::SimType;
   using qplayer::Circuit;
   ```

   > A test under `scenario/unit/test_passes/` is one level deeper — use
   > `../../../framework/...` and `../../../../include/...`.

2. **Write the case** with the `TEST(suite, name)` macro. Follow the
   **Arrange → Act → Assert** structure, separated by blank lines, and name it
   `Module.Behavior_Condition_ExpectedResult`:

   ```cpp
   TEST(FullState, BellState_MeasureBothQubits_CollapsesCorrectly) {
       // Arrange
       QRegister qr(SimType::FullState);
       Circuit circ(2, 2);
       circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);

       // Act
       auto result = qr.run(circ, 10000);

       // Assert
       auto counts = result.counts();
       double p00 = static_cast<double>(counts["00"]) / 10000.0;
       EXPECT_NEAR(p00, 0.5, 0.02);  // nshots=10000 statistical tolerance
   }
   ```

3. **Register the file** in [`tests/CMakeLists.txt`](CMakeLists.txt) by adding it
   to the `qplayer_tests` `add_executable(...)` source list:

   ```cmake
   scenario/unit/test_<module>.cpp
   ```

   This is mandatory — a test file that is not listed is never compiled or run.

4. **Rebuild and run**: `cmake --build build && ctest --test-dir build`.

### 3.2 A fixture-based test

Use `TEST_F(Fixture, name)` when cases share setup/teardown. Derive from
`qplayer::test::TestFixture` and override `SetUp()` / `TearDown()`; teardown runs
even if the body throws.

```cpp
class SeedFixture : public qplayer::test::TestFixture {
public:
    void SetUp() override    { ::unsetenv("QPLAYER_SEED"); }
    void TearDown() override { ::unsetenv("QPLAYER_SEED"); }
};

TEST_F(SeedFixture, SetSeed_SameSeed_ProducesSameStream) { /* ... */ }
```

### 3.3 A QASM acceptance case

1. Add the circuit at `tests/data/qasm/<name>.qasm`.
2. Add the expected result at `tests/data/qasm/expected/<name>.json`.
3. Register the `TestCase` in `scenario/acceptance/run_acceptance.cpp` with its
   `phase_gate` and `expected_fail` flag (see the `expected_fail` policy below).

### 3.4 Choosing an `EXPECT_NEAR` tolerance

Floating-point comparisons must use `EXPECT_NEAR(actual, expected, tol)` (never
`EXPECT_EQ`) and justify the tolerance in a comment:

| Tolerance | Meaning |
|-----------|---------|
| `1e-10` | Exact value from a closed-form derivation (not a statistical quantity). |
| `1e-6`  | Accumulated numerical error is acceptable. |
| `0.02`  | Statistical tolerance for a sampled distribution (e.g. `nshots=10000`). |

---

## 4. Framework Macro Reference

Registration (`framework/test_case.hpp`):

| Macro | Purpose |
|-------|---------|
| `TEST(suite, name)` | Standalone test case. |
| `TEST_F(Fixture, name)` | Fixture-based test with `SetUp` / `TearDown`. |
| `TEST_BACKENDS(suite, name)` | Backend-parameterized case (`framework/backend_params.hpp`). Runs once per proc in `testable_procs()`; the body receives a `ProcType proc`. |
| `EXPECTED_FAIL_TEST(suite, name)` | A case that is expected to fail (stub / not-yet-implemented), counted as a pass. |

### 4.1 Backend-parameterized tests (`TEST_BACKENDS`)

`QRegister`'s default `ProcType::Auto` prefers the GPU when a CUDA build sees a
device. A plain `TEST()` that constructs
`QRegister(SimType::FullState)` therefore exercises the **CPU** backend in a
CPU-only build but the **GPU** backend in a CUDA build — leaving the CPU path
untested there. `TEST_BACKENDS` closes that gap: write the case once and it is
registered once per proc returned by `qplayer::test::testable_procs()`.

- `testable_procs()` always contains `ProcType::CPU`, and adds `ProcType::GPU`
  only when the build defines `QPLAYER_BUILD_GPU` **and** `qplayer_cuda_device_count() > 0`.
  So: CPU-only build → `{CPU}`; CUDA build with a GPU → `{CPU, GPU}`; CUDA build
  with no GPU present → `{CPU}`.
- Each instance is registered as `suite.name_CPU` / `suite.name_GPU`, so the two
  variants report separately (visible with `ctest -V`).

```cpp
// One source case → Golden.Bell_Distribution_CPU (+ _GPU in a CUDA build).
TEST_BACKENDS(Golden, Bell_Distribution) {
    QRegister qr(SimType::FullState, proc);   // `proc` is injected by the macro
    Circuit circ(2, 2);
    circ.h(0).cx(0, 1).measure(0, 0).measure(1, 1);
    auto counts = qr.run(circ, 10000).counts();
    EXPECT_NEAR(static_cast<double>(counts["00"]) / 10000.0, 0.5, 0.02);
    EXPECT_NEAR(static_cast<double>(counts["11"]) / 10000.0, 0.5, 0.02);
}
```

Use `TEST_BACKENDS` for FullState correctness tests whose assertions hold on
both backends (statistical distributions, statevector equality within `1e-10`,
expectation values). Keep a plain `TEST()` with an explicit `ProcType::CPU` for
cases that assert **exact** sample counts under a fixed seed — GPU sampling uses
a parallel CDF scan that is not bit-identical to the CPU stream, e.g.
`Golden.Clifford10_ExactCounts_SeedDeterministic`. Backend-agnostic tests
(circuit drawing, parsing, IR, optimizer passes) stay on plain `TEST()`.

Assertions (`framework/assertions.hpp`):

| Macro | Purpose |
|-------|---------|
| `EXPECT_EQ(a, b)` / `EXPECT_NE(a, b)` | (In)equality — not for floating-point. |
| `EXPECT_NEAR(a, b, tol)` | Floating-point comparison within a tolerance. |
| `EXPECT_TRUE(x)` / `EXPECT_FALSE(x)` | Boolean checks. |
| `EXPECT_THROW(expr, ExcType)` | Asserts `expr` throws `ExcType`. |
| `EXPECT_NO_THROW(expr)` | Asserts `expr` does not throw. |
| `EXPECT_NOT_IMPLEMENTED(expr)` | Asserts `expr` throws `NotImplementedError` (stub phases). |
| `FAIL(message)` | Unconditional failure with a message. |

---

## 5. Golden Tests

Golden tests pin the measurement distribution of four reference circuits against
stored baselines in `tests/data/golden/`:

| Circuit | Qubits | nshots | Baseline |
|---------|--------|--------|----------|
| Bell state | 2 | 10,000 | `bell_state.json` |
| GHZ | 5 | 10,000 | `ghz_5q.json` |
| QFT | 4 | 1 | `qft_4q.json` |
| Random Clifford | 10 | 1,000 | `clifford_10q.json` |

A golden baseline changes **only** for an algorithm or bug fix, never for a
refactor or performance optimization — if a baseline shifts after a non-semantic
change, that is a numerical bug. Updating a baseline requires a documented
decision, an archived copy of the old file, and a change-log entry.

---

## 6. QASM Acceptance Runner

`qplayer_acceptance` runs the fixed circuits in `tests/data/qasm/` and classifies
each into a five-state status — `PASS`, `FAIL`, `NOT_IMPLEMENTED`,
`UNEXPECTED_PASS`, `UNEXPECTED_FAIL`. Each case carries an `expected_fail` flag
and a `phase_gate` label so that circuits gated to a later development phase can
be registered as expected-to-fail today and flipped to a hard pass when that
phase lands. The process exits `0` when only passing and expected-fail results
remain.

---

## 7. Conventions and Policies

- **No external test libraries.** GoogleTest, Catch2, doctest, and pytest are
  prohibited. Mocks, when needed, are implemented inside `framework/`.
- **No test-less implementation.** Every implementation commit ships with at
  least one `TEST()`; implementation and test are committed together.
- **Comments in English.** All files under `tests/**` use English comments.
- **Deterministic randomness.** Any randomness uses a fixed seed. The
  `QPLAYER_SEED` environment variable overrides and locks the RNG seed for
  reproducibility; tests that read it must clear it in `SetUp` / `TearDown` to
  stay isolated.
- **One behavior per case.** Keep a `TEST()` focused; split it when it grows past
  a few assertions.
