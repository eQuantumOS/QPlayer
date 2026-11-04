# QASM Acceptance Cases

This file describes the nine fixed QASM cases run by `qplayer_acceptance`, how
their expected results are stored, and how the runner decides its exit code.
See [tests/README.md](../../README.md) ("A QASM acceptance case" and "QASM
Acceptance Runner") for how to add a case and how
the runner fits into CTest.

---

## 1. The nine cases

| Label | QASM file | Expected-result JSON | Check (`kind`) | `phase_gate` |
|-------|-----------|----------------------|----------------|--------------|
| `bell`        | `tests/data/qasm/bell.qasm`        | `expected/bell.json`        | `counts`           | Phase 5 |
| `ghz_5`       | `tests/data/qasm/ghz_5.qasm`       | `expected/ghz_5.json`       | `counts`           | Phase 5 |
| `qft_4`       | `tests/data/qasm/qft_4.qasm`       | `expected/qft_4.json`       | `statevector`      | Phase 5 |
| `oracle_10`   | `tests/data/qasm/oracle_10.qasm`   | `expected/oracle_10.json`   | `statevector_dump` | Phase 5 |
| `oracle_20`   | `tests/data/qasm/oracle_20.qasm`   | `expected/oracle_20.json`   | `statevector_dump` | Phase 5 |
| `opt_cancel`  | `tests/data/qasm/opt_cancel.qasm`  | `expected/opt_cancel.json`  | `circuit_metric`   | Phase 4 |
| `noisy_bell`  | `tests/data/qasm/noisy_bell.qasm`  | `expected/noisy_bell.json`  | `fidelity`         | Phase 8 |
| `sparse_100q` | `tests/data/qasm/sparse_100q.qasm` | `expected/sparse_100q.json` | `counts_sparse`    | Phase 7 |
| `compile_swap`| `tests/data/qasm/compile_swap.qasm`| `expected/compile_swap.json`| `counts`           | Phase 8 |

`phase_gate` is the development phase in which the case started to pass; the
runner prints it next to each result. In this release every case is expected
to pass (`expected_fail=false`).

`circuit_metric` (`opt_cancel`) does not run a backend: it calls
`transpile(circuit, optimization_level=1)` and checks `circuit.num_ops()`. The
other eight cases check the distribution, statevector or fidelity returned by
`QRegister::run()`.

`statevector_dump` compares the final statevector of the oracle circuits
amplitude by amplitude. Fidelity would tolerate a global phase, but the purpose
here is to catch regressions in one fixed simulation path, so the amplitudes are
compared directly. The two oracle cases share the kind but store their
reference differently:

| Case | Qubits | States | Storage | Files |
|------|--------|--------|---------|-------|
| `oracle_10` | 10 | 1,024 | inline `expected_amplitudes` in the JSON | `expected/oracle_10.json` |
| `oracle_20` | 20 | 1,048,576 | `data_file` pointing at a text dump | `expected/oracle_20.json` + `expected/oracle_20.data.tar.gz` |

The two storage fields (`expected_amplitudes`, `data_file`) are mutually
exclusive: exactly one must be present. A sibling `.data` file is preferred once
the state count reaches about 2^14, where an inline array becomes unwieldy.
The `oracle_20.data` dump is 83 MB of text, so it is stored as
`oracle_20.data.tar.gz` and extracted into `<build>/tests/acceptance_data/` by
the `qplayer_acceptance_data` build target. The runner looks for the data file
next to the JSON first and then in that directory
(`QPLAYER_ACCEPTANCE_EXTRACT_DIR` overrides it).
Both oracle circuits exercise all 25 gates expressible directly in `qelib1.inc`,
plus `barrier`.

## 2. Exit code

| Situation | Exit code |
|-----------|-----------|
| Every case is `PASS`, or `expected_fail=true` and `NOT_IMPLEMENTED` | **0** |
| Any case is `FAIL`, `UNEXPECTED_PASS` or `UNEXPECTED_FAIL` | 1 |
| Any case with `expected_fail=false` is `NOT_IMPLEMENTED` | 1 |

CTest uses the runner's exit code directly; no output regex is involved.

## 3. Expected-result schema per kind

| `kind` | Required fields |
|--------|-----------------|
| `counts` | `nshots`, `tolerance`, `expected_counts` (probabilities in [0, 1]) |
| `statevector` | `nshots` (= 1), `tolerance_l2`, `expected_statevector_uniform_amplitude`, `expected_state_dimension` |
| `statevector_dump` | `num_qubits`, `num_states`, `tolerance_l2`, `tolerance_per_amplitude`, plus exactly one of ① `expected_amplitudes` — an array of `num_states` `[re, im]` pairs indexed by the bitstring read as an unsigned integer, MSB first (`oracle_10`) — or ② `data_file` — the name of a sibling text dump with `num_states` lines of the form `[P=<%.6f>] [<%+.12e>, <%+.12e>] \|<bitstring>>` (`oracle_20`) |
| `circuit_metric` | `metric` (= `"num_ops_after_transpile"`), `optimization_level`, `expected_value` |
| `fidelity` | `nshots`, `tolerance`, `noise_model`, `expected_fidelity` |
| `counts_sparse` | `nshots`, `tolerance`, `expected_nonzero_amplitude_count`, `expected_counts` |

How `statevector_dump` is compared:

1. The JSON is loaded and the storage field selects the source:
   - `expected_amplitudes`: the array length must equal `num_states`; the
     `[re, im]` pairs become a `std::vector<std::complex<double>>`.
   - `data_file`: the sibling `.data` file is read, its one-line header
     (`============== dump <N> states =============`) skipped, and each of the
     `num_states` body lines parsed. A line count other than `num_states`, or a
     bitstring length other than `num_qubits`, is a `FAIL`.
   - Both fields present, or neither: `FAIL` (schema violation).
2. The result of `QRegister::get_statevector()` is compared amplitude by
   amplitude; the first index with `|actual[i] - expected[i]| >
   tolerance_per_amplitude` is reported and the case fails.
3. If the overall L2 difference `sqrt(Σ |actual[i] - expected[i]|²)` exceeds
   `tolerance_l2`, that is reported as well.
4. In verbose mode the `.data` path also checks `|P − (re² + im²)| < 5e-7` on
   every line, a self-consistency guard on the dump file itself.
