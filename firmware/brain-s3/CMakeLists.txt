# ESP-IDF project: brain (ESP32-S3-Zero).
#
# Phase 2: UART link-up. Uses core::UartTransport over an EspUartLink to
# exchange HELLO/HEARTBEAT with the co-processor. Later phases add the registry,
# state machine, rules, and WiFi/Matter.
cmake_minimum_required(VERSION 3.16)
# PlatformIO infers the target from platformio.ini's `board = esp32-s3-devkitc-1`;
# plain idf.py (used by the Docker/esp-matter build in scripts/matter-build.sh)
# needs it set explicitly. Both paths agree on esp32s3.
set(IDF_TARGET esp32s3)

# experiment/commissioner-on-s3: when ESP_MATTER_PATH is set (the esp-matter
# Docker build, scripts/matter-build.sh), pull in the Matter SDK the same way
# thread-c6 does — EXTRA_COMPONENT_DIRS points at esp-matter components and
# the connectedhomeip esp32 config components. With ESP_MATTER_PATH unset
# (PlatformIO, plain idf.py outside Docker) the brain builds exactly as before:
# no Matter, no BLE, no controller.
set(KS_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
if(DEFINED ENV{ESP_MATTER_PATH})
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    list(APPEND KS_EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components")
endif()
set(EXTRA_COMPONENT_DIRS ${KS_EXTRA_COMPONENT_DIRS})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keystone_brain_s3)
add_compile_options(-Wno-error)

if(DEFINED ENV{ESP_MATTER_PATH})
    # Match esp-matter examples/controller's build flags. CHIP headers need
    # CHIP_HAVE_CONFIG_H; the SDK has many virtual overrides that trip the warning
    # without -Wno-overloaded-virtual; the controller never needs Thread
    # network commissioning (it pairs over BLE+Wi-Fi), so turn that driver off.
    idf_build_set_property(CXX_COMPILE_OPTIONS
        "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual;-Wno-error=unused-result" APPEND)
    idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-D_NO_GENERIC_THREAD_NETWORK_COMMISSIONING_DRIVER_" APPEND)
    # CHIPDeviceController.cpp has `%u` on fields whose type is `uint32_t`, which
    # on xtensa-esp32s3 is `unsigned long` — the warning becomes an error under
    # -Werror. The vendor's own examples/controller root CMake drops -Wformat
    # entirely for that reason (it never trips on c6 because `uint32_t` is
    # `unsigned int` there, so thread-c6 did not need it).
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0" APPEND)
endif()
