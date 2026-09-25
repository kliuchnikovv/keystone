# ESP-IDF project: Thread border router + Matter co-processor (ESP32-C6).
#
# Phase 8 (SPEC §9): a second C6 running the on-chip 802.15.4 radio as a Thread
# stack (openthread), later hosting the Matter fabric endpoint. Talks to the
# brain over its own UART bus (same wire protocol as zigbee-c6, chip_id 0x02).
# Kept in its own app because one 802.15.4 radio = one stack at a time
# (SPEC §10): Zigbee lives on chip 0x01, Thread on chip 0x02.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keystone_thread_c6)
