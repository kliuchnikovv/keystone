# Stage A: bus/transport only. When we add OpenThread, we'll append
# espressif/esp-openthread here (managed component), and later esp-matter.
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES core esp_uart_link hubproto nvs_boot_id nvs_flash)
