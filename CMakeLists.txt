cmake_minimum_required(VERSION 3.24)

project(baton
  VERSION 0.1.0
  DESCRIPTION "A durable job and workflow engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(BATON_BUILD_TESTS "Build unit tests (GoogleTest)" ON)
option(BATON_BUILD_BENCH "Build benchmarks (Google Benchmark) and the load generator" OFF)
option(BATON_BUILD_FUZZ "Build libFuzzer targets (requires Clang)" OFF)
option(BATON_STATIC_RUNTIME "Link libstdc++/libgcc statically (used by the Docker image)" OFF)
set(BATON_SANITIZE "" CACHE STRING "Sanitizers to enable: 'address;undefined' or 'thread'")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(BatonSanitizers)  # global: dependencies must be instrumented too
include(BatonOptions)     # baton_options: warnings-as-errors for our targets only
include(BatonDeps)

add_subdirectory(src)

if(BATON_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BATON_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()

if(BATON_BUILD_BENCH)
  add_subdirectory(bench)
endif()
