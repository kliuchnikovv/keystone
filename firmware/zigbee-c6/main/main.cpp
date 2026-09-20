// Phase 6, Stage 1 (SPEC §9: debug Zigbee separately from the protocol first).
//
// A real esp-zigbee-sdk COORDINATOR on the ESP32-C6 on-chip 802.15.4 radio.
// It forms a network, opens it for joining, and — when a real Zigbee device
// joins — discovers its active endpoints and simple descriptors and LOGS the
// clusters each endpoint exposes. That log is what tells us which ZCL clusters
// the device speaks, so Stage 2 can map them to DECLARE/STATE (SPEC §6.1) and
// wire the translator onto the UART bus to the brain.
//
// Stage 1 does NOT touch the UART link/hubproto yet — one new thing at a time.
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee_core.h"

namespace {

const char* kTag = "zigbee-c6";

// How long the network stays open for joining after formation, in seconds.
constexpr std::uint8_t kJoinWindowSec = 180;

// Primary channel mask: scan all channels (11-26). Narrow later for coexistence
// with WiFi (SPEC §10, 2.4 GHz coexistence).
constexpr std::uint32_t kPrimaryChannelMask = ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK;

// Basic cluster: read manufacturer/model to identify a joined device (SPEC §6.1
// manufacturer clusters vary by vendor; the model string disambiguates them).
constexpr std::uint16_t kBasicCluster = 0x0000;
constexpr std::uint16_t kAttrManufacturerName = 0x0004;
constexpr std::uint16_t kAttrModelIdentifier = 0x0005;

// Read ManufacturerName + ModelIdentifier from a device's endpoint 1. Called
// from the discovery flow (stack task context), so no esp_zb_lock needed.
void read_basic_info(std::uint16_t short_addr) {
  static std::uint16_t attrs[2] = {kAttrManufacturerName, kAttrModelIdentifier};
  esp_zb_zcl_read_attr_cmd_t cmd{};
  cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
  cmd.zcl_basic_cmd.dst_endpoint = 1;
  cmd.zcl_basic_cmd.src_endpoint = 1;
  cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cmd.clusterID = kBasicCluster;
  cmd.attr_number = 2;
  cmd.attr_field = attrs;
  esp_zb_zcl_read_attr_cmd_req(&cmd);
}

void log_joined_simple_desc(esp_zb_zdp_status_t status,
                            esp_zb_af_simple_desc_1_1_t* desc, void* ctx) {
  const auto short_addr = static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(ctx));
  if (status != ESP_ZB_ZDP_STATUS_SUCCESS || desc == nullptr) {
    ESP_LOGW(kTag, "simple_desc req failed for 0x%04x ep (status=%d)",
             short_addr, static_cast<int>(status));
    return;
  }
  ESP_LOGI(kTag,
           "  device 0x%04x ep %u: profile=0x%04x device_id=0x%04x "
           "in_clusters=%u out_clusters=%u",
           short_addr, desc->endpoint,
           static_cast<unsigned>(desc->app_profile_id),
           static_cast<unsigned>(desc->app_device_id),
           desc->app_input_cluster_count, desc->app_output_cluster_count);
  const std::uint16_t* clusters = desc->app_cluster_list;
  for (std::uint8_t i = 0; i < desc->app_input_cluster_count; ++i) {
    ESP_LOGI(kTag, "    [in ] cluster 0x%04x", static_cast<unsigned>(clusters[i]));
  }
  for (std::uint8_t i = 0; i < desc->app_output_cluster_count; ++i) {
    const std::uint16_t c = clusters[desc->app_input_cluster_count + i];
    ESP_LOGI(kTag, "    [out] cluster 0x%04x", static_cast<unsigned>(c));
  }
  // The application endpoint (1) carries Basic; read manufacturer/model from it.
  if (desc->endpoint == 1) {
    read_basic_info(short_addr);
  }
}

void log_joined_active_ep(esp_zb_zdp_status_t status, std::uint8_t ep_count,
                          std::uint8_t* ep_list, void* ctx) {
  const auto short_addr = static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(ctx));
  if (status != ESP_ZB_ZDP_STATUS_SUCCESS) {
    ESP_LOGW(kTag, "active_ep req failed for 0x%04x (status=%d)", short_addr,
             static_cast<int>(status));
    return;
  }
  ESP_LOGI(kTag, "device 0x%04x has %u endpoint(s); querying descriptors",
           short_addr, ep_count);
  for (std::uint8_t i = 0; i < ep_count; ++i) {
    esp_zb_zdo_simple_desc_req_param_t req{};
    req.addr_of_interest = short_addr;
    req.endpoint = ep_list[i];
    esp_zb_zdo_simple_desc_req(&req, log_joined_simple_desc, ctx);
  }
}

void discover_device(std::uint16_t short_addr) {
  ESP_LOGI(kTag, "new device joined: short_addr=0x%04x — discovering", short_addr);
  esp_zb_zdo_active_ep_req_param_t req{};
  req.addr_of_interest = short_addr;
  esp_zb_zdo_active_ep_req(
      &req, log_joined_active_ep,
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(short_addr)));
}

// esp_zb_scheduler_alarm() wants a void(uint8_t) callback; the commissioning
// entry point returns esp_err_t, so wrap it to avoid a function-type cast.
void retry_formation(std::uint8_t /*unused*/) {
  esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
}

void open_network_for_join() {
  esp_zb_bdb_open_network(kJoinWindowSec);
  ESP_LOGI(kTag, "network open for joining for %us", kJoinWindowSec);
}

// Logs a ZCL character-string attribute (Pascal-style: first byte is length).
void log_zcl_string(std::uint16_t src, const char* what,
                    const esp_zb_zcl_attribute_data_t& d) {
  if (d.type != ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING || d.value == nullptr) return;
  const auto* p = static_cast<const std::uint8_t*>(d.value);
  const std::size_t len = p[0];
  char buf[64];
  const std::size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  std::memcpy(buf, p + 1, n);
  buf[n] = '\0';
  ESP_LOGI(kTag, "device 0x%04x %s = \"%s\"", src, what, buf);
}

// esp-zigbee-sdk core action callback: fires for incoming ZCL indications
// (read/report responses, etc.) on the coordinator.
esp_err_t zcl_action_handler(esp_zb_core_action_callback_id_t cb_id,
                             const void* message) {
  switch (cb_id) {
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
      const auto* m =
          static_cast<const esp_zb_zcl_cmd_read_attr_resp_message_t*>(message);
      const std::uint16_t src = m->info.src_address.u.short_addr;
      for (const auto* v = m->variables; v != nullptr; v = v->next) {
        if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS) continue;
        const char* what = v->attribute.id == kAttrManufacturerName ? "manufacturer"
                           : v->attribute.id == kAttrModelIdentifier ? "model"
                                                                     : "attr";
        log_zcl_string(src, what, v->attribute.data);
      }
      break;
    }
    default:
      ESP_LOGD(kTag, "unhandled ZCL action 0x%x",
               static_cast<unsigned>(cb_id));
      break;
  }
  return ESP_OK;
}

}  // namespace

// esp-zigbee-sdk calls this from its stack task on every stack signal.
extern "C" void esp_zb_app_signal_handler(esp_zb_app_signal_t* signal_struct) {
  std::uint32_t* p_sg_p = signal_struct->p_app_signal;
  const esp_err_t err_status = signal_struct->esp_err_status;
  const esp_zb_app_signal_type_t sig = static_cast<esp_zb_app_signal_type_t>(*p_sg_p);

  switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
      ESP_LOGI(kTag, "stack initialized — forming network");
      esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
      break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
      if (err_status == ESP_OK) {
        ESP_LOGI(kTag, "device started; formation/steering will follow");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
      } else {
        ESP_LOGE(kTag, "device start failed (status=%d), retrying",
                 static_cast<int>(err_status));
        esp_zb_scheduler_alarm(retry_formation, 0, 1000);
      }
      break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
      if (err_status == ESP_OK) {
        esp_zb_ieee_addr_t ext_pan_id;
        esp_zb_get_extended_pan_id(ext_pan_id);
        ESP_LOGI(kTag,
                 "network formed: PAN=0x%04x channel=%u short=0x%04x",
                 esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                 esp_zb_get_short_address());
        open_network_for_join();
      } else {
        ESP_LOGE(kTag, "formation failed (status=%d), retrying",
                 static_cast<int>(err_status));
        esp_zb_scheduler_alarm(retry_formation, 0, 1000);
      }
      break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
      // On a coordinator this fires when the join window opens/closes.
      ESP_LOGI(kTag, "steering signal (status=%d)", static_cast<int>(err_status));
      break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
      auto* params = static_cast<esp_zb_zdo_signal_device_annce_params_t*>(
          esp_zb_app_signal_get_params(p_sg_p));
      discover_device(params->device_short_addr);
      break;
    }

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
      ESP_LOGI(kTag, "permit-join status changed (status=%d)",
               static_cast<int>(err_status));
      break;

    default:
      ESP_LOGD(kTag, "unhandled ZDO signal 0x%x (status=%d)",
               static_cast<unsigned>(sig), static_cast<int>(err_status));
      break;
  }
}

namespace {

void esp_zb_task(void* /*pv*/) {
  // Coordinator role config. (The ESP_ZB_ZC_CONFIG() convenience macro lives in
  // the SDK examples, not the library, so we fill esp_zb_cfg_t directly.)
  esp_zb_cfg_t zb_nwk_cfg{};
  zb_nwk_cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR;
  zb_nwk_cfg.install_code_policy = false;
  zb_nwk_cfg.nwk_cfg.zczr_cfg.max_children = 10;
  esp_zb_init(&zb_nwk_cfg);

  // Register a minimal endpoint (Basic + Identify) so the coordinator is a
  // valid ZCL node. It exposes no controllable entities of its own.
  esp_zb_cluster_list_t* clusters = esp_zb_zcl_cluster_list_create();
  esp_zb_attribute_list_t* basic =
      esp_zb_basic_cluster_create(nullptr);
  esp_zb_attribute_list_t* identify =
      esp_zb_identify_cluster_create(nullptr);
  esp_zb_cluster_list_add_basic_cluster(clusters, basic,
                                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(clusters, identify,
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

  esp_zb_ep_list_t* ep_list = esp_zb_ep_list_create();
  esp_zb_endpoint_config_t ep_cfg{};
  ep_cfg.endpoint = 1;
  ep_cfg.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  ep_cfg.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
  ep_cfg.app_device_version = 0;
  esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
  esp_zb_device_register(ep_list);

  esp_zb_core_action_handler_register(zcl_action_handler);
  esp_zb_set_primary_network_channel_set(kPrimaryChannelMask);
  ESP_ERROR_CHECK(esp_zb_start(false));
  esp_zb_stack_main_loop();
}

}  // namespace

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());

  // On-chip 802.15.4 radio, no host co-processor link (single-chip coordinator).
  esp_zb_platform_config_t config{};
  config.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
  config.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));

  ESP_LOGI(kTag, "Phase 6 Stage 1: Zigbee coordinator bring-up");
  xTaskCreate(esp_zb_task, "zb_main", 4096, nullptr, 5, nullptr);
}
