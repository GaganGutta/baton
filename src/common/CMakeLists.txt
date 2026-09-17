add_library(baton_common STATIC
  check.cpp
  clock.cpp
  crc32c.cpp
  fd.cpp
  logging.cpp
  posix_fs.cpp
  result.cpp)
target_link_libraries(baton_common PUBLIC baton_options)
