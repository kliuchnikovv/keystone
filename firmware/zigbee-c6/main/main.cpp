// Phase 6 Stage 2b (SPEC §9): the Zigbee -> DECLARE/STATE/COMMAND translator.
//
// Builds on Stage 1/2a (esp-zigbee-sdk coordinator that forms a network, admits
// a device, and discovers its clusters) by wiring the coordinator onto the
// UART bus to the brain, exactly the transport the fake device used in
// Phases 2-5. Each joined Zigbee On/Off device becomes one `switch` entity:
//
//   - join    -> register a switch entity, bind + configure On/Off reporting,
//                then send HELLO so the brain re-runs QUERY -> DECLARE.
//   - QUERY   -> DECLARE every mapped entity (switch, on_off) + DECLARE_END.
//   - On/Off report / read-resp -> STATE(on_off) up to the brain.
//   - COMMAND(on_off) from brain -> ZCL On/Off command down to the device -> CMD_ACK.
//
// Power/energy metering (clusters 0x0b04/0x0702) is Stage 2c.
//
// Two event sources publish onto the bus: the Zigbee stack task (HELLO/STATE)
// and the UART RX task (DECLARE/CMD_ACK). A mutex serializes publish; ZCL calls
// made from the UART task take esp_zb_lock (they must run under the stack lock).
//
// UART wiring (Phase 2): C6.TX(GPIO5)->S3.RX(GPIO6), C6.RX(GPIO4)<-S3.TX(GPIO7).
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_zigbee_core.h"

#include "esp_uart_link.hpp"
#include "nvs_boot_id.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "zigbee-c6";

// ---- UART link to the brain ----------------------------------------------
constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x01;
std::uint8_t g_boot_id = 0;  // persisted reboot counter (SPEC Phase 5)

core::UartTransport* g_bus = nullptr;
SemaphoreHandle_t g_bus_mtx = nullptr;

// ---- Capability model (SPEC §6.1) ----------------------------------------
// A Zigbee On/Off device maps to device_type=switch, attr on_off(0x01) bool.
constexpr std::uint8_t kOnOffAttr = 0x01;

// ---- Zigbee constants -----------------------------------------------------
constexpr std::uint8_t kCoordEndpoint = 1;
constexpr std::uint8_t kJoinWindowSec = 180;
constexpr std::uint32_t kPrimaryChannelMask = ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK;
constexpr std::uint16_t kBasicCluster = 0x0000;
constexpr std::uint16_t kAttrManufacturerName = 0x0004;
constexpr std::uint16_t kAttrModelIdentifier = 0x0005;

// ---- Entity mapping: Zigbee device <-> brain entity -----------------------
struct ZbSwitch {
  bool in_use;
  std::uint16_t entity_id;         // brain-facing id
  std::uint16_t short_addr;        // Zigbee NWK address (changes on re-pair)
  esp_zb_ieee_addr_t ieee;         // stable 64-bit address (for binding)
  std::uint8_t endpoint;           // device application endpoint
  bool on_off;                     // last known On/Off state
};

constexpr std::size_t kMaxSwitches = 8;
ZbSwitch g_switches[kMaxSwitches] = {};
std::uint16_t g_next_entity_id = 0x0100;

// IEEE address of the device currently being discovered (announce -> simple desc).
esp_zb_ieee_addr_t g_pending_ieee = {};

ZbSwitch* find_by_entity(std::uint16_t entity_id) {
  for (auto& s : g_switches) {
    if (s.in_use && s.entity_id == entity_id) return &s;
  }
  return nullptr;
}

ZbSwitch* find_by_addr(std::uint16_t short_addr, std::uint8_t endpoint) {
  for (auto& s : g_switches) {
    if (s.in_use && s.short_addr == short_addr && s.endpoint == endpoint) return &s;
  }
  return nullptr;
}

ZbSwitch* register_switch(std::uint16_t short_addr, const esp_zb_ieee_addr_t ieee,
                          std::uint8_t endpoint) {
  if (ZbSwitch* existing = find_by_addr(short_addr, endpoint)) return existing;
  for (auto& s : g_switches) {
    if (!s.in_use) {
      s.in_use = true;
      s.entity_id = g_next_entity_id++;
      s.short_addr = short_addr;
      std::memcpy(s.ieee, ieee, sizeof(esp_zb_ieee_addr_t));
      s.endpoint = endpoint;
      s.on_off = false;
      return &s;
    }
  }
  return nullptr;  // table full
}

// ---- Bus (brain) side -----------------------------------------------------
hubproto::Header make_header(hubproto::MsgType type, std::uint16_t entity_id,
                             std::uint16_t seq) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kOwnChipId;
  h.boot_id = g_boot_id;
  h.seq = seq;
  h.entity_id = entity_id;
  return h;
}

void publish_locked(const hubproto::Header& h, const std::uint8_t* buf,
                    std::size_t len) {
  if (g_bus == nullptr) return;
  xSemaphoreTake(g_bus_mtx, portMAX_DELAY);
  g_bus->publish(h, buf, len);
  xSemaphoreGive(g_bus_mtx);
}

void send_hello() {
  publish_locked(make_header(hubproto::MsgType::Hello, 0x0000, 0), nullptr, 0);
  ESP_LOGI(kTag, "sent HELLO (topology changed -> brain will re-QUERY)");
}

void send_declarations() {
  const std::uint8_t flags =
      hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable;
  std::size_t declared = 0;
  for (auto& s : g_switches) {
    if (!s.in_use) continue;
    const hubproto::AttrDecl attrs[1] = {
        {kOnOffAttr, hubproto::ValueType::Bool, flags}};
    std::uint8_t buf[16];
    auto r = hubproto::encode_declare(s.entity_id, hubproto::DeviceType::Switch,
                                      attrs, 1, buf, sizeof(buf));
    if (r.ok) {
      publish_locked(make_header(hubproto::MsgType::Declare, s.entity_id, 0), buf,
                     r.len);
      ++declared;
    }
  }
  publish_locked(make_header(hubproto::MsgType::DeclareEnd, 0x0000, 0), nullptr, 0);
  ESP_LOGI(kTag, "declared %u switch entity(ies) + DECLARE_END",
           static_cast<unsigned>(declared));
}

void send_state_on_off(const ZbSwitch& s) {
  const hubproto::StateEntry e[1] = {
      {kOnOffAttr, hubproto::Value::make_bool(s.on_off)}};
  std::uint8_t buf[16];
  auto r = hubproto::encode_state(s.entity_id, e, 1, buf, sizeof(buf));
  if (r.ok) {
    publish_locked(make_header(hubproto::MsgType::State, s.entity_id, 0), buf, r.len);
    ESP_LOGI(kTag, "STATE entity 0x%04x on_off=%d",
             static_cast<unsigned>(s.entity_id), s.on_off ? 1 : 0);
  }
}

// ---- COMMAND (brain -> device) : runs on the UART RX task -----------------
void handle_command(const hubproto::Header& h, const std::uint8_t* payload,
                    std::size_t len) {
  hubproto::Command cmd{};
  if (!hubproto::decode_command(payload, len, hubproto::ValueType::Bool, cmd)) {
    return;
  }
  ZbSwitch* s = find_by_entity(cmd.entity_id);
  hubproto::CmdAckStatus status = hubproto::CmdAckStatus::Unreachable;
  if (s != nullptr) {
    const bool on = cmd.value.as_bool();
    esp_zb_zcl_on_off_cmd_t req{};
    req.zcl_basic_cmd.dst_addr_u.addr_short = s->short_addr;
    req.zcl_basic_cmd.dst_endpoint = s->endpoint;
    req.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    // ZCL calls must run under the stack lock (we are on the UART task here).
    if (esp_zb_lock_acquire(portMAX_DELAY)) {
      esp_zb_zcl_on_off_cmd_req(&req);
      esp_zb_lock_release();
      status = hubproto::CmdAckStatus::Ok;
      ESP_LOGI(kTag, "COMMAND entity 0x%04x on_off=%d -> device 0x%04x ep%u",
               static_cast<unsigned>(cmd.entity_id), on ? 1 : 0,
               static_cast<unsigned>(s->short_addr), s->endpoint);
    } else {
      status = hubproto::CmdAckStatus::Fail;
    }
  } else {
    ESP_LOGW(kTag, "COMMAND for unknown entity 0x%04x",
             static_cast<unsigned>(cmd.entity_id));
  }
  std::uint8_t ack[1];
  if (hubproto::encode_cmd_ack(status, ack, sizeof(ack)).ok) {
    publish_locked(make_header(hubproto::MsgType::CmdAck, cmd.entity_id, h.seq), ack, 1);
  }
}

void on_uart_message(void* /*ctx*/, const hubproto::Header& h,
                     const std::uint8_t* payload, std::size_t len) {
  switch (h.type) {
    case hubproto::MsgType::Query:
      send_declarations();
      break;
    case hubproto::MsgType::Command:
      handle_command(h, payload, len);
      break;
    default:
      break;
  }
}

// ---- Zigbee -> bus : device setup after discovery -------------------------
// Bind the device's On/Off cluster to the coordinator and ask it to report
// On/Off changes, so physical state changes reach the brain as STATE.
void setup_on_off_reporting(const ZbSwitch& s) {
  esp_zb_ieee_addr_t coord_ieee;
  esp_zb_get_long_address(coord_ieee);

  esp_zb_zdo_bind_req_param_t bind{};
  std::memcpy(bind.src_address, s.ieee, sizeof(esp_zb_ieee_addr_t));
  bind.src_endp = s.endpoint;
  bind.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
  bind.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
  std::memcpy(bind.dst_address_u.addr_long, coord_ieee, sizeof(esp_zb_ieee_addr_t));
  bind.dst_endp = kCoordEndpoint;
  bind.req_dst_addr = s.short_addr;
  esp_zb_zdo_device_bind_req(&bind, nullptr, nullptr);

  esp_zb_zcl_config_report_record_t rec{};
  rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
  rec.attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;  // 0x0000
  rec.attrType = ESP_ZB_ZCL_ATTR_TYPE_BOOL;
  rec.min_interval = 0;
  rec.max_interval = 300;  // heartbeat report at least every 5 min
  rec.reportable_change = nullptr;

  esp_zb_zcl_config_report_cmd_t cfg{};
  cfg.zcl_basic_cmd.dst_addr_u.addr_short = s.short_addr;
  cfg.zcl_basic_cmd.dst_endpoint = s.endpoint;
  cfg.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
  cfg.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cfg.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
  cfg.record_number = 1;
  cfg.record_field = &rec;
  esp_zb_zcl_config_report_cmd_req(&cfg);

  ESP_LOGI(kTag, "bound + configured On/Off reporting for entity 0x%04x",
           static_cast<unsigned>(s.entity_id));
}

// Read current On/Off + Basic identity from a joined device's endpoint 1.
void read_device_state(std::uint16_t short_addr, std::uint8_t endpoint) {
  static std::uint16_t basic_attrs[2] = {kAttrManufacturerName, kAttrModelIdentifier};
  esp_zb_zcl_read_attr_cmd_t basic{};
  basic.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
  basic.zcl_basic_cmd.dst_endpoint = endpoint;
  basic.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
  basic.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  basic.clusterID = kBasicCluster;
  basic.attr_number = 2;
  basic.attr_field = basic_attrs;
  esp_zb_zcl_read_attr_cmd_req(&basic);

  static std::uint16_t onoff_attr[1] = {ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID};
  esp_zb_zcl_read_attr_cmd_t onoff{};
  onoff.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
  onoff.zcl_basic_cmd.dst_endpoint = endpoint;
  onoff.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
  onoff.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  onoff.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
  onoff.attr_number = 1;
  onoff.attr_field = onoff_attr;
  esp_zb_zcl_read_attr_cmd_req(&onoff);
}

// ---- Zigbee discovery (Stage 1/2a) ---------------------------------------
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

void log_joined_simple_desc(esp_zb_zdp_status_t status,
                            esp_zb_af_simple_desc_1_1_t* desc, void* ctx) {
  const auto short_addr = static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(ctx));
  if (status != ESP_ZB_ZDP_STATUS_SUCCESS || desc == nullptr) {
    ESP_LOGW(kTag, "simple_desc req failed for 0x%04x (status=%d)", short_addr,
             static_cast<int>(status));
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
  bool has_on_off = false;
  for (std::uint8_t i = 0; i < desc->app_input_cluster_count; ++i) {
    ESP_LOGI(kTag, "    [in ] cluster 0x%04x", static_cast<unsigned>(clusters[i]));
    if (clusters[i] == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) has_on_off = true;
  }
  for (std::uint8_t i = 0; i < desc->app_output_cluster_count; ++i) {
    const std::uint16_t c = clusters[desc->app_input_cluster_count + i];
    ESP_LOGI(kTag, "    [out] cluster 0x%04x", static_cast<unsigned>(c));
  }

  // An endpoint that exposes On/Off becomes a switch entity for the brain.
  if (has_on_off) {
    ZbSwitch* s = register_switch(short_addr, g_pending_ieee, desc->endpoint);
    if (s != nullptr) {
      ESP_LOGI(kTag, "mapped device 0x%04x ep%u -> switch entity 0x%04x",
               short_addr, desc->endpoint, static_cast<unsigned>(s->entity_id));
      setup_on_off_reporting(*s);
      read_device_state(short_addr, desc->endpoint);
      send_hello();  // topology changed -> brain re-QUERYs and gets this entity
    } else {
      ESP_LOGE(kTag, "switch table full; cannot map device 0x%04x", short_addr);
    }
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

void discover_device(std::uint16_t short_addr, const esp_zb_ieee_addr_t ieee) {
  ESP_LOGI(kTag, "new device joined: short_addr=0x%04x — discovering", short_addr);
  std::memcpy(g_pending_ieee, ieee, sizeof(esp_zb_ieee_addr_t));
  esp_zb_zdo_active_ep_req_param_t req{};
  req.addr_of_interest = short_addr;
  esp_zb_zdo_active_ep_req(
      &req, log_joined_active_ep,
      reinterpret_cast<void*>(static_cast<std::uintptr_t>(short_addr)));
}

void retry_formation(std::uint8_t /*unused*/) {
  esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
}

void open_network_for_join() {
  esp_zb_bdb_open_network(kJoinWindowSec);
  ESP_LOGI(kTag, "network open for joining for %us", kJoinWindowSec);
}

// ---- ZCL indications (reports / read responses) ---------------------------
esp_err_t zcl_action_handler(esp_zb_core_action_callback_id_t cb_id,
                             const void* message) {
  switch (cb_id) {
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
      const auto* m = static_cast<const esp_zb_zcl_report_attr_message_t*>(message);
      if (m->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
          m->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
          m->attribute.data.value != nullptr) {
        const std::uint16_t src = m->src_address.u.short_addr;
        ZbSwitch* s = find_by_addr(src, m->src_endpoint);
        if (s != nullptr) {
          s->on_off = *static_cast<const std::uint8_t*>(m->attribute.data.value) != 0;
          send_state_on_off(*s);
        }
      }
      break;
    }
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
      const auto* m =
          static_cast<const esp_zb_zcl_cmd_read_attr_resp_message_t*>(message);
      const std::uint16_t src = m->info.src_address.u.short_addr;
      for (const auto* v = m->variables; v != nullptr; v = v->next) {
        if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS) continue;
        if (m->info.cluster == kBasicCluster) {
          const char* what = v->attribute.id == kAttrManufacturerName ? "manufacturer"
                             : v->attribute.id == kAttrModelIdentifier ? "model"
                                                                       : "attr";
          log_zcl_string(src, what, v->attribute.data);
        } else if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
                   v->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
                   v->attribute.data.value != nullptr) {
          ZbSwitch* s = find_by_addr(src, m->info.src_endpoint);
          if (s != nullptr) {
            s->on_off =
                *static_cast<const std::uint8_t*>(v->attribute.data.value) != 0;
            send_state_on_off(*s);
          }
        }
      }
      break;
    }
    default:
      ESP_LOGD(kTag, "unhandled ZCL action 0x%x", static_cast<unsigned>(cb_id));
      break;
  }
  return ESP_OK;
}

}  // namespace

// esp-zigbee-sdk stack signals (formation, steering, device announce).
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
        ESP_LOGI(kTag, "network formed: PAN=0x%04x channel=%u short=0x%04x",
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
      ESP_LOGI(kTag, "steering signal (status=%d)", static_cast<int>(err_status));
      break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
      auto* params = static_cast<esp_zb_zdo_signal_device_annce_params_t*>(
          esp_zb_app_signal_get_params(p_sg_p));
      discover_device(params->device_short_addr, params->ieee_addr);
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
  esp_zb_cfg_t zb_nwk_cfg{};
  zb_nwk_cfg.esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR;
  zb_nwk_cfg.install_code_policy = false;
  zb_nwk_cfg.nwk_cfg.zczr_cfg.max_children = 10;
  esp_zb_init(&zb_nwk_cfg);

  // Minimal Basic + Identify endpoint so the coordinator is a valid ZCL node.
  esp_zb_cluster_list_t* clusters = esp_zb_zcl_cluster_list_create();
  esp_zb_cluster_list_add_basic_cluster(clusters,
                                        esp_zb_basic_cluster_create(nullptr),
                                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(clusters,
                                           esp_zb_identify_cluster_create(nullptr),
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_ep_list_t* ep_list = esp_zb_ep_list_create();
  esp_zb_endpoint_config_t ep_cfg{};
  ep_cfg.endpoint = kCoordEndpoint;
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
  g_boot_id = fw::next_boot_id();  // persisted reboot counter (SPEC Phase 5)

  // Bring up the UART link + transport to the brain first (Phase 2 transport).
  g_bus_mtx = xSemaphoreCreateMutex();
  static fw::EspUartLink link([] {
    fw::EspUartLink::Config c;
    c.port = UART_NUM_1;
    c.tx_gpio = kUartTxGpio;
    c.rx_gpio = kUartRxGpio;
    return c;
  }());
  ESP_ERROR_CHECK(link.begin());
  static core::UartTransport bus(link);
  g_bus = &bus;
  bus.subscribe(&on_uart_message, nullptr);
  link.start();
  ESP_LOGI(kTag, "chip 0x%02x up; boot_id=%u", kOwnChipId,
           static_cast<unsigned>(g_boot_id));

  // Announce to the brain now; declarations follow as devices join.
  send_hello();

  // Then start the Zigbee coordinator.
  esp_zb_platform_config_t config{};
  config.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
  config.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));
  ESP_LOGI(kTag, "Phase 6 Stage 2b: Zigbee translator + bus");
  xTaskCreate(esp_zb_task, "zb_main", 4096, nullptr, 5, nullptr);
}
