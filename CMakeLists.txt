cmake_minimum_required(VERSION 3.21)
project(chronos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Compiler warnings -------------------------------------------------------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter      # suppress during active development
)

# ---- Dependencies via FetchContent ------------------------------------------
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# Don't install gtest alongside our project.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.4
    GIT_SHALLOW    TRUE
)
set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS       OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

# ---- Core library -----------------------------------------------------------
# All modules link against chronos_lib; this avoids recompiling shared code
# for the main executable, tests, and benchmarks.
add_library(chronos_lib STATIC
    src/clock/clock_model.cpp
    src/clock/offset_estimator.cpp
    src/clock/drift_estimator.cpp
    src/simulation/event_source.cpp
    src/simulation/sync_exchange.cpp
    src/network/latency_model.cpp
    src/metrics/accuracy.cpp
    src/metrics/percentile.cpp
)
target_include_directories(chronos_lib PUBLIC include)

# ---- Main executable --------------------------------------------------------
add_executable(chronos src/main.cpp)
target_link_libraries(chronos PRIVATE chronos_lib)

# ---- Tests ------------------------------------------------------------------
enable_testing()

add_executable(chronos_tests
    tests/clock_model_test.cpp
    tests/event_source_test.cpp
    tests/accuracy_test.cpp
    tests/latency_model_test.cpp
    tests/sync_exchange_test.cpp
    tests/offset_estimator_test.cpp
    tests/drift_estimator_test.cpp
    tests/percentile_test.cpp
)
target_link_libraries(chronos_tests PRIVATE
    chronos_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(chronos_tests)

# ---- Benchmarks (stub — populated in Phase 7) -------------------------------
# add_executable(estimator_benchmark      benchmarks/estimator_benchmark.cpp)
# add_executable(reconstruction_benchmark benchmarks/reconstruction_benchmark.cpp)
# target_link_libraries(estimator_benchmark      PRIVATE chronos_lib benchmark::benchmark_main)
# target_link_libraries(reconstruction_benchmark PRIVATE chronos_lib benchmark::benchmark_main)
