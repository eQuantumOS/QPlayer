# Installation

QPlayer is built from source. It is distributed **only** as a release source
tarball attached to GitHub Releases — there is no PyPI package, no pre-built
binary or wheel, and no package-manager (vcpkg / Conan) integration. Dependency
resolution is `find_package` only, with pybind11 as the sole `FetchContent`
fallback. PyPI / wheel distribution is out of scope and would require
a separate design decision.

This document covers Ubuntu Server 22.04 LTS and 24.04 LTS on x86_64. MSVC, ICC,
AppleClang, macOS, Windows, and ARM64 are not supported.

---

## 1. System requirements

| Requirement       | Minimum version           | Notes                                                                 |
|-------------------|---------------------------|-----------------------------------------------------------------------|
| OS                | Ubuntu 22.04 / 24.04 LTS  | x86_64 only. Other Linux distributions may work but are unsupported.  |
| C++ compiler      | GCC ≥ 11 or Clang ≥ 13    | C++17 with full `<filesystem>` support required (`std::optional` / `std::variant` are used heavily). |
| CMake             | ≥ 3.20                    | The build script uses `target_compile_definitions` generator expressions and `FetchContent_MakeAvailable`. |
| OpenBLAS | ≥ 0.3.20 | BLAS / LAPACK provider. Install via `apt`. **MKL is forbidden** (GPL v3 incompatibility). |
| Python (optional) | ≥ 3.10 | Only needed for the Python bindings (install with `pip install.`). |
| pybind11 (opt.) | ≥ 2.10 | If not present system-wide, CMake fetches v2.11.1 automatically. |
| CUDA (optional) | Toolkit ≥ 12.0, sm_75+ | Only needed for the FullState GPU backend. Compute Capability 7.5 (Turing) or higher. |

---

## 2. Install system dependencies

```bash
sudo apt update
sudo apt install -y \
    build-essential   `# gcc / g++` \
    cmake             `# >= 3.20 on 22.04, >= 3.28 on 24.04` \
    pkg-config        `# helps FindOpenBLAS locate cblas.h` \
    libopenblas-dev   `# OpenBLAS headers + library`
```

Optional packages:

```bash
# Python bindings:
sudo apt install -y python3 python3-dev python3-pip
pip install --user build pybind11

# CUDA backend:
# Follow https://developer.nvidia.com/cuda-downloads — use the runfile or .deb
# installer for the matching Ubuntu version. Toolkit 12.0 or newer required.
```

Verify the toolchain versions:

```bash
g++ --version       # expect 11.x or newer
cmake --version     # expect 3.20 or newer
pkg-config --modversion openblas   # expect 0.3.20 or newer
python3 --version   # expect 3.10 or newer (only for the Python bindings)
nvcc --version      # expect 12.0 or newer (only for the GPU build)
```

---

## 3. Configure and build (C++)

This guide assumes you downloaded the release source tarball
`QPlayer-<codename>-<x.y.z>.tar.gz` from the GitHub Releases page (e.g.
`QPlayer-Lynx-6.0.0.tar.gz`). A `-clean.tar.gz` variant — identical code with
internal design-reference comments removed — is also attached and builds the same
way. Extract it and build from the extracted directory:

```bash
tar -xzf QPlayer-Lynx-6.0.0.tar.gz
cd QPlayer-Lynx-6.0.0          # extracted directory matches the tarball name

# Default: CPU-only, Release build, in-house tests enabled
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -- -j$(nproc)
```

CMake options:

| Option                          | Default   | Description                                                                  |
|---------------------------------|-----------|------------------------------------------------------------------------------|
| `-DCMAKE_BUILD_TYPE=Release`    | `Release` | Use `Debug` for sanitizer / gdb sessions. Benchmarks always require Release. |
| `-DQPLAYER_BUILD_GPU=ON`        | `OFF`     | Compile the FullState GPU backend. Requires CUDA Toolkit ≥ 12.0 (sm_75+).    |
| `-DQPLAYER_BUILD_TESTS=OFF`     | `ON`      | Skip the in-house test framework and unit tests.                             |
| `-DQPLAYER_BUILD_EXAMPLES=OFF` | `ON` | Build the C++ usage examples in `examples/` (the `run_qasm` CLI). Pass `=OFF` to skip. See. |

To use QPlayer from Python, see (`pip install.`) — it handles the binding build for you.

> **The C++ build tree (`build/`) and the Python install are independent.**
> `pip install .` runs its *own* CMake build through scikit-build-core; it does
> **not** reuse `build/` or inherit any flag you passed to `cmake -B build` above.
> In particular, `-DQPLAYER_BUILD_GPU=ON` here enables the GPU backend only for the
> C++ library and `ctest` — the Python module stays **CPU-only** unless you pass the
> GPU flag to `pip` as well. A plain `pip install.` after a GPU `cmake` build
> still yields a CPU-only module.

---

## 4. Install the Python package

The Python bindings are compiled on demand by `pip` via the bundled
`pyproject.toml` (scikit-build-core backend). There is no PyPI release;
you install directly from the extracted source directory.

```bash
# From the extracted source directory (CPU-only build, default):
pip install .

# GPU build — you MUST forward the CMake flag to pip; the `cmake -B build`
# flags do NOT carry over to this build. Add --force-reinstall when replacing an
# existing CPU-only install, otherwise pip may skip the rebuild and keep it:
pip install . --config-settings=cmake.define.QPLAYER_BUILD_GPU=ON --force-reinstall
```

`pyproject.toml` is preconfigured so that `pip install .` builds only the
extension module (the test suite is skipped). OpenBLAS and a C++17 compiler must
be present (); pybind11 is fetched automatically if not installed
system-wide.

Verify the install:

```bash
python -c "from qplayer import QRegister, Circuit, SimType; print(QRegister)"
```

For a GPU install, also confirm the extension was actually compiled with CUDA —
a CPU-only module loads fine but silently falls back at runtime:

```bash
# Prints linked CUDA libraries (libcudart, libcublas, …); empty = CPU-only build
python -c "import qplayer,glob,os;print(glob.glob(os.path.dirname(qplayer.__file__)+'/_qplayer*.so')[0])" \
    | xargs ldd | grep -i cuda || echo "CPU-only build (no CUDA linked)"
```

> **Common GPU pitfall.** Building the C++ tree with `cmake -B build
> -DQPLAYER_BUILD_GPU=ON` and then running a plain `pip install .` produces a
> **CPU-only** Python module — the two builds are independent. The symptom is
> the runtime warning `GPU backend requested but no usable CUDA device detected
> (CUDA build disabled or no GPU present); falling back to CPU.` even on a host
> with a working GPU. Fix: reinstall with the GPU config-setting shown above
> (`--config-settings=cmake.define.QPLAYER_BUILD_GPU=ON --force-reinstall`).

> **No sdist / PyPI distribution.** `python -m build --sdist` still
> works as a local build, but the project does not publish or attach an sdist —
> the source tarball you already downloaded *is* the distribution, and
> `pip install .` against it produces the identical wheel.

---

## 5. Run tests

```bash
ctest --test-dir build --output-on-failure
```

CTest registers each test *binary* as a single entry (`qplayer_tests`,
`qplayer_acceptance`), so the summary shows two lines. The individual
`[ RUN ] / [ OK ]` cases run *inside* those binaries. `--output-on-failure`
only prints a binary's console log when it fails; to always see the per-case
log, raise the verbosity:

```bash
ctest --test-dir build                     # summary only (2 binaries)
ctest --test-dir build --output-on-failure # per-binary log on failure only
ctest --test-dir build -V                  # full per-case log, always
ctest --test-dir build -VV                 # -V plus CTest-internal detail
ctest --test-dir build -V -R qplayer_tests # verbose, one binary only
```

When the CUDA backend is built (`-DQPLAYER_BUILD_GPU=ON`) and a GPU is present,
FullState correctness cases run on **both** CPU and GPU automatically; the
`-V` output distinguishes them by a `_CPU` / `_GPU` suffix. A
CPU-only build runs the `_CPU` cases only.

The in-house test framework lives under `tests/framework/`; unit and integration
tests are in `tests/scenario/unit/` and `tests/scenario/integration/`, and QASM acceptance circuits
in `tests/data/qasm/`. (No external test framework is used — GoogleTest / Catch2 are
intentionally avoided,.)

---

## 6. Run examples

Runnable examples live in [`examples/`](examples/) — see
[`examples/README.md`](examples/README.md) for the full list and CLI flags. The
C++ example is a single OpenQASM v2 runner (`run_qasm`); the Python examples
cover the native circuit builder, noise models, and the Qiskit / PennyLane
plugins. Bundled circuits for quick experiments are in `tests/data/qasm/` (e.g.
`bell.qasm`, `ghz_5.qasm`, `qft_4.qasm`).

> As of `Lynx-v6.4.0` the examples run on every backend — FullState CPU/GPU,
> SparseState and TensorNetwork (MPS) — and cover noise models and Interactive
> mode. Most examples take `--backend` / `--proc` to select the simulator.

### C++ — `run_qasm` CLI

The C++ example is built by default (`-DQPLAYER_BUILD_EXAMPLES=ON`); pass
`-DQPLAYER_BUILD_EXAMPLES=OFF` to skip it. Build the `run_qasm` target:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target run_qasm

# Run the bundled Bell circuit: 1000 shots, fixed seed
./build/examples/run_qasm tests/data/qasm/bell.qasm --shots 1000 --seed 1
```

The executable lands at `build/examples/run_qasm`. It parses a QASM file,
optionally transpiles, runs, and prints counts. It accepts `--backend`
(`fullstate` | `sparse` | `tn`), `--opt-level`, `--statevector`, `--draw`,
`--expect PAULI`, and more — run `./build/examples/run_qasm --help` or see
`examples/README.md `. When `-DQPLAYER_BUILD_TESTS=ON` (default), this same
runner is also registered as the CTest smoke test `example_run_qasm_bell`.

### Python — native circuit builder

After installing the Python package, run the bundled demos:

```bash
python examples/python_demo.py      # Bell / GHZ(5) / expectation, built in code
python examples/run_qasm.py tests/data/qasm/bell.qasm --shots 1000 --seed 1
```

`python_demo.py` builds circuits with the native `Circuit` builder — for
example, a GHZ state:

```python
import qplayer

qr = qplayer.QRegister(qplayer.SimType.FullState)

circ = qplayer.Circuit(5, 5)
circ.h(0)
for i in range(4):
    circ.cx(i, i + 1)
for i in range(5):
    circ.measure(i, i)

result = qr.run(circ, nshots=10000)
print(result.counts())
# Expected: ~5000 "00000", ~5000 "11111"
```

### OpenQASM v2 input

Both runners accept any OpenQASM v2 file. The bundled `tests/data/qasm/bell.qasm`:

```qasm
OPENQASM 2.0;
include "qelib1.inc";

qreg q[2];
creg c[2];

h q[0];
cx q[0], q[1];

measure q -> c;
```

Run it on a different backend, for example SparseState:

```bash
./build/examples/run_qasm tests/data/qasm/bell.qasm --backend sparse --shots 1000
```

---

## 7. Troubleshooting

- **`Could NOT find OpenBLAS`** — install `libopenblas-dev` and re-run `cmake -B build`. If installed in a non-standard prefix, append `-DOpenBLAS_INCLUDE_DIR=<prefix>/include -DOpenBLAS_LIBRARY=<prefix>/lib/libopenblas.so`.
- **`GCC <ver> is too old`** — Ubuntu 22.04 ships GCC 11.x by default; older systems need `sudo apt install gcc-11 g++-11` and `CC=gcc-11 CXX=g++-11 cmake -B build`.
- **`Unsupported compiler: MSVC / ICC / AppleClang`** — these toolchains are intentionally rejected. Use GCC ≥ 11 or Clang ≥ 13 on Ubuntu.
- **CUDA detection fails** — verify `nvcc --version` and `nvidia-smi`. Pass `-DCMAKE_CUDA_COMPILER=$(which nvcc)` if CMake cannot find it automatically. Without a CUDA toolkit, requesting the GPU backend at runtime falls back to FullState CPU with a warning (no error).
- **GPU build aborts with "CUDA … does not support GCC 13+" (or, on older trees, a flood of `_Float32 / _Float64 / _Float128 does not name a type`)** — the selected nvcc is too old for the host GCC. CUDA < 12.4 cannot parse GCC 13+ glibc headers, so **GCC 13+ (Ubuntu 24.04 default) needs CUDA ≥ 12.4**. The apt `nvidia-cuda-toolkit` package ships nvcc 12.0. As of the build auto-selects the newest toolkit `find_package(CUDAToolkit)` resolves (typically `/usr/local/cuda-*`), so when a newer toolkit is also installed the default `cmake -B build -DQPLAYER_BUILD_GPU=ON` just works; if only CUDA 12.0 is present it fails fast with the message above instead of the cryptic flood. To override explicitly, point CMake at a specific nvcc — `cmake -B build -DQPLAYER_BUILD_GPU=ON -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.6/bin/nvcc` — or `export CUDACXX=/usr/local/cuda-12.6/bin/nvcc` once. (Removing the conflicting apt package with `sudo apt remove nvidia-cuda-toolkit` is the permanent fix.) A stale `build/` can cache a wrong `CMAKE_CUDA_COMPILER`; delete `build/` and reconfigure.
- **`pip install.` cannot find a compiler / OpenBLAS** — the Python build uses the same toolchain as the C++ build; install the dependencies first.