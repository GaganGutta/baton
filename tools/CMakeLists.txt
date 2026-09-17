# Offline tools that read a data directory.

# baton-logcheck: an independent check of the lease rules against the server's
# own log. The chaos harness runs it after every round (docs/design.md 10.2).
add_executable(baton-logcheck logcheck.cpp)
target_link_libraries(baton-logcheck PRIVATE
  baton_check baton_snapshot baton_state baton_log baton_options)
