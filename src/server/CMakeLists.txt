add_library(baton_server STATIC
  commands.cpp
  config.cpp
  server.cpp)
target_link_libraries(baton_server PUBLIC baton_common baton_log baton_net baton_sched baton_state)

add_executable(baton main.cpp)
target_link_libraries(baton PRIVATE baton_server)
