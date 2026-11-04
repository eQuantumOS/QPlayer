# CMakeLists.txt — QPlayer root build script.
# Targets Ubuntu 22.04 / 24.04 LTS x86_64 only. Permitted external
# dependencies are OpenBLAS, pybind11, and CUDA Toolkit.
# No external package manager (vcpkg / Conan) is used — find_package only,
# with pybind11 as the sole FetchContent fallback.

cmake_minimum_required(VERSION 3.20)

project(qplayer
    VERSION 6.0.0
    DESCRIPTION "QPlayer (Lynx series, Lynx-v6.0.0 — noise trajectory canonical op order + reset noise, canonical-gauge MPS splits, TN noise-path speedups, trajectory diagnostics and explicit idle-channel APIs) — high-performance quantum circuit emulator"
    LANGUAGES CXX)

# ── 1. Platform and compiler guards ───────────────────────────────
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "QPlayer supports Linux only (Ubuntu 22.04 / 24.04 LTS). "
                        "Detected: ${CMAKE_SYSTEM_NAME}. "
                        "MSVC, ICC, AppleClang, macOS, Windows, and ARM64 are unsupported.")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "QPlayer requires x86_64. Detected: ${CMAKE_SYSTEM_PROCESSOR}. "
                        "ARM64 and other architectures are unsupported.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old; QPlayer requires GCC >= 11.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
        message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old; QPlayer requires Clang >= 13.")
    endif()
else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. "
                        "MSVC, ICC, AppleClang, and other compilers are not supported. "
                        "Use GCC >= 11 or Clang >= 13.")
endif()

# ── 2. Language standard and global flags ────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Default to Release build" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── 3. User-facing build options ──────────────────────────────────
option(QPLAYER_BUILD_PYTHON   "Build the pybind11 Python module (Phase 7+)" OFF)
option(QPLAYER_BUILD_TESTS    "Build the in-house test framework and unit tests" ON)
option(QPLAYER_BUILD_GPU      "Build the CUDA FullState GPU backend (requires CUDA Toolkit)" OFF)
option(QPLAYER_BUILD_EXAMPLES "Build the C++ usage examples in examples/ (e.g. run_qasm CLI)" ON)
option(QPLAYER_BUILD_DOCS     "Define the 'docs' target generating the Doxygen API reference" ON)

# ── 4. External dependencies — three permitted only ──────────────────────────
# OpenBLAS supplies the LAPACK zgesvd_ used by MPS SVD truncation
# Site tensors are complex<double>, so the real dgesvd_ named in the
# original design sketch does not apply — corrected 2026-08-25.
find_package(OpenBLAS)
if(OpenBLAS_FOUND)
    message(STATUS "[qplayer] OpenBLAS ${OpenBLAS_VERSION} located: ${OpenBLAS_LIBRARY} (dlopen'd at run time, not linked --)")
else()
    message(STATUS "[qplayer] OpenBLAS not found at configure time; TensorNetwork needs libopenblas.so.0 at run time")
endif()

# OpenMP is a compiler-builtin (libgomp on GCC) — not a third-party package, so
# it sits outside the "three permitted dependencies" rule (C).
# Drives the 2-level thread + SIMD parallelism of the FullState CPU backend
#
find_package(OpenMP REQUIRED)
message(STATUS "[qplayer] OpenMP ${OpenMP_CXX_VERSION} located (flags: ${OpenMP_CXX_FLAGS})")

if(QPLAYER_BUILD_PYTHON)
    find_package(pybind11 2.10 QUIET)
    if(NOT pybind11_FOUND)
        message(STATUS "[qplayer] pybind11 not found via find_package; fetching v2.11.1")
        include(FetchContent)
        FetchContent_Declare(pybind11
            GIT_REPOSITORY https://github.com/pybind/pybind11.git
            GIT_TAG        v2.11.1)
        FetchContent_MakeAvailable(pybind11)
    endif()
endif()

if(QPLAYER_BUILD_GPU)
    find_package(CUDAToolkit 12.0 REQUIRED)

    # find_package(CUDAToolkit) and enable_language(CUDA) use different search logic and
    # can disagree: on a box with both an apt nvidia-cuda-toolkit (/usr/bin/nvcc, 12.0)
    # and a /usr/local/cuda-* install, FindCUDAToolkit prefers the newer toolkit while
    # enable_language(CUDA) may still pick the apt nvcc — even when `which nvcc` resolves
    # to the newer one. Pin enable_language to the toolkit FindCUDAToolkit resolved, unless
    # the user pinned one explicitly (-DCMAKE_CUDA_COMPILER or CUDACXX)..
    if(NOT DEFINED CMAKE_CUDA_COMPILER AND NOT DEFINED ENV{CUDACXX})
        set(CMAKE_CUDA_COMPILER "${CUDAToolkit_NVCC_EXECUTABLE}")
    endif()

    # CUDA <12.4 (cudafe++) cannot parse GCC 13+ glibc headers — the _Float32/64/128 types
    # in mathcalls-narrow.h trigger a flood of "does not name a type" errors. Fail fast
    # BEFORE enable_language(CUDA): its internal compiler-identification test would
    # otherwise abort with that cryptic flood instead of this actionable message..
    if(CUDAToolkit_VERSION VERSION_LESS 12.4
       AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
        message(FATAL_ERROR
            "Selected CUDA ${CUDAToolkit_VERSION} (${CUDAToolkit_NVCC_EXECUTABLE}) does not "
            "support GCC ${CMAKE_CXX_COMPILER_VERSION} as host compiler — CUDA >= 12.4 is "
            "required for GCC 13+. Point CMake at a newer nvcc, e.g.\n"
            "  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.6/bin/nvcc\n"
            "or export CUDACXX=/usr/local/cuda-12.6/bin/nvcc before configuring. See INSTALL.md.")
    endif()

    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

# ── 5. Core static library ───────────────────────────────────────────────────
# Phase 0-S: every public header has a matching stub.cpp. The previous
# src/qplayer_core_dummy.cpp placeholder is gone — src/qregister.cpp now pulls
# the public umbrella header so header-only compilation remains covered.
add_library(qplayer_core STATIC
    # Logging + result + memory budget guard.
    src/log.cpp
    src/result.cpp
    src/backend/memory_check.cpp

    # Circuit IR + Interactive facade + JSON/QASM serialiser.
    src/circuit/circuit.cpp
    src/circuit/interactive.cpp
    src/circuit/serializer.cpp
    src/circuit/qasm_emitter.cpp
    src/circuit/circuit_draw.cpp
    src/circuit/detail/json_value.cpp

    # Noise + device model.
    src/noise/noise_model.cpp
    src/noise/device_model.cpp
    src/noise/t1t2_calculator.cpp

    # Parser facade (OpenQASM v2).
    src/parser/parser.cpp
    src/parser/lexer.cpp
    src/parser/ast_parser.cpp
    src/parser/semantic_validator.cpp
    src/parser/circuit_builder.cpp
    src/parser/qelib1_builtin.cpp

    # Transpiler + PassManager + concrete pass placeholders.
    src/transpiler/transpiler.cpp
    src/transpiler/pass_manager.cpp
    src/transpiler/passes/analysis/duration_annotation_pass.cpp
    src/transpiler/passes/validation/coupling_validator.cpp
    src/transpiler/passes/layout/initial_layout_pass.cpp
    src/transpiler/passes/routing/swap_router_pass.cpp
    src/transpiler/passes/translation/basis_gate_translation_pass.cpp
    src/transpiler/passes/optimization/swap_cancel_pass.cpp
    src/transpiler/passes/optimization/qubit_truncation_pass.cpp
    src/transpiler/passes/optimization/cancel_inverse.cpp
    src/transpiler/passes/optimization/commutation_reorder_pass.cpp
    src/transpiler/passes/optimization/merge_single_qubit_pass.cpp
    src/transpiler/passes/optimization/merge_single_qubit_unitary_pass.cpp
    src/transpiler/passes/optimization/diagonal_phase_pass.cpp
    src/transpiler/passes/optimization/remove_identity_pass.cpp
    src/transpiler/passes/optimization/merge_same_gate_pass.cpp
    src/transpiler/passes/optimization/pattern_replace_pass.cpp
    src/transpiler/passes/optimization/remove_idle_qubit_pass.cpp
    src/transpiler/passes/optimization/merge_two_qubit_pass.cpp
    src/transpiler/passes/scheduling/alap_schedule.cpp
    src/transpiler/passes/validation/semantic_validator_pass.cpp

    # Simulator backends + factory.
    src/backend/result_access.cpp
    src/backend/factory.cpp
    src/backend/full_state/cpu_dispatcher.cpp
    src/backend/full_state/full_state_cpu.cpp
    src/backend/full_state/gate_fusion.cpp
    src/backend/full_state/pauli_expectation.cpp
    src/backend/full_state/qubit_reorder.cpp
    src/backend/full_state/full_state_gpu.cpp
    src/backend/noise_schedule.cpp
    src/backend/sparse_state/sparse_state_backend.cpp
    src/backend/sparse_state/feasibility.cpp
    src/backend/tensor_network/tn_backend.cpp
    src/backend/tensor_network/tn_cpu.cpp
    src/backend/tensor_network/blas_loader.cpp
    src/backend/tensor_network/mps.cpp
    src/backend/tensor_network/topology.cpp
    src/backend/tensor_network/tn_fusion.cpp
    src/backend/tensor_network/contraction_order.cpp
    src/backend/tensor_network/tn_amplitude.cpp
    src/backend/tensor_network/tn_rdm.cpp
    src/backend/tensor_network/tn_expectation.cpp
    src/backend/tensor_network/tn_measure.cpp
    src/backend/tensor_network/tn_noise.cpp
    src/backend/tensor_network/tn_sampling.cpp
    src/backend/tensor_network/tn_mpo.cpp
    src/backend/tensor_network/tn_svd_gpu.cpp

    # Public entry point.
    src/qregister.cpp)

target_include_directories(qplayer_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

# OpenBLAS is NOT linked: it builds a thread pool at library-initialisation
# time, which every FullState-only process would pay for without ever calling
# BLAS. The TensorNetwork backend dlopen's it instead, so the build
# needs the dynamic loader rather than the library.
target_link_libraries(qplayer_core PUBLIC
    ${CMAKE_DL_LIBS}
    OpenMP::OpenMP_CXX)

# -Wpedantic flags GCC line-directive style inside nvcc-generated stubs, so the
# warning set is scoped to CXX. CUDA TUs keep -Wall/-Wextra via -Xcompiler.
target_compile_options(qplayer_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic
                              -Wno-unused-function -Wno-unused-parameter>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra,-Wno-unused-function,-Wno-unused-parameter>)

# CUDA kernel translation units. Only compiled when QPLAYER_BUILD_GPU
# is enabled; the matching .cpp stub stays free of CUDA headers so the default
# (CPU-only) build never drags in nvcc.
if(QPLAYER_BUILD_GPU)
    target_sources(qplayer_core PRIVATE
        src/backend/full_state/full_state_gpu.cu
        src/backend/full_state/qubit_reorder.cu
        src/backend/tensor_network/tn_svd_gpu.cu)
    # CUDA::curand for the GPU Monte Carlo trajectory PRNG (curand_kernel.h device
    # API, curandStatePhilox4_32_10 —). Part of the CUDA Toolkit, not a
    # new external dependency.
    # CUDA::cusolver for the TensorNetwork two-site SVD offload
    # Part of the CUDA Toolkit, not a new external dependency.
    target_link_libraries(qplayer_core PUBLIC CUDA::cudart CUDA::curand CUDA::cusolver)
    # Multi-target sm_75+ so one binary ships native kernels for Turing through
    # Hopper without JIT recompile (: sm_75 minimum).
    set_target_properties(qplayer_core PROPERTIES
        CUDA_ARCHITECTURES "75;80;86;89;90")
    # The .cpp TUs that call the .cu launchers need QPLAYER_BUILD_GPU defined so
    # factory.cpp wires in the cudaGetDeviceCount() runtime probe.
    target_compile_definitions(qplayer_core PRIVATE QPLAYER_BUILD_GPU)
endif()

# ── 6. Cache-tuning constants ─────────────────────────────────────────
include(detect_cache)
qplayer_detect_cache(qplayer_core)

# ── 7. Python bindings (Phase 0-S skeleton) ──────────────────
# Builds a single pybind11 extension module `_qplayer` that is re-exported by
# bindings/qplayer/__init__.py. The C++ core is linked in as a static
# dependency; -fPIC is required because qplayer_core is a STATIC archive.
if(QPLAYER_BUILD_PYTHON)
    set_target_properties(qplayer_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # pybind11_add_module flips INTERPROCEDURAL_OPTIMIZATION on by default; the
    # serial LTO link emits an informational warning. Phase 0-S has no perf
    # need for LTO, so we opt out until Phase 7 measures the real impact.
    pybind11_add_module(_qplayer NO_EXTRAS
        bindings/qplayer/qplayer_bindings.cpp)
    target_link_libraries(_qplayer PRIVATE qplayer_core)
    target_compile_options(_qplayer PRIVATE -Wall -Wextra -Wpedantic)
    # Propagate project() VERSION to the binding as a preprocessor macro so
    # qplayer.__version__ tracks CMakeLists VERSION without manual edits.
    # pyproject.toml is verified against PROJECT_VERSION
    target_compile_definitions(_qplayer PRIVATE
        QPLAYER_VERSION_STRING="${PROJECT_VERSION}")
    # scikit-build-core installs the .so into bindings/qplayer/ where
    # __init__.py picks it up. The destination keeps the editable layout flat.
    install(TARGETS _qplayer DESTINATION qplayer)
endif()

# ── 8. Tests — Phase 1 test framework ─────────────────────────────────
if(QPLAYER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── 8.5. C++ usage examples (early CLI runner ahead of Phase 12) ────
# Added after the tests block so the example smoke test can register via the
# enable_testing() called above when QPLAYER_BUILD_TESTS is ON.
if(QPLAYER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── 8.6. Doxygen API reference ─────────────────────────
# Opt-in and non-fatal: Doxygen is a documentation generator, not a link-time
# dependency, so a host without it must still configure and build. The target
# is only defined when the tool is found; `--target docs` on a bare host then
# fails with CMake's own "no rule to make target" rather than a build error
# planted in the library graph.
if(QPLAYER_BUILD_DOCS)
    find_package(Doxygen QUIET COMPONENTS dot)
    if(DOXYGEN_FOUND)
        set(QPLAYER_DOXYGEN_VERSION    "${PROJECT_VERSION}")
        set(QPLAYER_DOXYGEN_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/docs")
        # One unified reference: include/ and src/ together, so a
        # reader can follow a call from the public entry point into the kernel
        # that implements it. The English user-facing markdown files
        # join them so their cross-links resolve inside the HTML output instead
        # of dangling. Korean design docs stay out by policy.
        set(QPLAYER_DOXYGEN_INPUT      "${CMAKE_SOURCE_DIR}/include \
${CMAKE_SOURCE_DIR}/src \
${CMAKE_SOURCE_DIR}/README.md \
${CMAKE_SOURCE_DIR}/INSTALL.md \
${CMAKE_SOURCE_DIR}/examples/README.md \
${CMAKE_SOURCE_DIR}/tests/README.md")
        set(QPLAYER_DOXYGEN_STRIP_PATH "${CMAKE_SOURCE_DIR}")
        set(QPLAYER_DOXYGEN_EXAMPLE_PATH "${CMAKE_SOURCE_DIR}/examples")
        set(QPLAYER_DOXYGEN_MAINPAGE   "${CMAKE_SOURCE_DIR}/README.md")
        set(QPLAYER_DOXYGEN_WARN_LOG   "${CMAKE_BINARY_DIR}/doxygen-warnings.log")
        if(TARGET Doxygen::dot)
            set(QPLAYER_DOXYGEN_HAVE_DOT YES)
        else()
            set(QPLAYER_DOXYGEN_HAVE_DOT NO)
        endif()

        # Tags shared by all three configs are extracted into Doxyfile.common,
        # which each concrete config pulls in with @INCLUDE ( set
        # the three-config count as the threshold for this indirection).
        set(QPLAYER_DOXYGEN_COMMON "${CMAKE_BINARY_DIR}/Doxyfile.common")
        configure_file("${CMAKE_SOURCE_DIR}/cmake/Doxyfile.common.in"
                       "${QPLAYER_DOXYGEN_COMMON}" @ONLY)

        configure_file("${CMAKE_SOURCE_DIR}/cmake/Doxyfile.in"
                       "${CMAKE_BINARY_DIR}/Doxyfile" @ONLY)

        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Generating Doxygen API reference into docs/html"
            VERBATIM)

        # Coverage gate. A second Doxygen config is required
        # because Doxygen silently disables WARN_IF_UNDOCUMENTED while
        # EXTRACT_ALL is YES, and the docs target keeps EXTRACT_ALL on so
        # undocumented declarations still render as a structural map.
        set(QPLAYER_DOXYGEN_COV_DIR "${CMAKE_BINARY_DIR}/doxygen-coverage")
        set(QPLAYER_DOXYGEN_COV_LOG "${CMAKE_BINARY_DIR}/doxygen-coverage.log")
        configure_file("${CMAKE_SOURCE_DIR}/cmake/Doxyfile.coverage.in"
                       "${CMAKE_BINARY_DIR}/Doxyfile.coverage" @ONLY)

        # The verdict script is Python; without an interpreter the measurement
        # config is still configured but the target is not defined, matching the
        # non-fatal documentation policy.
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_custom_target(docs-coverage
                COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_BINARY_DIR}/Doxyfile.coverage"
                COMMAND ${CMAKE_COMMAND} -E echo
                        "[qplayer] undocumented-entity log: ${QPLAYER_DOXYGEN_COV_LOG}"
                COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/"
                        "${QPLAYER_DOXYGEN_COV_DIR}/xml"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                COMMENT "Measuring Doxygen documentation coverage"
                VERBATIM)
            message(STATUS "[qplayer] Doxygen ${DOXYGEN_VERSION} found — 'docs' + 'docs-coverage' targets available (dot: ${QPLAYER_DOXYGEN_HAVE_DOT})")
        else()
            message(STATUS "[qplayer] Doxygen ${DOXYGEN_VERSION} found — 'docs' target available; 'docs-coverage' needs a Python 3 interpreter (dot: ${QPLAYER_DOXYGEN_HAVE_DOT})")
        endif()
    else()
        message(STATUS "[qplayer] Doxygen not found — 'docs' target unavailable")
    endif()
endif()

# ── 9. Build summary ─────────────────────────────────────────────────────────
message(STATUS "[qplayer] Build configuration:")
message(STATUS "  CMAKE_BUILD_TYPE       = ${CMAKE_BUILD_TYPE}")
message(STATUS "  CMAKE_CXX_COMPILER     = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  QPLAYER_BUILD_PYTHON   = ${QPLAYER_BUILD_PYTHON}")
message(STATUS "  QPLAYER_BUILD_TESTS    = ${QPLAYER_BUILD_TESTS}")
message(STATUS "  QPLAYER_BUILD_GPU      = ${QPLAYER_BUILD_GPU}")
message(STATUS "  QPLAYER_BUILD_EXAMPLES = ${QPLAYER_BUILD_EXAMPLES}")
message(STATUS "  QPLAYER_BUILD_DOCS     = ${QPLAYER_BUILD_DOCS}")
