# examples/CMakeLists.txt — C++ usage examples (built when QPLAYER_BUILD_EXAMPLES=ON).
# Each example links the core static library and demonstrates the public API.
# The full examples set (Python + C++) lands at Phase 7 / Phase 12;
# run_qasm is an early CLI runner added ahead of that schedule on user request.

add_executable(run_qasm run_qasm.cpp)
target_link_libraries(run_qasm PRIVATE qplayer_core)
target_compile_options(run_qasm PRIVATE -Wall -Wextra -Wpedantic)

# noise_demo: C++ counterpart of examples/noise_demo.py — same nine cases, same
# shots/seed, same output layout, so the two can be diffed side by side.
add_executable(noise_demo noise_demo.cpp)
target_link_libraries(noise_demo PRIVATE qplayer_core)
target_compile_options(noise_demo PRIVATE -Wall -Wextra -Wpedantic)

# Smoke test: run the bundled Bell circuit and assert a clean exit. Guarded by
# QPLAYER_BUILD_TESTS because that block is where enable_testing() is called.
if(QPLAYER_BUILD_TESTS)
    add_test(NAME example_run_qasm_bell
             COMMAND run_qasm ${CMAKE_SOURCE_DIR}/tests/data/qasm/bell.qasm
                     --shots 100 --seed 1)
endif()
