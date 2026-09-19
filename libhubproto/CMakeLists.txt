# libhubproto — the inter-chip wire protocol (framing, header codec, payloads).
#
# Portable C++17 with zero radio / ESP-IDF dependencies so it compiles both on
# the host (this build) and inside every firmware app. On ESP-IDF the same
# sources are wrapped by a component CMakeLists; here it is a plain static lib.
#
# Phase 0: only a version stamp exists. Framing/codec land in Phase 1.
add_library(hubproto STATIC
  src/version.cpp
  src/crc16.cpp
  src/cobs.cpp
  src/message.cpp
  src/payload.cpp)

target_include_directories(hubproto PUBLIC include)
target_link_libraries(hubproto PRIVATE keystone_warnings)
