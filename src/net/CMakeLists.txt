add_library(baton_net STATIC
  poller_epoll.cpp
  poller_kqueue.cpp
  resp.cpp
  socket.cpp)
target_link_libraries(baton_net PUBLIC baton_common)
