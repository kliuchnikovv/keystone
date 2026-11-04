# The hub's TLS identity comes from scripts/gen-hub-cert.sh (hub_identity.cpp, not in git).
# Without it the firmware still builds and serves the app protocol over plain ws:// only
# (development); with it, wss:// is served.
set(KS_SRCS main.cpp ota_job.cpp device_ota_job.cpp app_server.cpp relay_client.cpp status_led.cpp)
set(KS_TLS_DEFS "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/hub_identity.cpp")
  list(APPEND KS_SRCS hub_identity.cpp)
  set(KS_TLS_DEFS KEYSTONE_HAVE_TLS=1)
endif()

set(KS_REQUIRES core esp_uart_link hubproto esp_timer esp_http_client esp_http_server esp_https_server esp-tls mdns esp_websocket_client
                # PlatformIO adds the components of the headers a source includes; with
                # plain idf.py they have to be named here.
                esp_wifi esp_event esp_netif console esp_app_format nvs_flash)

# experiment/commissioner-on-s3: when the Matter SDK is on the path (Docker build),
# link against esp_matter + the controller/commissioner library. No main.cpp code
# references them yet — this commit measures the static footprint of adding the
# controller before any init code goes in. See docs/matter-backlog.md §3.
if(DEFINED ENV{ESP_MATTER_PATH})
    list(APPEND KS_REQUIRES esp_matter esp_matter_controller)
endif()

idf_component_register(SRCS ${KS_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES ${KS_REQUIRES})

if(KS_TLS_DEFS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ${KS_TLS_DEFS})
endif()
