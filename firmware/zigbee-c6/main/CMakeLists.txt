# Phase 6 Stage 2b: Zigbee coordinator + translator wired onto the UART bus.
# esp-zigbee-lib is pulled as a managed dependency (see idf_component.yml); the
# shared protocol/transport components come from ../components (EXTRA_COMPONENT_DIRS).
idf_component_register(SRCS "main.cpp" "status_led.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES core esp_uart_link esp_ota_sink hubproto nvs_boot_id nvs_flash esp_timer)
