# ESP-IDF project: the Thread network co-processor (ESP32-C6, chip_id 0x02).
#
# Two shapes:
#   - Thread host only (default): the CHIP stack is absent; the chip forms the
#     Thread network and lends the dataset to the brain (docs/matter-backlog.md §3).
#   - Thread host + Matter accessory + Matter Bridge (when ESP_MATTER_PATH is set):
#     the same Thread host plus a server-side Matter stack so Apple Home / Google
#     Home can add the hub as one accessory and see every hub entity as a
#     bridged endpoint on it (docs/matter-backlog.md § 10, fork F2). The
#     controller stays on brain-s3.
# In either case the toolchain is the esp-matter Docker image (ESP-IDF 5.5) —
# the C6's on-chip 802.15.4 radio needs the OpenThread port from IDF 5.2 on.
cmake_minimum_required(VERSION 3.16)

# IDF_TARGET must be set before project.cmake so the device HAL files and
# sdkconfig overlays for esp32c6 are picked up on a from-scratch configure.
set(IDF_TARGET esp32c6)

set(PROJECT_VER "1.0")
set(PROJECT_VER_NUMBER 1)

if(DEFINED ENV{ESP_MATTER_PATH})
    # The esp-matter examples use these paths to find the device HAL and
    # esp-matter's own components. This project only targets ESP32-C6, and
    # `IDF_TARGET` is not populated until project.cmake runs below — so the
    # device path is set unconditionally.
    if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
        set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/esp32c6_devkit_c)
    endif()
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    # Ask for the Matter overlay here, in the project itself. IDF merges the
    # files listed in SDKCONFIG_DEFAULTS on top of each other while it creates
    # sdkconfig — and it reads that list from this cmake variable (or from an
    # environment variable of the same name, which is only seen during the
    # configure step and is easy to lose: the bootloader's own cmake pass does
    # not inherit it). Asking from the project is the one way that cannot be
    # dropped on the way.
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.matter")
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
    include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)
    set(EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../components"
        "${ESP_MATTER_PATH}/examples/common"
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components"
        "${ESP_MATTER_PATH}/device_hal/device"
        ${extra_components_dirs_append})
else()
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    set(EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../components")
endif()

project(keystone_thread_c6)

# KS_TEST_CRASH=1 builds an image that aborts 8 s after boot, to exercise the OTA
# rollback path (docs/ota.md). Never flash it for real use.
if(DEFINED ENV{KS_TEST_CRASH})
    idf_build_set_property(COMPILE_DEFINITIONS "KS_TEST_CRASH" APPEND)
endif()

if(DEFINED ENV{ESP_MATTER_PATH})
    # CHIP headers use `DECLARE_DYNAMIC_ENDPOINT` and friends that depend on the
    # Matter autoconf header; the chip's own sources are not portable enough for
    # the stricter format diagnostics (uint32_t printed with %u is an error on
    # this toolchain) and the Matter build is -Werror=all. brain-s3 does the same.
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0" APPEND)
else()
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os" APPEND)
endif()
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
