add_library(baton_state STATIC
  engine.cpp
  ready_heap.cpp
  records.cpp
  state.cpp)
target_link_libraries(baton_state PUBLIC baton_common baton_sched)
