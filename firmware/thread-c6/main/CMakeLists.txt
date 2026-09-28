# Stage B: OpenThread up. When we layer esp-matter on top, add that here too.
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES core esp_uart_link hubproto nvs_boot_id nvs_flash
                                openthread esp_event esp_netif vfs
                                esp_timer)
