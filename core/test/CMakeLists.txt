# Host unit tests for core.
add_executable(core_tests
  test_main.cpp
  test_smoke.cpp
  test_uart_transport.cpp
  test_entity_registry.cpp
  test_radio_coordinator.cpp
  test_state_machine.cpp
  test_command_dispatcher.cpp
  test_chip_monitor.cpp
  test_reconciler.cpp
  test_ota_verifier.cpp
  test_rule_engine.cpp
  test_rule_conditions.cpp
  test_entity_names.cpp
  test_timer_table.cpp
  test_schedule_table.cpp
  test_scene_table.cpp
  test_sun.cpp
  test_zcl_profile.cpp
  test_matter_plan.cpp
  test_wifi_policy.cpp
  test_setup_mode.cpp
  test_json.cpp
  test_app_protocol.cpp
  test_app_requests.cpp
  test_sha256.cpp
  test_ota.cpp)

target_link_libraries(core_tests PRIVATE core doctest::doctest keystone_warnings)

doctest_discover_tests(core_tests)
