# tests/CMakeLists.txt — QPlayer in-house test framework and unit tests.
# Builds the qplayer_tests executable and registers it with CTest.
# No external test libraries are used.

add_executable(qplayer_tests
    # Framework sources.
    framework/test_runner.cpp
    framework/test_main.cpp

    # Unit tests.
    scenario/unit/test_framework.cpp
    scenario/unit/test_log.cpp
    scenario/unit/test_circuit_ir.cpp
    scenario/unit/test_circuit_metadata.cpp
    scenario/unit/test_circuit_serializer.cpp
    scenario/unit/test_circuit_draw.cpp
    scenario/unit/test_lexer.cpp
    scenario/unit/test_parser_ast.cpp
    scenario/unit/test_semantic_validator.cpp
    scenario/unit/test_circuit_builder.cpp
    scenario/unit/test_pass_manager.cpp
    scenario/unit/test_passes/test_semantic_validator.cpp
    scenario/unit/test_passes/test_cancel_inverse_pass.cpp
    scenario/unit/test_passes/test_commutation_reorder_pass.cpp
    scenario/unit/test_passes/test_merge_single_qubit_pass.cpp
    scenario/unit/test_passes/test_merge_single_qubit_unitary_pass.cpp
    scenario/unit/test_passes/test_diagonal_phase_pass.cpp
    scenario/unit/test_passes/test_remove_identity_pass.cpp
    scenario/unit/test_passes/test_merge_same_gate_pass.cpp
    scenario/unit/test_passes/test_pattern_replace_pass.cpp
    scenario/unit/test_passes/test_remove_idle_qubit_pass.cpp
    scenario/unit/test_passes/test_merge_two_qubit_pass.cpp
    scenario/unit/test_passes/test_initial_layout.cpp
    scenario/unit/test_passes/test_swap_router.cpp
    scenario/unit/test_passes/test_swap_cancel.cpp
    scenario/unit/test_passes/test_basis_gate_translation.cpp
    scenario/unit/test_passes/test_qubit_truncation.cpp
    scenario/unit/test_transpiler.cpp
    scenario/unit/test_compiler.cpp
    scenario/unit/test_qregister_transpile.cpp
    scenario/unit/test_qregister_load_device.cpp
    scenario/unit/test_pass_chain.cpp
    scenario/unit/test_transpile_acceptance_qasm.cpp
    scenario/unit/test_memory_check.cpp
    scenario/unit/test_full_state.cpp
    scenario/unit/test_full_state_gpu.cpp
    scenario/unit/test_lazy_sampling.cpp
    scenario/unit/test_qelib1_gate_convention.cpp
    scenario/unit/test_cpu_dispatcher.cpp
    scenario/unit/test_seed.cpp
    scenario/unit/test_gate_fusion.cpp
    scenario/unit/test_ecr_gate.cpp
    scenario/unit/test_google_gates.cpp
    scenario/unit/test_qubit_reorder.cpp
    scenario/unit/test_transpile_device_json.cpp
    scenario/unit/test_circuit_add_expectation.cpp
    scenario/unit/test_observable.cpp
    scenario/unit/test_primitives.cpp
    scenario/unit/test_parser_broadcast.cpp
    scenario/unit/test_cache_blocking.cpp
    scenario/unit/test_statevector_dump.cpp
    scenario/unit/test_qtimer.cpp
    scenario/unit/test_sparse_state.cpp
    scenario/unit/test_sparse_partition_store.cpp
    scenario/unit/test_interactive.cpp
    scenario/unit/test_noise_model.cpp
    scenario/unit/test_device_model.cpp
    scenario/unit/test_willow_device.cpp
    scenario/unit/test_t1t2_calculator.cpp
    scenario/unit/test_passes/test_duration_annotation.cpp
    scenario/unit/test_passes/test_idle_noise_inserter.cpp
    scenario/unit/test_passes/test_alap_schedule.cpp
    scenario/unit/test_noise.cpp
    scenario/unit/test_seed_reproducibility.cpp
    scenario/integration/test_execute_routing.cpp
    scenario/integration/test_compile_swap_roundtrip.cpp
    scenario/integration/test_noise_cpu_gpu_parity.cpp
    scenario/integration/test_device_noise.cpp
    scenario/integration/test_bell_expectation.cpp
    scenario/integration/test_primitives_vqe.cpp
    scenario/integration/test_bell_state.cpp
    scenario/integration/test_ghz.cpp
    scenario/integration/test_golden.cpp
    scenario/integration/test_golden_gpu.cpp)

# Allow test sources to include framework headers without path prefix.
target_include_directories(qplayer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_SOURCE_DIR}/include)

# Link against the core library so tests can exercise public API stubs.
target_link_libraries(qplayer_tests PRIVATE qplayer_core)

# Inherit the same strict warning flags as qplayer_core.
# Expose the tests directory path so test_circuit_builder.cpp can locate
# acceptance QASM files via parse_file (same data directory used by the
# acceptance runner -- avoids duplicating test data files).
target_compile_definitions(qplayer_tests PRIVATE
    QPLAYER_ACCEPTANCE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    QPLAYER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/golden")

target_compile_options(qplayer_tests PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-function -Wno-unused-parameter)

# : when CUDA is built, expose QPLAYER_BUILD_GPU to the test TUs so
# test_full_state_gpu.cpp compiles its GPU-kernel cases (factory-probe cases
# always compile). The kernels themselves live in the qplayer_core archive.
if(QPLAYER_BUILD_GPU)
    target_compile_definitions(qplayer_tests PRIVATE QPLAYER_BUILD_GPU)
endif()

# Register with CTest.  A single executable runs all cases; --output-on-failure
# is set globally via ctest options so individual assertion text is visible.
# The runner exits 0 iff only passed + expected_fail results exist, so ctest
# uses the process exit code as the pass/fail signal (no regex needed).
add_test(NAME qplayer_tests COMMAND qplayer_tests)

# Per-task-group CTest entries so `ctest -R group_noise` runs a single group and
# reports its own pass/fail and timing. The full `qplayer_tests` entry above
# still runs everything (no --group). Group names must match the SSOT in
# tests/framework/test_groups.hpp (a self-test enforces suite->group coverage).
set(QPLAYER_TEST_GROUPS
    parser backend golden optimizer noise parallel primitives framework)
foreach(_grp IN LISTS QPLAYER_TEST_GROUPS)
    add_test(NAME group_${_grp} COMMAND qplayer_tests --group=${_grp})
endforeach()

# ── QASM acceptance runner ────────────────────────────────
# Loads the 7 fixed QASM acceptance cases (tests/data/qasm/*.qasm) plus expected
# values (tests/data/qasm/expected/*.json) and reports the 5-state Status table.
# Phase 0-T initial expectation: 7/7 NOT_IMPLEMENTED -> exit 0.
add_executable(qplayer_acceptance
    scenario/acceptance/run_acceptance.cpp
    scenario/acceptance/main.cpp)

target_include_directories(qplayer_acceptance PRIVATE
    ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(qplayer_acceptance PRIVATE qplayer_core)

# Embed the absolute path to the acceptance data directory so the runner can
# locate tests/data/qasm and tests/data/qasm/expected without depending on the CWD at
# CTest invocation time. An env-var override remains available for CI.
target_compile_definitions(qplayer_acceptance PRIVATE
    QPLAYER_ACCEPTANCE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

target_compile_options(qplayer_acceptance PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-function -Wno-unused-parameter)

add_test(NAME qplayer_acceptance COMMAND qplayer_acceptance)
