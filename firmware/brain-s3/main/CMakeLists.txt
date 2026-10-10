idf_component_register(SRCS "main.cpp" "ota_job.cpp" "app_server.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES core esp_uart_link hubproto esp_timer esp_http_client esp_http_server esp-tls)
