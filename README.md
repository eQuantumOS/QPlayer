# QPlayer

> High-performance quantum circuit emulator in C++17 — single API for batch and interactive execution across four backends.

QPlayer accepts three input paths (OpenQASM v2 text, C++ native API, Python native API) and lowers them to a common Circuit IR, then executes on the FullState (CPU / GPU), SparseState, or TensorNetwork backend. Both the OpenQASM v2 parser and the unit-test framework are implemented in-house — no external parser or test library dependencies.

- **License**: Dual License — GPL v3 (open source) + Commercial License (commercial users, `gicha@etri.re.kr`). See [LICENSE](LICENSE).
- **Language**: C++17 (core), Python 3.10+ (bindings — progressive introduction from Phase 7).
- **Build**: CMake ≥ 3.20 (no external package manager required).
- **Status**: Phase 8 complete — `Caracal-v5.0.0`. OpenQASM v2 parser, optimizer, FullState CPU/GPU + SparseState backends, Python bindings, and the noise model + DeviceModel + Interactive mode + coupling-map compilation are all in place. TensorNetwork (Phase 9–10) is upcoming. See the milestone roadmap for details.
- **Documentation**: design documents are in Korean — User-facing documents (this README, [INSTALL.md](INSTALL.md), [NOTICE](NOTICE), Doxygen output) are in English.

---

## Key Features

- **Single entry point API** — `QRegister` exposes both batch execution (`run()`) and interactive execution (`interactive.*`).
- **Common Circuit IR** — OpenQASM, C++, and Python inputs all lower to the same IR, executed consistently across all backends.
- **In-house OpenQASM v2 parser** — supports `qelib1.inc` standard gates, custom `gate` / `opaque` definitions, `creg` / `measure` / `reset` / `barrier` / `include` / `if` directives.
- **Multiple backends** — FullState (CPU / GPU), SparseState, TensorNetwork (MPS), selectable via the same API.
- **High-performance CPU path** — AVX2 / AVX-512 SIMD kernels, OpenMP two-level parallelism, cache blocking, CPUID runtime dispatcher.
- **GPU path** — CUDA-resident state vector, cuBLAS GEMV / GEMM, CUDA Streams pipelining (Compute Capability sm_75 or higher).
- **Circuit optimization pipeline** — gate cancellation, commutation reordering, single-qubit merge, gate fusion (up to 4-qubit window).
- **Noise model** — depolarizing, amplitude / phase damping, bit / phase flip, readout error, and arbitrary Kraus channels.
- **In-house test framework** — `TEST` / `EXPECT_*` macros (12 in total) implemented under `tests/framework/`. No GoogleTest, Catch2, or doctest dependency.

---

## Backend Capability Matrix

| Feature              | FullState CPU | FullState GPU | SparseState | TensorNetwork |
|----------------------|:-------------:|:-------------:|:-----------:|:-------------:|
| Noise model          |       ✓       |       ✓       |      ✗      |       ✓       |
| GPU acceleration     |       ✗       |       ✓       |      ✗      |       ✓       |
| Gate fusion (batch)  |       ✓       |       ✓       |      ✗      |       ✓       |
| Interactive mode     |       ✓       |       ✗       |      ✓      |       ✗       |
| Cache blocking       |       ◐       |       ✗       |      ✗      |       ✗       |
| MPS / TN form        |       ✗       |       ✗       |      ✗      |       ✓       |
| Practical qubit cap  |     ~35       |   ~30 (VRAM)  |    ~100+    |    ~100+      |

Legend: ✓ supported · ✗ not supported · ◐ opt-in (cache blocking defaults OFF — net-negative on modern server CPUs,; enable via `QPLAYER_CACHE_BLOCKING=1` or `set_cache_blocking(true)`). Interactive mode is CPU-statevector only; GPU statevector is reachable through the batch `run()` path.

---

## External Dependencies

Only three external dependencies are permitted. Adding a fourth requires updating `` first.

| Dependency      | Purpose                                | Notes                                                                                          |
|-----------------|----------------------------------------|------------------------------------------------------------------------------------------------|
| OpenBLAS        | BLAS / LAPACK operations               | GPL v3 incompatibility.                                                     |
| pybind11 | Python bindings (Phase 7+) | `find_package(pybind11 2.10 QUIET)` with FetchContent v2.11.1 fallback. |
| CUDA Toolkit | GPU acceleration (optional) | sm_75 (Turing) or higher. cuBLAS / cuRAND / cuSolver used for matrix operations. |

System package installs are described in [INSTALL.md](INSTALL.md).

---

## Quick Build

```bash
# Configure (CPU-only, default)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -- -j$(nproc)

# Run tests
ctest --test-dir build
```

Optional flags:

- `-DQPLAYER_BUILD_GPU=ON` — enable CUDA backend (requires CUDA Toolkit 12.0+).
- `-DQPLAYER_BUILD_PYTHON=ON` — build pybind11 module (requires Python 3.10+, available from Phase 7).
- `-DQPLAYER_BUILD_TESTS=OFF` — skip test targets.
- `-DQPLAYER_BUILD_EXAMPLES=OFF` — skip the C++ examples (the `run_qasm` CLI is built by default).

See [INSTALL.md ](INSTALL.md) for the smallest end-to-end Bell / GHZ example.

---

## Repository Layout

```
QPlayer/
├── include/                   # Public + internal headers (C++)
│   ├── circuit/  parser/  backend/  noise/
│   ├── transpiler/passes/{validation,layout,routing,translation,optimization,scheduling,analysis}/
│   └── types.hpp  error.hpp  log.hpp  result.hpp  qregister.hpp  observable.hpp  primitives.hpp
├── src/                       # Implementation (C++ + CUDA)
│   ├── backend/{full_state,sparse_state,tensor_network}/
│   ├── circuit/
│   ├── parser/
│   ├── transpiler/passes/{validation,layout,routing,translation,optimization,scheduling,analysis}/
│   └── noise/
├── bindings/ # Python bindings
│ ├── qplayer/ # Core pybind11 module (: Phase 7+)
│ └── plugins/{qiskit,pennylane}/ # Optional framework plugins (0214)
├── tests/ # In-house test framework + scenario tests + fixtures
│   ├── framework/             # Self-implemented TEST / EXPECT_* macros (test engine)
│   ├── scenario/              # Test code — what is verified
│   │   ├── unit/              # Per-module unit tests
│   │   ├── integration/       # Pipeline end-to-end + Golden 4 (Bell, GHZ, QFT, Random Clifford)
│   │   ├── acceptance/        # QASM acceptance runner
│   │   └── python/            # Python binding tests
│   ├── data/                  # Test inputs / fixtures
│   │   ├── qasm/              # QASM acceptance inputs + expected/*.json
│   │   ├── golden/            # Golden reference JSON
│   │   └── device_json/       # Device-model fixtures (willow, etc.)
│   └── tools/                 # Auxiliary measurement / dump helpers (non-ctest)
├── examples/                  # User examples (C++ + Python)
│   └── benchmarks/{noise,performance}/  # Runnable benchmark harnesses
├── cmake/                     # CMake modules (FindOpenBLAS, detect_cache)
├── CMakeLists.txt
└── pyproject.toml # scikit-build-core ≥ 0.7
```

---

## Development Status

`Caracal-v5.0.0` is complete: on top of the Ocelot foundation (OpenQASM v2 parser, circuit optimizer, FullState CPU/GPU and SparseState backends, Python bindings), the noise model + DeviceModel, Interactive mode, and coupling-map compilation have all landed. TensorNetwork (MPS/SVD) is the next milestone.

---

## Contributing

This is a single-author research project — external contributions are not accepted at this stage. For licensing or commercial inquiries, contact `gicha@etri.re.kr`.

---

## See Also

- [LICENSE](LICENSE) — GPL v3 + Commercial Dual License terms.
- [NOTICE](NOTICE) — third-party attribution.
- [INSTALL.md](INSTALL.md) — system dependency setup, build configuration, optional CUDA path, and a minimal Bell / GHZ example.