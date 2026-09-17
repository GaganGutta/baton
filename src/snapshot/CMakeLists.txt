add_library(baton_snapshot STATIC
  recovery.cpp
  snapshot.cpp
  snapshotter.cpp)
target_link_libraries(baton_snapshot PUBLIC baton_common baton_log baton_state)
