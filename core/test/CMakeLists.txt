# Host unit tests for core.
add_executable(core_tests
  test_main.cpp
  test_smoke.cpp)

target_link_libraries(core_tests PRIVATE core doctest::doctest keystone_warnings)

doctest_discover_tests(core_tests)
