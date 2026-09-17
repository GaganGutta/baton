# libFuzzer targets. Built only with the `fuzz` preset (Clang, ASan + UBSan).
# scripts/fuzz.sh runs each one for a bounded time; CI does the same.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "BATON_BUILD_FUZZ requires Clang (libFuzzer)")
endif()

# baton_add_fuzzer(<name> SOURCES a.cpp LIBS baton_log ...)
function(baton_add_fuzzer name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} baton_options)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

baton_add_fuzzer(fuzz_log_segment SOURCES log_segment_fuzz.cpp LIBS baton_log baton_testing)
baton_add_fuzzer(fuzz_log_damage SOURCES log_damage_fuzz.cpp LIBS baton_log baton_testing)

# Writes seed inputs so the fuzzers start from valid structures.
add_executable(fuzz_make_seeds make_seeds.cpp)
target_link_libraries(fuzz_make_seeds PRIVATE baton_log baton_options)
