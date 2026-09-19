# Host build for keystone-esp.
#
# Builds and unit-tests the portable, radio-free parts of the project on a
# normal desktop toolchain (no ESP-IDF, no hardware):
#   - libhubproto  : wire protocol (framing, header codec, payloads)
#   - core         : brain logic (Bus, EntityRegistry, StateMachine, ...)
#
# Firmware apps under firmware/ are ESP-IDF projects and are intentionally NOT
# part of this build — each is built with `idf.py` from its own directory.
#
# Usage:
#   cmake -S . -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)

project(keystone-esp
  VERSION 0.1.0
  DESCRIPTION "Multi-chip ESP32 smart-home hub — host build (libhubproto + core)"
  LANGUAGES CXX)

# Only the host build lives here; ESP-IDF drives the firmware builds itself.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Shared warning flags for our own targets (kept off third-party code).
add_library(keystone_warnings INTERFACE)
if(MSVC)
  target_compile_options(keystone_warnings INTERFACE /W4)
else()
  target_compile_options(keystone_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

include(CTest)  # defines BUILD_TESTING (ON by default)

add_subdirectory(libhubproto)
add_subdirectory(core)

if(BUILD_TESTING)
  include(cmake/doctest.cmake)
  add_subdirectory(libhubproto/test)
  add_subdirectory(core/test)
endif()
