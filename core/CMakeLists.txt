# core — the brain: Bus, EntityRegistry, StateMachine, RuleEngine, Dispatcher.
#
# Portable C++17 with ZERO radio / ESP-IDF-radio dependencies (SPEC Invariant 3
# & 5) so the brain can migrate S3 -> P4 -> Linux with the core unchanged. It
# depends only on libhubproto for the wire types.
#
# Phase 0: placeholder only. Real components land in Phases 3-5.
add_library(core STATIC
  src/placeholder.cpp
  src/uart_transport.cpp
  src/sha256.cpp
  src/ota_receiver.cpp
  src/ota_sender.cpp)

target_include_directories(core PUBLIC include)
target_link_libraries(core
  PUBLIC hubproto
  PRIVATE keystone_warnings)
