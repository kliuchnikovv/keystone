# ESP-IDF component: a persistent reboot counter in NVS (SPEC Phase 5).
idf_component_register(
  SRCS "nvs_boot_id.cpp"
  INCLUDE_DIRS "include"
  REQUIRES nvs_flash)
