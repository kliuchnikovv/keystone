idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES core esp_uart_link hubproto nvs_boot_id)
