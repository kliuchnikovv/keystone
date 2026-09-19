# Host unit tests for libhubproto.
add_executable(hubproto_tests
  test_main.cpp
  test_smoke.cpp
  test_crc16.cpp
  test_cobs.cpp
  test_header.cpp
  test_payload_declare.cpp
  test_payload_state.cpp
  test_frame.cpp)

target_link_libraries(hubproto_tests PRIVATE hubproto doctest::doctest keystone_warnings)

doctest_discover_tests(hubproto_tests)
