# ESP-IDF component wrapper for libhubproto.
#
# References the shared sources in ../../../libhubproto so the same protocol code
# builds on host (top-level CMake) and on target (here), with no duplication and
# without touching the host build.
set(HP "${CMAKE_CURRENT_LIST_DIR}/../../../libhubproto")

idf_component_register(
  SRCS
    "${HP}/src/version.cpp"
    "${HP}/src/crc16.cpp"
    "${HP}/src/cobs.cpp"
    "${HP}/src/message.cpp"
    "${HP}/src/payload.cpp"
    "${HP}/src/frame.cpp"
  INCLUDE_DIRS
    "${HP}/include")
