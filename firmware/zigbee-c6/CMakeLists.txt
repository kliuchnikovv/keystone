# ESP-IDF project: Zigbee coordinator co-processor (ESP32-C6).
#
# Phase 0: RGB-LED blink only, to confirm the toolchain flashes this board.
# Later phases add libhubproto + UART transport, then the esp-zigbee-sdk
# coordinator and the Zigbee -> DECLARE/STATE translator.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keystone_zigbee_c6)
