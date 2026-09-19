# Host unit tests for libhubproto.
add_executable(hubproto_tests
  test_main.cpp
  test_smoke.cpp)

target_link_libraries(hubproto_tests PRIVATE hubproto doctest::doctest keystone_warnings)

doctest_discover_tests(hubproto_tests)
