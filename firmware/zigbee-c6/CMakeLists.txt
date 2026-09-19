# ESP-IDF project: Zigbee coordinator co-processor (ESP32-C6).
#
# Phase 2: UART link-up. Uses core::UartTransport over an EspUartLink to send
# HELLO/HEARTBEAT to the brain. Phase 6 adds the esp-zigbee-sdk coordinator and
# the Zigbee -> DECLARE/STATE translator.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keystone_zigbee_c6)
