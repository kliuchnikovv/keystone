// Phase 6 Stage 2b (SPEC §9): the Zigbee -> DECLARE/STATE/COMMAND translator.
//
// Builds on Stage 1/2a (esp-zigbee-sdk coordinator that forms a network, admits
// a device, and discovers its clusters) by wiring the coordinator onto the
// UART bus to the brain. Survives reboots: the Zigbee network (PAN, key,
// channel) is persisted by ZBOSS, and the entity table is persisted in NVS;
// on reboot the coordinator restores the same network so devices stay joined,
// then re-arms reporting and reads current state for each entity.
//
// Each joined Zigbee On/Off device becomes one `switch` entity:
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
#include "esp_timer.h"
#include "nvs.h"
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
// A Zigbee On/Off device maps to device_type=switch, attr on_off(0x01) bool,
// plus optional metering attributes when the device exposes them.
constexpr std::uint8_t kOnOffAttr = 0x01;
constexpr std::uint8_t kPowerWAttr = 0x13;   // instantaneous active power, W
constexpr std::uint8_t kEnergyWhAttr = 0x14; // cumulative energy, Wh (U32)
constexpr std::uint16_t kPowerUnpublished = 0xFFFF;
constexpr std::uint32_t kEnergyUnpublished = 0xFFFFFFFF;

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
  bool on_off_valid;               // true once we've received any On/Off value
  bool has_metering;               // device exposes Electrical Measurement (0x0b04)
  std::uint16_t ac_power_mul;      // ActivePower multiplier (default 1)
  std::uint16_t ac_power_div;      // ActivePower divisor    (default 1)
  std::uint16_t last_power_w;      // last computed power in W (for STATE dedup)
  bool has_energy_meter;           // device exposes Metering cluster (0x0702)
  std::uint32_t metering_mul;      // Metering Multiplier (default 1)
  std::uint32_t metering_div;      // Metering Divisor    (default 1)
  std::uint32_t last_energy_wh;    // last sent energy in Wh (for STATE dedup)
};

constexpr std::size_t kMaxSwitches = 32;
ZbSwitch g_switches[kMaxSwitches] = {};
std::uint16_t g_next_entity_id = 0x0100;

// IEEE address of the device currently being discovered (announce -> simple desc).
esp_zb_ieee_addr_t g_pending_ieee = {};

// Set once we've re-armed reporting for NVS-restored devices after a reboot.
bool g_rearm_scheduled = false;

// Pending brain COMMANDs awaiting a ZCL Default Response from the device.
// Matched by ZCL tsn returned from esp_zb_zcl_on_off_cmd_req(): only on success
// do we send CmdAck Ok + STATE back to the brain. Failures become CmdAck Fail;
// no response at all -> brain's CommandDispatcher times out to Unreachable.
struct PendingCmd {
  bool in_use;
  std::uint8_t tsn;
  std::uint16_t src_short;       // device we sent to (for extra safety)
  std::uint16_t entity_id;
  std::uint16_t hubproto_seq;    // brain's seq, to echo in CmdAck
  bool target_on;
};
PendingCmd g_pending[8] = {};

PendingCmd* find_pending(std::uint8_t tsn, std::uint16_t src_short) {
  for (auto& p : g_pending) {
    if (p.in_use && p.tsn == tsn && p.src_short == src_short) return &p;
  }
  return nullptr;
}

PendingCmd* alloc_pending() {
  for (auto& p : g_pending) {
    if (!p.in_use) return &p;
  }
  return nullptr;
}

// NVS persistence: the switch table survives a C6 reflash/reboot, so a device
// that is still joined (its bindings + reporting config persist in the Zigbee
// stack) is re-declared to the brain without a factory-reset/re-pair.
const char* kNvsNamespace = "keystone";
const char* kNvsSwitches = "zb_sw";
const char* kNvsNextId = "zb_next";

void persist_switches() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, kNvsSwitches, g_switches, sizeof(g_switches));
  nvs_set_u16(h, kNvsNextId, g_next_entity_id);
  nvs_commit(h);
  nvs_close(h);
}

void restore_switches() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::size_t sz = sizeof(g_switches);
  nvs_get_blob(h, kNvsSwitches, g_switches, &sz);
  nvs_get_u16(h, kNvsNextId, &g_next_entity_id);
  nvs_close(h);
  std::size_t n = 0;
  for (auto& s : g_switches) {
    if (!s.in_use) continue;
    if (s.ac_power_div == 0) s.ac_power_div = 1;
    if (s.ac_power_mul == 0) s.ac_power_mul = 1;
    if (s.metering_div == 0) s.metering_div = 1;
    if (s.metering_mul == 0) s.metering_mul = 1;
    if (s.last_energy_wh == 0 && !s.has_energy_meter) s.last_energy_wh = kEnergyUnpublished;
    ++n;
  }
  if (n > 0) ESP_LOGI(kTag, "restored %u switch entity(ies) from NVS",
                      static_cast<unsigned>(n));
}

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

// Identity is the stable IEEE address: a re-paired device keeps its entity_id
// and just updates its (volatile) short address.
ZbSwitch* register_switch(std::uint16_t short_addr, const esp_zb_ieee_addr_t ieee,
                          std::uint8_t endpoint) {
  for (auto& s : g_switches) {
    if (s.in_use && std::memcmp(s.ieee, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) {
      s.short_addr = short_addr;
      s.endpoint = endpoint;
      persist_switches();
      return &s;
    }
  }
  for (auto& s : g_switches) {
    if (!s.in_use) {
      s.in_use = true;
      s.entity_id = g_next_entity_id++;
      s.short_addr = short_addr;
      std::memcpy(s.ieee, ieee, sizeof(esp_zb_ieee_addr_t));
      s.endpoint = endpoint;
      s.on_off = false;
      s.on_off_valid = false;
      s.has_metering = false;
      s.ac_power_mul = 1;
      s.ac_power_div = 1;
      s.last_power_w = kPowerUnpublished;
      s.has_energy_meter = false;
      s.metering_mul = 1;
      s.metering_div = 1;
      s.last_energy_wh = kEnergyUnpublished;
      persist_switches();
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

// Fires from an esp_timer; heartbeats every 5s so the brain can (a) see this
// chip's liveness in `chips` and (b) self-heal after a brain reboot even
// without a fresh HELLO from our side (any inbound message wakes up
// RadioCoordinator's QUERY on an unknown chip).
void heartbeat_cb(void* /*arg*/) {
  publish_locked(make_header(hubproto::MsgType::Heartbeat, 0x0000, 0), nullptr, 0);
}

void send_hello() {
  publish_locked(make_header(hubproto::MsgType::Hello, 0x0000, 0), nullptr, 0);
  ESP_LOGI(kTag, "sent HELLO (topology changed -> brain will re-QUERY)");
}

void send_declarations() {
  const std::uint8_t rw_report =
      hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable;
  const std::uint8_t r_report =
      hubproto::kFlagReadable | hubproto::kFlagReportable;
  std::size_t declared = 0;
  for (auto& s : g_switches) {
    if (!s.in_use) continue;
    // Always: on_off. Optionally: power_w (Electrical Measurement) and/or energy_wh (Metering).
    hubproto::AttrDecl attrs[3] = {
        {kOnOffAttr, hubproto::ValueType::Bool, rw_report}, {}, {}};
    std::uint8_t attr_count = 1;
    if (s.has_metering) {
      attrs[attr_count++] = {kPowerWAttr, hubproto::ValueType::U16, r_report};
    }
    if (s.has_energy_meter) {
      attrs[attr_count++] = {kEnergyWhAttr, hubproto::ValueType::U32, r_report};
    }
    std::uint8_t buf[32];
    auto r = hubproto::encode_declare(s.entity_id, hubproto::DeviceType::Switch,
                                      attrs, attr_count, buf, sizeof(buf));
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

void send_state_power_w(ZbSwitch& s, std::uint16_t watts) {
  // Sentinel: means "never published"; the first STATE always goes through
  // even if the reading is 0. Subsequent duplicates are dropped as noise.
  if (watts == s.last_power_w && s.last_power_w != kPowerUnpublished) return;
  s.last_power_w = watts;
  const hubproto::StateEntry e[1] = {{kPowerWAttr, hubproto::Value::make_u16(watts)}};
  std::uint8_t buf[16];
  auto r = hubproto::encode_state(s.entity_id, e, 1, buf, sizeof(buf));
  if (r.ok) {
    publish_locked(make_header(hubproto::MsgType::State, s.entity_id, 0), buf, r.len);
    ESP_LOGI(kTag, "STATE entity 0x%04x power_w=%u",
             static_cast<unsigned>(s.entity_id), static_cast<unsigned>(watts));
  }
}

void send_state_energy_wh(ZbSwitch& s, std::uint32_t wh) {
  if (wh == s.last_energy_wh) return;
  s.last_energy_wh = wh;
  const hubproto::StateEntry e[1] = {{kEnergyWhAttr, hubproto::Value::make_u32(wh)}};
  std::uint8_t buf[16];
  auto r = hubproto::encode_state(s.entity_id, e, 1, buf, sizeof(buf));
  if (r.ok) {
    publish_locked(make_header(hubproto::MsgType::State, s.entity_id, 0), buf, r.len);
    ESP_LOGI(kTag, "STATE entity 0x%04x energy_wh=%lu",
             static_cast<unsigned>(s.entity_id), static_cast<unsigned long>(wh));
  }
}

// Scale raw CurrentSummationDelivered (uint48, lower 32 bits) by
// Metering Multiplier/Divisor → Wh. UnitOfMeasure=0 means kWh per ZCL spec,
// but IKEA uses divisor=1 and raw value already in Wh. We don't read
// UnitOfMeasure; just apply raw * mul / div and let the brain display it.
std::uint32_t scale_summation(const ZbSwitch& s, std::uint64_t raw) {
  if (s.metering_div == 0) return 0;
  const std::uint64_t scaled =
      raw * static_cast<std::uint64_t>(s.metering_mul) /
      static_cast<std::uint64_t>(s.metering_div);
  return scaled > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(scaled);
}

// Apply the cached ACPower multiplier/divisor to a raw ActivePower reading
// (spec: `real_power = raw * multiplier / divisor` in Watts). Clamps to u16.
std::uint16_t scale_active_power(const ZbSwitch& s, std::int16_t raw) {
  if (s.ac_power_div == 0) return 0;  // guard, restore also clamps but be safe
  const std::int32_t scaled =
      (static_cast<std::int32_t>(raw) * static_cast<std::int32_t>(s.ac_power_mul)) /
      static_cast<std::int32_t>(s.ac_power_div);
  if (scaled < 0) return 0;
  if (scaled > 0xFFFF) return 0xFFFF;
  return static_cast<std::uint16_t>(scaled);
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
  if (s == nullptr) {
    ESP_LOGW(kTag, "COMMAND for unknown entity 0x%04x",
             static_cast<unsigned>(cmd.entity_id));
    std::uint8_t ack[1];
    hubproto::encode_cmd_ack(hubproto::CmdAckStatus::Unreachable, ack, sizeof(ack));
    publish_locked(make_header(hubproto::MsgType::CmdAck, cmd.entity_id, h.seq), ack, 1);
    return;
  }

  PendingCmd* p = alloc_pending();
  if (p == nullptr) {
    ESP_LOGW(kTag, "pending cmd table full; dropping command");
    std::uint8_t ack[1];
    hubproto::encode_cmd_ack(hubproto::CmdAckStatus::Fail, ack, sizeof(ack));
    publish_locked(make_header(hubproto::MsgType::CmdAck, cmd.entity_id, h.seq), ack, 1);
    return;
  }

  const bool on = cmd.value.as_bool();
  esp_zb_zcl_on_off_cmd_t req{};
  req.zcl_basic_cmd.dst_addr_u.addr_short = s->short_addr;
  req.zcl_basic_cmd.dst_endpoint = s->endpoint;
  req.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
  req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  req.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;

  // ZCL calls must run under the stack lock (we are on the UART task here).
  // Reply to the brain only after the device's Default Response arrives — no
  // more optimistic Ok. If the device never responds, the brain's dispatcher
  // times out to Unreachable via its own retries.
  std::uint8_t tsn = 0;
  if (esp_zb_lock_acquire(portMAX_DELAY)) {
    tsn = esp_zb_zcl_on_off_cmd_req(&req);
    esp_zb_lock_release();
  }
  p->in_use = true;
  p->tsn = tsn;
  p->src_short = s->short_addr;
  p->entity_id = cmd.entity_id;
  p->hubproto_seq = h.seq;
  p->target_on = on;
  ESP_LOGI(kTag, "COMMAND entity 0x%04x on_off=%d -> device 0x%04x ep%u tsn=%u",
           static_cast<unsigned>(cmd.entity_id), on ? 1 : 0,
           static_cast<unsigned>(s->short_addr), s->endpoint,
           static_cast<unsigned>(tsn));
}

void read_device_state(std::uint16_t short_addr, std::uint8_t endpoint);

// esp_zb_scheduler_alarm callback: re-read current state from every mapped
// device (ZCL calls must run under the stack lock; scheduler runs in stack
// context, so we don't need to acquire it explicitly).
void refresh_states_cb(std::uint8_t /*unused*/) {
  for (const auto& s : g_switches) {
    if (s.in_use) read_device_state(s.short_addr, s.endpoint);
  }
}

// Chip-level "meta" commands: a Command with entity_id=kSelfEntityId asks the
// coordinator itself to do something (open the join window, drop a device,
// blink identify) rather than acting on an entity. attr_id picks the op, the
// u16 value is its argument. Reused Command frame so no wire-protocol change.
constexpr std::uint8_t kMetaPair = 0x01;      // arg = seconds to open network
constexpr std::uint8_t kMetaUnpair = 0x02;    // arg = entity_id to remove
constexpr std::uint8_t kMetaIdentify = 0x03;  // arg = entity_id, blinks 10s

void handle_meta(std::uint8_t attr, std::uint16_t arg) {
  switch (attr) {
    case kMetaPair: {
      const std::uint16_t sec = arg == 0 ? 180 : arg;
      if (esp_zb_lock_acquire(portMAX_DELAY)) {
        esp_zb_bdb_open_network(sec > 254 ? 254 : static_cast<std::uint8_t>(sec));
        esp_zb_lock_release();
      }
      ESP_LOGI(kTag, "meta: pair (open network for %us)", static_cast<unsigned>(sec));
      break;
    }
    case kMetaUnpair: {
      ZbSwitch* s = find_by_entity(arg);
      if (s == nullptr) {
        ESP_LOGW(kTag, "meta: unpair unknown entity 0x%04x", static_cast<unsigned>(arg));
        break;
      }
      esp_zb_zdo_mgmt_leave_req_param_t leave{};
      std::memcpy(leave.device_address, s->ieee, sizeof(esp_zb_ieee_addr_t));
      leave.dst_nwk_addr = s->short_addr;  // send Leave to the device itself
      leave.remove_children = 0;
      leave.rejoin = 0;
      if (esp_zb_lock_acquire(portMAX_DELAY)) {
        esp_zb_zdo_device_leave_req(&leave, nullptr, nullptr);
        esp_zb_lock_release();
      }
      ESP_LOGI(kTag, "meta: unpair entity 0x%04x (device 0x%04x)",
               static_cast<unsigned>(arg), static_cast<unsigned>(s->short_addr));
      s->in_use = false;
      persist_switches();
      break;
    }
    case kMetaIdentify: {
      ZbSwitch* s = find_by_entity(arg);
      if (s == nullptr) {
        ESP_LOGW(kTag, "meta: identify unknown entity 0x%04x", static_cast<unsigned>(arg));
        break;
      }
      esp_zb_zcl_identify_cmd_t cmd{};
      cmd.zcl_basic_cmd.dst_addr_u.addr_short = s->short_addr;
      cmd.zcl_basic_cmd.dst_endpoint = s->endpoint;
      cmd.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
      cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
      cmd.identify_time = 10;  // seconds
      if (esp_zb_lock_acquire(portMAX_DELAY)) {
        esp_zb_zcl_identify_cmd_req(&cmd);
        esp_zb_lock_release();
      }
      ESP_LOGI(kTag, "meta: identify entity 0x%04x for 10s",
               static_cast<unsigned>(arg));
      break;
    }
    default:
      ESP_LOGW(kTag, "meta: unknown attr 0x%02x", static_cast<unsigned>(attr));
      break;
  }
}

void on_uart_message(void* /*ctx*/, const hubproto::Header& h,
                     const std::uint8_t* payload, std::size_t len) {
  switch (h.type) {
    case hubproto::MsgType::Query:
      send_declarations();
      // Also re-read current state from each device so the brain gets a fresh
      // STATE for every entity after (re-)declaration, not just at first join.
      esp_zb_scheduler_alarm(refresh_states_cb, 0, 100);
      break;
    case hubproto::MsgType::Command: {
      // Peek entity_id before decoding — chip-meta commands live at entity_id 0
      // and have no registry-backed value_type, so decode_command wouldn't work
      // for them anyway.
      if (len >= 5) {
        const std::uint16_t ent = static_cast<std::uint16_t>(payload[0] | (payload[1] << 8));
        if (ent == hubproto::kSelfEntityId) {
          const std::uint8_t attr = payload[2];
          const std::uint16_t arg = static_cast<std::uint16_t>(payload[3] | (payload[4] << 8));
          handle_meta(attr, arg);
          break;
        }
      }
      handle_command(h, payload, len);
      break;
    }
    default:
      break;
  }
}

// ---- Zigbee -> bus : device setup after discovery -------------------------
// Bind a cluster from the device to the coordinator so unicast reports arrive
// at our endpoint. Fire-and-forget; the ZDO answer isn't tracked.
void bind_cluster(const ZbSwitch& s, std::uint16_t cluster_id) {
  esp_zb_ieee_addr_t coord_ieee;
  esp_zb_get_long_address(coord_ieee);

  esp_zb_zdo_bind_req_param_t bind{};
  std::memcpy(bind.src_address, s.ieee, sizeof(esp_zb_ieee_addr_t));
  bind.src_endp = s.endpoint;
  bind.cluster_id = cluster_id;
  bind.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
  std::memcpy(bind.dst_address_u.addr_long, coord_ieee, sizeof(esp_zb_ieee_addr_t));
  bind.dst_endp = kCoordEndpoint;
  bind.req_dst_addr = s.short_addr;
  esp_zb_zdo_device_bind_req(&bind, nullptr, nullptr);
}

// Ask the device to send periodic reports for `attr_id` in `cluster_id`.
// `attr_type` is the ZCL wire type (BOOL, S16, etc). `min`/`max` are seconds;
// `delta` is the reportable change (nullptr = "any change").
void configure_report(const ZbSwitch& s, std::uint16_t cluster_id,
                      std::uint16_t attr_id, std::uint8_t attr_type,
                      std::uint16_t min_s, std::uint16_t max_s,
                      void* delta) {
  esp_zb_zcl_config_report_record_t rec{};
  rec.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
  rec.attributeID = attr_id;
  rec.attrType = attr_type;
  rec.min_interval = min_s;
  rec.max_interval = max_s;
  rec.reportable_change = delta;

  esp_zb_zcl_config_report_cmd_t cfg{};
  cfg.zcl_basic_cmd.dst_addr_u.addr_short = s.short_addr;
  cfg.zcl_basic_cmd.dst_endpoint = s.endpoint;
  cfg.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
  cfg.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cfg.clusterID = cluster_id;
  cfg.record_number = 1;
  cfg.record_field = &rec;
  esp_zb_zcl_config_report_cmd_req(&cfg);
}

// Bind + configure reporting for every cluster the device exposes that we
// translate. Called at fresh join and again at boot for restored devices.
void setup_reporting(const ZbSwitch& s) {
  bind_cluster(s, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
  configure_report(s, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                   ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                   0, 300, nullptr);
  if (s.has_metering) {
    // ActivePower: report if it moved by >=1 W, at least every 30s and no
    // more than once per 5s (min_interval throttles the device).
    static std::int16_t kPowerDelta = 1;
    bind_cluster(s, ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT);
    configure_report(s, ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                     ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,
                     ESP_ZB_ZCL_ATTR_TYPE_S16, 5, 30, &kPowerDelta);
  }
  if (s.has_energy_meter) {
    // CurrentSummationDelivered: UINT48 (6 bytes LE). SDK requires a non-null
    // delta pointer for analog types — passing nullptr crashes (memcpy from 0).
    // delta=1 Wh means report on any 1 Wh change, at most every 5s.
    static std::uint8_t kEnergyDelta[6] = {1, 0, 0, 0, 0, 0};  // 1 Wh UINT48 LE
    bind_cluster(s, ESP_ZB_ZCL_CLUSTER_ID_METERING);
    configure_report(s, ESP_ZB_ZCL_CLUSTER_ID_METERING,
                     ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                     ESP_ZB_ZCL_ATTR_TYPE_U48, 5, 3600, kEnergyDelta);
  }
  const char* suffix = (s.has_metering && s.has_energy_meter) ? " (on_off + power + energy)"
                       : s.has_metering                       ? " (on_off + power)"
                       : s.has_energy_meter                   ? " (on_off + energy)"
                                                              : " (on_off)";
  ESP_LOGI(kTag, "bound + configured reporting for entity 0x%04x%s",
           static_cast<unsigned>(s.entity_id), suffix);
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

  // If this device advertised Electrical Measurement, also read ActivePower +
  // its scaling factors (AC Power Multiplier / Divisor). The response handler
  // caches the factors and, when a subsequent ActivePower value arrives,
  // scales it to Watts and publishes STATE(power_w). This works even without
  // configured reporting.
  ZbSwitch* sw = find_by_addr(short_addr, endpoint);
  if (sw != nullptr && sw->has_metering) {
    static std::uint16_t em_attrs[3] = {
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,        // 0x050B
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_MULTIPLIER_ID,  // 0x0604
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_DIVISOR_ID,     // 0x0605
    };
    esp_zb_zcl_read_attr_cmd_t em{};
    em.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    em.zcl_basic_cmd.dst_endpoint = endpoint;
    em.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
    em.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    em.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT;
    em.attr_number = 3;
    em.attr_field = em_attrs;
    esp_zb_zcl_read_attr_cmd_req(&em);
  }
  if (sw != nullptr && sw->has_energy_meter) {
    static std::uint16_t metering_attrs[3] = {
        ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
        ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
    };
    esp_zb_zcl_read_attr_cmd_t metering{};
    metering.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    metering.zcl_basic_cmd.dst_endpoint = endpoint;
    metering.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
    metering.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    metering.clusterID = ESP_ZB_ZCL_CLUSTER_ID_METERING;
    metering.attr_number = 3;
    metering.attr_field = metering_attrs;
    esp_zb_zcl_read_attr_cmd_req(&metering);
  }
}

// Callback for boot-time NWK address lookup (esp_zb_zdo_nwk_addr_req).
// ctx = index into g_switches (pointer-as-integer, same pattern as short_addr).
// Runs in Zigbee stack context.
void on_nwk_addr_resp(esp_zb_zdp_status_t status, esp_zb_zdo_nwk_addr_rsp_t* resp,
                      void* ctx) {
  const auto idx = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(ctx));
  if (idx >= kMaxSwitches) return;
  ZbSwitch& s = g_switches[idx];
  if (!s.in_use) return;

  const std::uint16_t nwk_addr = (resp != nullptr) ? resp->nwk_addr : 0xFFFF;

  if (status != ESP_ZB_ZDP_STATUS_SUCCESS) {
    // Device is currently offline. It will send DEVICE_ANNCE when it comes back;
    // discover_device() will then update its short_addr and rearm it.
    ESP_LOGW(kTag,
             "NWK addr lookup failed for entity 0x%04x (offline?) — will rearm on announce",
             static_cast<unsigned>(s.entity_id));
    return;
  }

  if (s.short_addr != nwk_addr) {
    ESP_LOGI(kTag, "entity 0x%04x: short addr 0x%04x -> 0x%04x (updated)",
             static_cast<unsigned>(s.entity_id),
             static_cast<unsigned>(s.short_addr), static_cast<unsigned>(nwk_addr));
    s.short_addr = nwk_addr;
    persist_switches();
  }

  // Publish cached STATE so the brain's `home` shows the last-known value
  // immediately while the fresh async read is in flight.
  if (s.on_off_valid) send_state_on_off(s);
  if (s.has_metering && s.last_power_w != kPowerUnpublished) {
    const std::uint16_t cached = s.last_power_w;
    s.last_power_w = kPowerUnpublished;  // force past the dedup guard
    send_state_power_w(s, cached);
  }
  if (s.has_energy_meter && s.last_energy_wh != kEnergyUnpublished) {
    const std::uint32_t cached = s.last_energy_wh;
    s.last_energy_wh = kEnergyUnpublished;  // force past the dedup guard
    send_state_energy_wh(s, cached);
  }

  setup_reporting(s);
  read_device_state(nwk_addr, s.endpoint);
  ESP_LOGI(kTag, "re-armed entity 0x%04x (device 0x%04x)",
           static_cast<unsigned>(s.entity_id), static_cast<unsigned>(nwk_addr));
}

// After a reboot, entities come back from NVS but their reporting/bindings and
// current state were never re-established this run. Once the network is up,
// broadcast a ZDO NWK address request for each restored entity to confirm its
// current short address (which may have changed), then rearm reporting and read
// live state. Runs in stack context via a scheduler alarm.
void rearm_restored(std::uint8_t /*unused*/) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < kMaxSwitches; ++i) {
    ZbSwitch& s = g_switches[i];
    if (!s.in_use) continue;
    ESP_LOGI(kTag,
             "boot re-discover entity 0x%04x ieee=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             static_cast<unsigned>(s.entity_id),
             s.ieee[7], s.ieee[6], s.ieee[5], s.ieee[4],
             s.ieee[3], s.ieee[2], s.ieee[1], s.ieee[0]);
    esp_zb_zdo_nwk_addr_req_param_t req{};
    std::memcpy(req.ieee_addr_of_interest, s.ieee, sizeof(esp_zb_ieee_addr_t));
    req.dst_nwk_addr = 0xFFFF;  // broadcast — device may have a different short addr
    req.request_type = 0;       // single-device response
    req.start_index = 0;
    esp_zb_zdo_nwk_addr_req(&req, on_nwk_addr_resp,
                             reinterpret_cast<void*>(static_cast<std::uintptr_t>(i)));
    ++n;
  }
  if (n > 0)
    ESP_LOGI(kTag, "boot re-discover: issued NWK addr requests for %u entity(ies)",
             static_cast<unsigned>(n));
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
  bool has_metering = false;
  bool has_energy = false;
  for (std::uint8_t i = 0; i < desc->app_input_cluster_count; ++i) {
    ESP_LOGI(kTag, "    [in ] cluster 0x%04x", static_cast<unsigned>(clusters[i]));
    if (clusters[i] == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) has_on_off = true;
    if (clusters[i] == ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT) has_metering = true;
    if (clusters[i] == ESP_ZB_ZCL_CLUSTER_ID_METERING) has_energy = true;
  }
  for (std::uint8_t i = 0; i < desc->app_output_cluster_count; ++i) {
    const std::uint16_t c = clusters[desc->app_input_cluster_count + i];
    ESP_LOGI(kTag, "    [out] cluster 0x%04x", static_cast<unsigned>(c));
  }

  // An endpoint that exposes On/Off becomes a switch entity for the brain.
  if (has_on_off) {
    ZbSwitch* s = register_switch(short_addr, g_pending_ieee, desc->endpoint);
    if (s != nullptr) {
      s->has_metering = has_metering;
      s->has_energy_meter = has_energy;
      persist_switches();
      const char* suffix2 = (has_metering && has_energy) ? " (power + energy)"
                            : has_metering                ? " (with power)"
                            : has_energy                  ? " (with energy)"
                                                          : "";
      ESP_LOGI(kTag, "mapped device 0x%04x ep%u -> switch entity 0x%04x%s",
               short_addr, desc->endpoint, static_cast<unsigned>(s->entity_id), suffix2);
      setup_reporting(*s);
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
      if (m->attribute.data.value == nullptr) break;
      const std::uint16_t src = m->src_address.u.short_addr;
      ZbSwitch* s = find_by_addr(src, m->src_endpoint);
      if (s == nullptr) break;
      if (m->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
          m->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        s->on_off = *static_cast<const std::uint8_t*>(m->attribute.data.value) != 0;
        s->on_off_valid = true;
        persist_switches();
        send_state_on_off(*s);
      } else if (m->cluster == ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT &&
                 m->attribute.id == ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID) {
        const auto* raw = static_cast<const std::uint8_t*>(m->attribute.data.value);
        const std::int16_t rawP = static_cast<std::int16_t>(raw[0] | (raw[1] << 8));
        send_state_power_w(*s, scale_active_power(*s, rawP));
      } else if (m->cluster == ESP_ZB_ZCL_CLUSTER_ID_METERING &&
                 m->attribute.id ==
                     ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID) {
        // CurrentSummationDelivered: UINT48, 6 bytes LE
        const auto* raw = static_cast<const std::uint8_t*>(m->attribute.data.value);
        std::uint64_t summation = 0;
        for (int b = 5; b >= 0; --b) summation = (summation << 8) | raw[b];
        send_state_energy_wh(*s, scale_summation(*s, summation));
      }
      break;
    }
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID: {
      const auto* m =
          static_cast<const esp_zb_zcl_cmd_default_resp_message_t*>(message);
      const std::uint8_t tsn = m->info.header.tsn;
      const std::uint16_t src = m->info.src_address.u.short_addr;
      PendingCmd* p = find_pending(tsn, src);
      if (p == nullptr) break;  // not one of our tracked commands
      const bool ok = m->status_code == ESP_ZB_ZCL_STATUS_SUCCESS;
      std::uint8_t ack[1];
      hubproto::encode_cmd_ack(ok ? hubproto::CmdAckStatus::Ok
                                  : hubproto::CmdAckStatus::Fail,
                               ack, sizeof(ack));
      publish_locked(make_header(hubproto::MsgType::CmdAck, p->entity_id,
                                 p->hubproto_seq),
                     ack, 1);
      ESP_LOGI(kTag, "default resp tsn=%u entity 0x%04x status=%d -> CmdAck %s",
               static_cast<unsigned>(tsn),
               static_cast<unsigned>(p->entity_id),
               static_cast<int>(m->status_code), ok ? "Ok" : "Fail");
      if (ok) {
        // Device confirmed the command applied: reflect it as STATE.
        ZbSwitch* sw = find_by_addr(src, m->info.src_endpoint);
        if (sw != nullptr) {
          sw->on_off = p->target_on;
          sw->on_off_valid = true;
          persist_switches();
          send_state_on_off(*sw);
        }
      }
      p->in_use = false;
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
            s->on_off_valid = true;
            persist_switches();
            send_state_on_off(*s);
          }
        } else if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT &&
                   v->attribute.data.value != nullptr) {
          ZbSwitch* s = find_by_addr(src, m->info.src_endpoint);
          if (s == nullptr) continue;
          const auto* raw = static_cast<const std::uint8_t*>(v->attribute.data.value);
          switch (v->attribute.id) {
            case ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_MULTIPLIER_ID:
              s->ac_power_mul = static_cast<std::uint16_t>(raw[0] | (raw[1] << 8));
              if (s->ac_power_mul == 0) s->ac_power_mul = 1;
              persist_switches();
              ESP_LOGI(kTag, "entity 0x%04x ACPowerMultiplier=%u",
                       static_cast<unsigned>(s->entity_id),
                       static_cast<unsigned>(s->ac_power_mul));
              break;
            case ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_DIVISOR_ID:
              s->ac_power_div = static_cast<std::uint16_t>(raw[0] | (raw[1] << 8));
              if (s->ac_power_div == 0) s->ac_power_div = 1;
              persist_switches();
              ESP_LOGI(kTag, "entity 0x%04x ACPowerDivisor=%u",
                       static_cast<unsigned>(s->entity_id),
                       static_cast<unsigned>(s->ac_power_div));
              break;
            case ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID: {
              const std::int16_t rawP =
                  static_cast<std::int16_t>(raw[0] | (raw[1] << 8));
              const std::uint16_t w = scale_active_power(*s, rawP);
              ESP_LOGI(kTag, "entity 0x%04x ActivePower raw=%d mul=%u div=%u -> %u W",
                       static_cast<unsigned>(s->entity_id), static_cast<int>(rawP),
                       static_cast<unsigned>(s->ac_power_mul),
                       static_cast<unsigned>(s->ac_power_div), static_cast<unsigned>(w));
              send_state_power_w(*s, w);
              break;
            }
            default:
              ESP_LOGW(kTag, "unhandled EM attr 0x%04x (data type 0x%02x)",
                       static_cast<unsigned>(v->attribute.id),
                       static_cast<unsigned>(v->attribute.data.type));
              break;
          }
        } else if (m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_METERING &&
                   v->attribute.data.value != nullptr) {
          ZbSwitch* s = find_by_addr(src, m->info.src_endpoint);
          if (s == nullptr) continue;
          const auto* raw = static_cast<const std::uint8_t*>(v->attribute.data.value);
          switch (v->attribute.id) {
            case ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID: {
              // ZCL spec: UINT24, 3 bytes LE (not UINT32)
              const std::uint32_t val =
                  static_cast<std::uint32_t>(raw[0]) |
                  (static_cast<std::uint32_t>(raw[1]) << 8) |
                  (static_cast<std::uint32_t>(raw[2]) << 16);
              s->metering_mul = (val == 0) ? 1u : val;
              persist_switches();
              ESP_LOGI(kTag, "entity 0x%04x Metering Multiplier=%lu",
                       static_cast<unsigned>(s->entity_id),
                       static_cast<unsigned long>(s->metering_mul));
              break;
            }
            case ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID: {
              // ZCL spec: UINT24, 3 bytes LE (not UINT32)
              const std::uint32_t val =
                  static_cast<std::uint32_t>(raw[0]) |
                  (static_cast<std::uint32_t>(raw[1]) << 8) |
                  (static_cast<std::uint32_t>(raw[2]) << 16);
              s->metering_div = (val == 0) ? 1u : val;
              persist_switches();
              ESP_LOGI(kTag, "entity 0x%04x Metering Divisor=%lu",
                       static_cast<unsigned>(s->entity_id),
                       static_cast<unsigned long>(s->metering_div));
              break;
            }
            case ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID: {
              std::uint64_t summation = 0;
              for (int b = 5; b >= 0; --b) summation = (summation << 8) | raw[b];
              const std::uint32_t wh = scale_summation(*s, summation);
              ESP_LOGI(kTag, "entity 0x%04x CurrentSummation raw=%llu -> %lu Wh",
                       static_cast<unsigned>(s->entity_id),
                       static_cast<unsigned long long>(summation),
                       static_cast<unsigned long>(wh));
              send_state_energy_wh(*s, wh);
              break;
            }
            default:
              break;
          }
        } else {
          // Any other read-resp variable (success but nothing to do) — log for
          // diagnostics so we can see if a device answered with a null value or
          // an unexpected type.
          ESP_LOGD(kTag, "read-resp cluster 0x%04x attr 0x%04x type 0x%02x value=%p",
                   static_cast<unsigned>(m->info.cluster),
                   static_cast<unsigned>(v->attribute.id),
                   static_cast<unsigned>(v->attribute.data.type),
                   v->attribute.data.value);
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

// Kick rearm + open joins after the network is up (shared by formation and reboot paths).
void network_ready() {
  open_network_for_join();
  bool have_restored = false;
  for (const auto& s : g_switches) {
    if (s.in_use) { have_restored = true; break; }
  }
  if (have_restored && !g_rearm_scheduled) {
    g_rearm_scheduled = true;
    esp_zb_scheduler_alarm(rearm_restored, 0, 3000);
  }
}

// esp-zigbee-sdk stack signals (formation, steering, device announce).
//
// Lifecycle (matches Espressif examples):
//   SKIP_STARTUP -> INITIALIZATION (load NVS network state)
//   -> DEVICE_FIRST_START (factory new)  -> FORMATION -> STEERING -> ready
//   -> DEVICE_REBOOT     (NVS present)  -> network_ready() (no re-formation)
//
// The old code called FORMATION on every boot, which created a new network
// (different key/PAN) each time — devices needed a factory reset to re-pair.
extern "C" void esp_zb_app_signal_handler(esp_zb_app_signal_t* signal_struct) {
  std::uint32_t* p_sg_p = signal_struct->p_app_signal;
  const esp_err_t err_status = signal_struct->esp_err_status;
  const esp_zb_app_signal_type_t sig = static_cast<esp_zb_app_signal_type_t>(*p_sg_p);

  switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
      ESP_LOGI(kTag, "stack initialized — loading stored network state");
      esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
      break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
      if (err_status == ESP_OK) {
        if (esp_zb_bdb_is_factory_new()) {
          ESP_LOGI(kTag, "factory-new — forming network");
          esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        } else {
          ESP_LOGI(kTag, "rebooted — network restored (PAN=0x%04x ch=%u)",
                   esp_zb_get_pan_id(), esp_zb_get_current_channel());
          network_ready();
        }
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
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
      } else {
        ESP_LOGE(kTag, "formation failed (status=%d), retrying",
                 static_cast<int>(err_status));
        esp_zb_scheduler_alarm(retry_formation, 0, 1000);
      }
      break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
      if (err_status == ESP_OK) {
        ESP_LOGI(kTag, "steering done — network open for joining");
        network_ready();
      } else {
        ESP_LOGW(kTag, "steering failed (status=%d)", static_cast<int>(err_status));
      }
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
  restore_switches();  // re-declare still-joined devices without a re-pair

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

  // Periodic heartbeat so `chips` on the brain sees us alive and, after a
  // brain reboot, its self-heal QUERY has something to trigger on.
  static esp_timer_handle_t hb_timer;
  const esp_timer_create_args_t hb_args{
      .callback = &heartbeat_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "hb",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&hb_args, &hb_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(hb_timer, 5000000));  // 5 s

  // Then start the Zigbee coordinator.
  esp_zb_platform_config_t config{};
  config.radio_config.radio_mode = ZB_RADIO_MODE_NATIVE;
  config.host_config.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE;
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));
  ESP_LOGI(kTag, "Phase 6 Stage 2b: Zigbee translator + bus");
  xTaskCreate(esp_zb_task, "zb_main", 4096, nullptr, 5, nullptr);
}
