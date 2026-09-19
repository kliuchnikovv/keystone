# ESP-IDF project: brain (ESP32-S3-Zero).
#
# Phase 0: RGB-LED blink only, to confirm the toolchain flashes this board.
# Later phases add the core library + UART transport + WiFi/Matter.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keystone_brain_s3)
