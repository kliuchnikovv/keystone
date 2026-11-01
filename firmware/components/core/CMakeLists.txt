# ESP-IDF component wrapper for the brain core.
#
# References the shared sources in ../../../core. Radio-free by design
# (SPEC Invariants 3 & 5); depends only on hubproto for the wire types.
set(CORE "${CMAKE_CURRENT_LIST_DIR}/../../../core")

idf_component_register(
  SRCS
    "${CORE}/src/placeholder.cpp"
    "${CORE}/src/uart_transport.cpp"
    "${CORE}/src/sha256.cpp"
    "${CORE}/src/ota_receiver.cpp"
    "${CORE}/src/ota_sender.cpp"
    "${CORE}/src/device_ota_receiver.cpp"
    "${CORE}/src/device_ota_sender.cpp"
  INCLUDE_DIRS
    "${CORE}/include"
  REQUIRES
    hubproto)
