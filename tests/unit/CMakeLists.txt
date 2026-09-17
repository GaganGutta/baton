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
