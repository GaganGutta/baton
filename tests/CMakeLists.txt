# baton_add_test(<name> SOURCES a.cpp b.cpp LIBS baton_common ...)
# One test binary per module keeps link times short and failures localized.
function(baton_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests")  # "support/..."
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} baton_options GTest::gtest GTest::gmock GTest::gtest_main)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

add_subdirectory(unit)
