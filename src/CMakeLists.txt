option(BATON_ENABLE_CLANG_TIDY "Run clang-tidy on baton sources during the build" OFF)
if(BATON_ENABLE_CLANG_TIDY)
  find_program(BATON_CLANG_TIDY NAMES clang-tidy clang-tidy-21 clang-tidy-20 clang-tidy-19 REQUIRED)
  # Scoped to src/: third-party code and tests are not linted.
  set(CMAKE_CXX_CLANG_TIDY "${BATON_CLANG_TIDY}")
endif()

configure_file(common/version.h.in "${PROJECT_BINARY_DIR}/generated/common/version.h" @ONLY)

# One static library per module. Link dependencies mirror the allowed layering:
# common <- log, sched <- state <- snapshot <- server; net depends only on common.
add_subdirectory(common)
add_subdirectory(log)
add_subdirectory(sched)
add_subdirectory(state)
add_subdirectory(server)

if(BATON_BUILD_TESTS OR BATON_BUILD_FUZZ)
  add_subdirectory(testing)
endif()
