# ESP-IDF component: EspUartLink, a core::ByteLink backed by the UART driver.
idf_component_register(
  SRCS "esp_uart_link.cpp"
  INCLUDE_DIRS "include"
  REQUIRES driver core)
