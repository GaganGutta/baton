add_library(baton_sched STATIC
  timing_wheel.cpp)
target_link_libraries(baton_sched PUBLIC baton_common)
