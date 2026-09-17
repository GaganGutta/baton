# Test support code (never linked into the baton binary).
add_library(baton_testing STATIC
  sim_fs.cpp)
target_link_libraries(baton_testing PUBLIC baton_common)
