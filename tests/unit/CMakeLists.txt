baton_add_test(common_test
  SOURCES
    common/check_test.cpp
    common/clock_test.cpp
    common/codec_test.cpp
    common/crc32c_test.cpp
    common/fd_test.cpp
    common/histogram_test.cpp
    common/logging_test.cpp
    common/posix_fs_test.cpp
    common/result_test.cpp
  LIBS baton_common)

baton_add_test(testing_test
  SOURCES
    testing/sim_fs_test.cpp
  LIBS baton_testing)

baton_add_test(log_test
  SOURCES
    log/crash_test.cpp
    log/format_test.cpp
    log/log_writer_test.cpp
    log/recovery_test.cpp
  LIBS baton_log baton_testing)

baton_add_test(sched_test
  SOURCES
    sched/timing_wheel_test.cpp
  LIBS baton_sched)

baton_add_test(state_test
  SOURCES
    state/engine_dlq_time_test.cpp
    state/engine_test.cpp
    state/model_test.cpp
    state/ready_heap_test.cpp
    state/records_test.cpp
    state/state_test.cpp
  LIBS baton_state)

baton_add_test(net_test
  SOURCES
    net/poller_test.cpp
    net/resp_test.cpp
  LIBS baton_net)

baton_add_test(snapshot_test
  SOURCES
    snapshot/recovery_test.cpp
    snapshot/snapshot_test.cpp
  LIBS baton_snapshot baton_testing)

baton_add_test(server_test
  SOURCES
    server/config_test.cpp
    server/server_test.cpp
  LIBS baton_server baton_testing)

baton_add_test(check_test
  SOURCES
    check/lease_model_test.cpp
  LIBS baton_check baton_testing)
