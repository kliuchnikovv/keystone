# ESP-IDF component: EspUartLink, a core::ByteLink backed by the UART driver.
idf_component_register(
  SRCS "esp_uart_link.cpp"
  INCLUDE_DIRS "include"
  # driver/uart.h lives in esp_driver_uart since ESP-IDF 5.3 (the monolithic
  # `driver` component was split into esp_driver_*). REQUIRES is public so
  # consumers of esp_uart_link.hpp also get the include path.
  REQUIRES esp_driver_uart core)
