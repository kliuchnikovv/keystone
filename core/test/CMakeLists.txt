# Host unit tests for core.
add_executable(core_tests
  test_main.cpp
  test_smoke.cpp
  test_uart_transport.cpp
  test_entity_registry.cpp
  test_radio_coordinator.cpp
  test_state_machine.cpp
  test_command_dispatcher.cpp
  test_rule_engine.cpp
  test_entity_names.cpp)

target_link_libraries(core_tests PRIVATE core doctest::doctest keystone_warnings)

doctest_discover_tests(core_tests)
