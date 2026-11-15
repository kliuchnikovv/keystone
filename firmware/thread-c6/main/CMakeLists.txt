# Thread network co-processor main component.
# OpenThread as a Border Router (SRP server + DNS-SD + border routing), a Wi-Fi
# STA as the backbone, mDNS reflecting SRP onto Wi-Fi, the UART link to the
# brain and the OTA sink — always.
#
# When ESP_MATTER_PATH is set, esp-matter is linked in as well so the chip can
# serve as a Matter accessory + bridge (docs/matter-backlog.md § 10, F2). The
# matter_entry / bridge code lives in its own translation units so a build
# without esp-matter is still a working Thread host.
set(KS_SRCS "main.cpp" "status_led.cpp" "matter_accessory.cpp")
set(KS_REQUIRES core esp_uart_link esp_ota_sink hubproto nvs_boot_id
                nvs_flash esp_timer esp_wifi esp_netif esp_event
                openthread vfs console esp_coex)
set(KS_PRIV_INCLUDES "")
if(DEFINED ENV{ESP_MATTER_PATH})
    list(APPEND KS_REQUIRES esp_matter esp_matter_bridge app_bridge)
    list(APPEND KS_SRCS "matter_bridge.cpp" "keystone_bridged_device.cpp")
    # The bridge needs esp-matter's own headers and the integration header of
    # the Electrical Energy Measurement cluster (NotifyCumulativeEnergyMeasured).
    list(APPEND KS_PRIV_INCLUDES
         "${ESP_MATTER_PATH}/examples/common/utils"
         "${ESP_MATTER_PATH}/components/esp_matter"
         "${ESP_MATTER_PATH}/components/esp_matter/data_model_provider/clusters/electrical_energy_measurement")
endif()

idf_component_register(SRCS ${KS_SRCS}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS ${KS_PRIV_INCLUDES}
                       REQUIRES ${KS_REQUIRES})

if(DEFINED ENV{ESP_MATTER_PATH})
    target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
endif()
