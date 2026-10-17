# Matter Bridge co-processor main component.
# Pulls in esp_matter's aggregator/bridged-endpoint machinery (app_bridge,
# esp_matter_bridge live under $ESP_MATTER_PATH/examples/common — added via
# EXTRA_COMPONENT_DIRS in the root CMakeLists).
idf_component_register(SRCS "main.cpp" "keystone_bridged_device.cpp" "status_led.cpp"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils"
                                         "${ESP_MATTER_PATH}/components/esp_matter"
                                         "${ESP_MATTER_PATH}/components/esp_matter/data_model_provider/clusters/electrical_energy_measurement"
                       REQUIRES esp_matter esp_matter_bridge app_bridge
                                core esp_uart_link esp_ota_sink hubproto nvs_boot_id
                                nvs_flash esp_event esp_netif esp_timer openthread console)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
