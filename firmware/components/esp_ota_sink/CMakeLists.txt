# ESP-IDF component: EspOtaSink, a core::OtaSink backed by esp_ota_*.
# Shared by every co-processor that takes firmware from the brain over the link.
idf_component_register(
  SRCS "esp_ota_sink.cpp"
  INCLUDE_DIRS "include"
  REQUIRES core hubproto app_update esp_partition esp_system esp_driver_uart nvs_flash)
