# Thread network co-processor main component.
# No esp-matter here any more: the CHIP stack and the Matter Bridge are gone
# (docs/matter-backlog.md §3). What is left is OpenThread as a Border Router
# (SRP server, DNS-SD server, border routing — one flag turns all three on),
# a Wi-Fi STA that is the backbone, mDNS that reflects SRP onto Wi-Fi, the UART
# link to the brain and the OTA sink.
idf_component_register(SRCS "main.cpp" "status_led.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES core esp_uart_link esp_ota_sink hubproto nvs_boot_id
                                nvs_flash esp_timer esp_wifi esp_netif esp_event
                                openthread vfs console esp_coex)
