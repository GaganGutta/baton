add_library(baton_log STATIC
  format.cpp
  log_writer.cpp
  recovery.cpp)
target_link_libraries(baton_log PUBLIC baton_common)
