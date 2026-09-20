# Phase 6 Stage 1: pure Zigbee coordinator bring-up (no protocol bus yet).
# esp-zigbee-lib is pulled as a managed dependency (see idf_component.yml) and is
# added to the requirements automatically. Stage 2 re-adds core/esp_uart_link/
# hubproto/nvs_boot_id when the translator is wired onto the UART bus.
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash)
