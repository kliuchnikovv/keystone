# ESP-IDF project: brain (ESP32-S3-Zero).
#
# Phase 2: UART link-up. Uses core::UartTransport over an EspUartLink to
# exchange HELLO/HEARTBEAT with the co-processor. Later phases add the registry,
# state machine, rules, and WiFi/Matter.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keystone_brain_s3)
