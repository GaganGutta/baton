# Benchmarks. Built with the `release` preset; results are only meaningful from
# an optimized build on an otherwise idle machine. docs/benchmarks.md (M8)
# records the numbers together with the commands that produced them.
add_executable(baton_microbench
  micro/crc32c_bench.cpp
  micro/ready_heap_bench.cpp
  micro/records_bench.cpp
  micro/resp_bench.cpp
  micro/timing_wheel_bench.cpp)
target_link_libraries(baton_microbench PRIVATE
  baton_log baton_net baton_state baton_options benchmark::benchmark benchmark::benchmark_main)

# Snapshot cost and recovery time on a real file system (docs/design.md 8.6).
# Run through bench/run_storage_bench.sh.
add_executable(storage_bench storage/storage_bench.cpp)
target_link_libraries(storage_bench PRIVATE baton_snapshot baton_state baton_log baton_options)

# The load generator: baton, beanstalkd and faktory protocols (bench/README.md).
add_executable(baton-loadgen loadgen/loadgen.cpp)
target_include_directories(baton-loadgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(baton-loadgen PRIVATE baton_common baton_options)
