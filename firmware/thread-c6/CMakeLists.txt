# ESP-IDF project: the Thread network co-processor (ESP32-C6, chip_id 0x02).
#
# The toolchain is the esp-matter Docker image (ESP-IDF 5.5 — the C6's on-chip
# 802.15.4 radio is only supported by the OpenThread port from IDF 5.2 on), but
# the app itself no longer uses esp-matter: the CHIP stack and the Matter
# Bridge are gone, and what is left is the Thread network the brain borrows
# (docs/matter-backlog.md §3). See scripts/matter-build.sh.
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0")
set(PROJECT_VER_NUMBER 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components")

project(keystone_thread_c6)

# KS_TEST_CRASH=1 builds an image that aborts 8 s after boot, to exercise the OTA
# rollback path (docs/ota.md). Never flash it for real use.
if(DEFINED ENV{KS_TEST_CRASH})
    idf_build_set_property(COMPILE_DEFINITIONS "KS_TEST_CRASH" APPEND)
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
