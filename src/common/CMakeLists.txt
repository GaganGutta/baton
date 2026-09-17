add_library(baton_common STATIC
  check.cpp
  clock.cpp
  fd.cpp
  logging.cpp
  result.cpp)
target_link_libraries(baton_common PUBLIC baton_options)
