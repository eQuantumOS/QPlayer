cmake_minimum_required(VERSION 3.4...3.18)
project(qplayer)

find_package(Python3 COMPONENTS Interpreter Development)
find_package(OpenMP REQUIRED)

# Add the path to pybind11
add_subdirectory(pybind11)

# Add a new executable target for initializing Token::KindNames
# add_executable(TokenInitializer src/TokenInitializer.cpp QPlayer/qasm/src/Token.cpp)
# target_link_libraries(TokenInitializer PRIVATE QASMtoken)
set(QPLAYER_SOURCES
    qplayer/core.cpp
    # qplayer/qelib1.inc        ## UPDATE[01.16]: qelib1.inc is a required file, but no longer included directly in the file. 
    ../cli/src/Parser.cpp
    ../cli/src/Scanner.cpp
    ../cli/src/Token.cpp
    ../../core/src/gate.cpp
    ../../core/src/register.cpp
    ../../core/src/timer.cpp
    ../../core/src/misc.cpp
    ../../core/src/dump.cpp
    ../../core/src/experimental.cpp
    ../../core/src/state.cpp
)
pybind11_add_module(qplayer_wra ${QPLAYER_SOURCES})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../cli
                    ${CMAKE_CURRENT_SOURCE_DIR}/../cli/include
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../release/include
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)

# Add the following line to disable the warning about converting string literals to char*
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-write-strings -lstdc++fs -std=c++17")

# Link the Python library
target_link_libraries(qplayer_wra PRIVATE Python3::Python)
target_link_libraries(qplayer_wra PRIVATE OpenMP::OpenMP_CXX)
