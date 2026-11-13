# The hub's TLS identity comes from scripts/gen-hub-cert.sh (hub_identity.cpp, not in git).
# Without it the firmware still builds and serves the app protocol over plain ws:// only
# (development); with it, wss:// is served.
set(KS_SRCS main.cpp ota_job.cpp device_ota_job.cpp app_server.cpp relay_client.cpp status_led.cpp)
set(KS_DEFS "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/hub_identity.cpp")
  list(APPEND KS_SRCS hub_identity.cpp)
  list(APPEND KS_DEFS KEYSTONE_HAVE_TLS=1)
endif()

set(KS_REQUIRES core esp_uart_link hubproto esp_timer esp_http_client esp_http_server esp_https_server esp-tls mdns esp_websocket_client
                # PlatformIO adds the components of the headers a source includes; with
                # plain idf.py they have to be named here.
                esp_wifi esp_event esp_netif console esp_app_format nvs_flash)

# experiment/commissioner-on-s3: when the Matter SDK is on the path (Docker build),
# link against esp_matter + the controller/commissioner library, and tell main.cpp
# the Matter hooks (esp_matter::start, controller_client::init/setup_commissioner)
# are available via KEYSTONE_HAVE_MATTER. PlatformIO builds without the env var
# and compiles exactly the same main.cpp with the Matter hooks #ifdef-ed out.
if(DEFINED ENV{ESP_MATTER_PATH})
    list(APPEND KS_REQUIRES esp_matter esp_matter_controller)
    list(APPEND KS_DEFS KEYSTONE_HAVE_MATTER=1)
    # The PAA trust store for the commissioner's attestation verification
    # (it includes CHIP headers, so it only builds with the Matter SDK).
    list(APPEND KS_SRCS matter_paa.cpp)
    # The hub's own commissioner and the bridge from a commissioned node to hub
    # entities (chip 0x03). Firmware-only: the core never sees CHIP.
    list(APPEND KS_SRCS matter_bridge.cpp)
endif()

idf_component_register(SRCS ${KS_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES ${KS_REQUIRES})

if(KS_DEFS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ${KS_DEFS})
endif()
