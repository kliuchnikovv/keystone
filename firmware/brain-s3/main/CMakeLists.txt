# The hub's TLS identity comes from scripts/gen-hub-cert.sh (hub_identity.cpp, not in git).
# Without it the firmware still builds and serves the app protocol over plain ws:// only
# (development); with it, wss:// is served.
set(KS_SRCS main.cpp ota_job.cpp app_server.cpp relay_client.cpp status_led.cpp)
set(KS_TLS_DEFS "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/hub_identity.cpp")
  list(APPEND KS_SRCS hub_identity.cpp)
  set(KS_TLS_DEFS KEYSTONE_HAVE_TLS=1)
endif()

idf_component_register(SRCS ${KS_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES core esp_uart_link hubproto esp_timer esp_http_client esp_http_server esp_https_server esp-tls mdns esp_websocket_client)

if(KS_TLS_DEFS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ${KS_TLS_DEFS})
endif()
