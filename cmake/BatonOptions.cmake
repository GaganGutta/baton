# baton_options carries the compile options every baton target uses. It is an
# INTERFACE library so third-party code pulled in with FetchContent is not
# compiled with -Werror.
add_library(baton_options INTERFACE)
target_compile_options(baton_options INTERFACE -Wall -Wextra -Werror)
target_include_directories(baton_options INTERFACE
  "${PROJECT_SOURCE_DIR}/src"
  "${PROJECT_BINARY_DIR}/generated")

find_package(Threads REQUIRED)
target_link_libraries(baton_options INTERFACE Threads::Threads)

if(BATON_STATIC_RUNTIME)
  target_link_options(baton_options INTERFACE -static-libstdc++ -static-libgcc)
endif()
