# Matter co-processor main component.
# Pulls in the Matter data model (esp_matter) plus our shared UART bridge.
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils"
                       REQUIRES esp_matter core esp_uart_link hubproto nvs_boot_id
                                nvs_flash esp_event esp_netif esp_timer openthread)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
