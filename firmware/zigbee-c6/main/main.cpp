// The Zigbee -> DECLARE/STATE/COMMAND translator (SPEC §9, Phase 6 and later).
//
// An esp-zigbee-sdk coordinator wired onto the UART bus to the brain. It survives reboots: the
// Zigbee network (PAN, key, channel) is persisted by ZBOSS, and the entity table is persisted in
// NVS; on reboot the coordinator restores the same network so devices stay joined, then re-arms
// reporting and reads current state for each entity.
//
// Every application endpoint a joined device exposes becomes one entity whose type and attributes
// come from the clusters it advertises (core/zcl_profile.hpp, host-tested):
//
//   On/Off (+ metering)            -> switch      on_off, power_w, energy_wh
//   On/Off + Level / Color         -> light       on_off, brightness, color_temp, color_xy
//   Thermostat                     -> climate     temperature, setpoint, mode
//   Occupancy / IAS motion         -> occupancy   occupied
//   IAS contact                    -> contact     contact
//   Temperature/Humidity/Lux       -> sensor      temperature, humidity, illuminance
//   remote controls / buttons      -> button      event (a pulse: N, then idle)
//   + battery on anything that is not a plug or a lamp
//
//   - join    -> register entities, bind + configure reporting, send HELLO so the brain
//                re-runs QUERY -> DECLARE.
//   - QUERY   -> DECLARE every mapped entity + DECLARE_END, then re-read every device's state.
//   - reports / read responses -> STATE up to the brain (units: hubproto/attrs.hpp).
//   - COMMAND from the brain -> ZCL command or attribute write -> CMD_ACK.
//
// Two event sources publish onto the bus: the Zigbee stack task (HELLO/STATE) and the UART RX
// task (DECLARE/CMD_ACK). A mutex serializes publish; ZCL calls made from the UART task take
// esp_zb_lock (they must run under the stack lock).
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

// The ZBoss headers have no C++ guards of their own (raw_command_cb below uses their buffer API).
extern "C" {
#include "zboss_api.h"
#include "zcl/zb_zcl_common.h"
}
#include "esp_zigbee_core.h"

#include "esp_ota_sink.hpp"
#include "esp_uart_link.hpp"
#include "nvs_boot_id.hpp"
#include "core/ota_receiver.hpp"
#include "core/uart_transport.hpp"
#include "core/zcl_profile.hpp"
#include "hubproto/attrs.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/units.hpp"
#include "hubproto/version.hpp"

namespace zcl = core::zcl;
namespace A = hubproto::attr;

namespace {

const char* kTag = "zigbee-c6";

// ---- UART link to the brain ----------------------------------------------
constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x01;
std::uint8_t g_boot_id = 0;  // persisted reboot counter (SPEC Phase 5)

core::UartTransport* g_bus = nullptr;
SemaphoreHandle_t g_bus_mtx = nullptr;

// ---- Zigbee constants -----------------------------------------------------
constexpr std::uint8_t kCoordEndpoint = 1;
constexpr std::uint8_t kJoinWindowSec = 180;
constexpr std::uint32_t kPrimaryChannelMask = ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK;
constexpr std::uint16_t kBasicCluster = 0x0000;
constexpr std::uint16_t kAttrManufacturerName = 0x0004;
constexpr std::uint16_t kAttrModelIdentifier = 0x0005;

// ZCL attribute ids the translator uses (cluster-scoped).
constexpr std::uint16_t kAttrPresentValue = 0x0055;       // multistate input
constexpr std::uint16_t kAttrBatteryPercent = 0x0021;     // power configuration, half-percent
constexpr std::uint16_t kAttrColorX = 0x0003;
constexpr std::uint16_t kAttrColorY = 0x0004;
constexpr std::uint16_t kAttrColorTempMireds = 0x0007;
constexpr std::uint16_t kAttrColorCapabilities = 0x400A;
constexpr std::uint16_t kAttrLocalTemperature = 0x0000;   // thermostat
constexpr std::uint16_t kAttrHeatingSetpoint = 0x0012;    // thermostat, occupied heating, 0.01 °C
constexpr std::uint16_t kAttrSystemMode = 0x001C;
constexpr std::uint16_t kAttrZoneType = 0x0001;           // IAS zone
constexpr std::uint16_t kAttrZoneStatus = 0x0002;
constexpr std::uint16_t kAttrCieAddress = 0x0010;
constexpr std::uint8_t kLevelTransitionDs = 5;            // 0.5 s fades for brightness/colour

// ---- Entity mapping: Zigbee endpoint <-> brain entity ---------------------
constexpr std::size_t kMaxEntities = 32;
constexpr std::size_t kMaxCache = 8;
constexpr std::uint16_t kFirstEntityId = 0x0100;

// What was last published per attribute: STATE is only sent on a change (except on_off and
// button events, which always go out). Not persisted — after a chip reboot the first reading of
// every attribute is published, which is what lets the brain's reconciler see a device that came
// back in a different state.
struct Cached {
  std::uint8_t attr;
  bool valid;
  hubproto::Value v;
};

struct ZbEntity {
  bool in_use;
  hubproto::DeviceType type;
  std::uint8_t endpoint;           // device application endpoint
  std::uint16_t entity_id;         // brain-facing id
  std::uint16_t short_addr;        // Zigbee NWK address (changes on re-pair)
  esp_zb_ieee_addr_t ieee;         // stable 64-bit address (identity, with the endpoint)
  std::uint32_t caps;              // zcl::Cap bits: which attributes this entity carries
  std::uint16_t ac_power_mul;      // ActivePower multiplier (default 1)
  std::uint16_t ac_power_div;      // ActivePower divisor    (default 1)
  std::uint32_t metering_mul;      // Metering Multiplier (default 1)
  std::uint32_t metering_div;      // Metering Divisor    (default 1)
  // ---- runtime only ----
  Cached cache[kMaxCache];
  std::uint16_t color_x, color_y;
  bool have_x, have_y;
};

ZbEntity g_ent[kMaxEntities] = {};
std::uint16_t g_next_entity_id = kFirstEntityId;

// IEEE address of the device currently being discovered (announce -> simple desc).
esp_zb_ieee_addr_t g_pending_ieee = {};

// Set once we've re-armed reporting for NVS-restored devices after a reboot.
bool g_rearm_scheduled = false;
// Set once the Zigbee stack has initialized. The scheduler must not be touched
// before: a QUERY from the brain can arrive within milliseconds of our HELLO.
volatile bool g_zb_stack_up = false;
fw::OtaConfirmGate g_ota_confirm;

// IAS Zone endpoints wait here for their ZoneType (motion or contact?) before becoming entities.
struct IasPending {
  bool used;
  std::uint16_t short_addr;
  std::uint8_t endpoint;
  esp_zb_ieee_addr_t ieee;
  zcl::Profile profile;
};
IasPending g_ias_pending[4] = {};

// Pending brain COMMANDs awaiting a device answer: the ZCL Default Response (cluster commands)
// or the Write Attributes Response (attribute writes), matched by ZCL tsn. Only on success do we
// send CmdAck Ok + STATE back to the brain. Failures become CmdAck Fail; no response at all ->
// the brain's CommandDispatcher times out to Unreachable (and the slot is reclaimed after 10 s).
struct PendingCmd {
  bool in_use;
  std::uint8_t tsn;
  std::uint16_t src_short;       // device we sent to (for extra safety)
  std::uint16_t entity_id;
  std::uint16_t hubproto_seq;    // brain's seq, to echo in CmdAck
  std::uint8_t attr;
  hubproto::Value target;        // the value the device was asked for
  std::uint32_t t_ms;
  // Storage for an attribute write; must outlive the request.
  esp_zb_zcl_attribute_t wattr;
  std::uint8_t wbuf[2];
};
PendingCmd g_pending[8] = {};
constexpr std::uint32_t kPendingTtlMs = 10000;

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

PendingCmd* find_pending(std::uint8_t tsn, std::uint16_t src_short) {
  for (auto& p : g_pending) {
    if (p.in_use && p.tsn == tsn && p.src_short == src_short) return &p;
  }
  return nullptr;
}

PendingCmd* alloc_pending() {
  const std::uint32_t now = now_ms();
  for (auto& p : g_pending) {
    if (p.in_use && now - p.t_ms > kPendingTtlMs) p.in_use = false;  // the device never answered
  }
  for (auto& p : g_pending) {
    if (!p.in_use) return &p;
  }
  return nullptr;
}

// ---- NVS persistence --------------------------------------------------------
// The entity table survives a C6 reflash/reboot, so a device that is still joined (its bindings +
// reporting config persist in the Zigbee stack) is re-declared to the brain without a
// factory-reset/re-pair.
const char* kNvsNamespace = "keystone";
const char* kNvsEntities = "zb_ent";  // current layout (Rec[])
const char* kNvsLegacy = "zb_sw";     // before multi-type support: the old ZbSwitch[] blob
const char* kNvsNextId = "zb_next";

// Persisted fields only (32 bytes).
struct Rec {
  std::uint8_t in_use;
  std::uint8_t type;
  std::uint8_t endpoint;
  std::uint8_t pad;
  std::uint16_t entity_id;
  std::uint16_t short_addr;
  std::uint8_t ieee[8];
  std::uint32_t caps;
  std::uint16_t ac_power_mul;
  std::uint16_t ac_power_div;
  std::uint32_t metering_mul;
  std::uint32_t metering_div;
};
static_assert(sizeof(Rec) == 32, "persisted entity record layout");

// The pre-0.x layout, kept only to migrate a hub that already has a plug paired.
struct LegacySwitch {
  bool in_use;
  std::uint16_t entity_id;
  std::uint16_t short_addr;
  esp_zb_ieee_addr_t ieee;
  std::uint8_t endpoint;
  bool on_off;
  bool on_off_valid;
  bool has_metering;
  std::uint16_t ac_power_mul;
  std::uint16_t ac_power_div;
  std::uint16_t last_power_w;
  bool has_energy_meter;
  std::uint32_t metering_mul;
  std::uint32_t metering_div;
  std::uint32_t last_energy_wh;
};

void persist_entities() {
  static Rec recs[kMaxEntities];
  for (std::size_t i = 0; i < kMaxEntities; ++i) {
    const ZbEntity& e = g_ent[i];
    Rec& r = recs[i];
    std::memset(&r, 0, sizeof(r));
    r.in_use = e.in_use ? 1 : 0;
    r.type = static_cast<std::uint8_t>(e.type);
    r.endpoint = e.endpoint;
    r.entity_id = e.entity_id;
    r.short_addr = e.short_addr;
    std::memcpy(r.ieee, e.ieee, sizeof(r.ieee));
    r.caps = e.caps;
    r.ac_power_mul = e.ac_power_mul;
    r.ac_power_div = e.ac_power_div;
    r.metering_mul = e.metering_mul;
    r.metering_div = e.metering_div;
  }
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, kNvsEntities, recs, sizeof(recs));
  nvs_set_u16(h, kNvsNextId, g_next_entity_id);
  nvs_commit(h);
  nvs_close(h);
}

void sanitize_entity(ZbEntity& e) {
  if (e.ac_power_div == 0) e.ac_power_div = 1;
  if (e.ac_power_mul == 0) e.ac_power_mul = 1;
  if (e.metering_div == 0) e.metering_div = 1;
  if (e.metering_mul == 0) e.metering_mul = 1;
}

void restore_entities() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  nvs_get_u16(h, kNvsNextId, &g_next_entity_id);

  static Rec recs[kMaxEntities];
  std::size_t sz = sizeof(recs);
  bool migrated = false;
  if (nvs_get_blob(h, kNvsEntities, recs, &sz) == ESP_OK && sz == sizeof(recs)) {
    for (std::size_t i = 0; i < kMaxEntities; ++i) {
      const Rec& r = recs[i];
      ZbEntity& e = g_ent[i];
      std::memset(&e, 0, sizeof(e));
      e.in_use = r.in_use != 0;
      e.type = static_cast<hubproto::DeviceType>(r.type);
      e.endpoint = r.endpoint;
      e.entity_id = r.entity_id;
      e.short_addr = r.short_addr;
      std::memcpy(e.ieee, r.ieee, sizeof(e.ieee));
      e.caps = r.caps;
      e.ac_power_mul = r.ac_power_mul;
      e.ac_power_div = r.ac_power_div;
      e.metering_mul = r.metering_mul;
      e.metering_div = r.metering_div;
    }
  } else {
    // First boot of this firmware on a hub paired by an older one: every old entity was an
    // On/Off switch. Keep its entity_id (rules, names, Matter endpoints hang off it).
    static LegacySwitch old[kMaxEntities];
    std::size_t osz = sizeof(old);
    if (nvs_get_blob(h, kNvsLegacy, old, &osz) == ESP_OK && osz == sizeof(old)) {
      for (std::size_t i = 0; i < kMaxEntities; ++i) {
        const LegacySwitch& o = old[i];
        ZbEntity& e = g_ent[i];
        std::memset(&e, 0, sizeof(e));
        if (!o.in_use) continue;
        e.in_use = true;
        e.type = hubproto::DeviceType::Switch;
        e.endpoint = o.endpoint;
        e.entity_id = o.entity_id;
        e.short_addr = o.short_addr;
        std::memcpy(e.ieee, o.ieee, sizeof(e.ieee));
        e.caps = static_cast<std::uint32_t>(zcl::kCapOnOff) |
                 (o.has_metering ? static_cast<std::uint32_t>(zcl::kCapPower) : 0u) |
                 (o.has_energy_meter ? static_cast<std::uint32_t>(zcl::kCapEnergy) : 0u);
        e.ac_power_mul = o.ac_power_mul;
        e.ac_power_div = o.ac_power_div;
        e.metering_mul = o.metering_mul;
        e.metering_div = o.metering_div;
        migrated = true;
      }
    }
  }
  nvs_close(h);

  std::size_t n = 0;
  for (auto& e : g_ent) {
    if (!e.in_use) continue;
    sanitize_entity(e);
    ++n;
  }
  if (migrated) persist_entities();
  if (n > 0) {
    ESP_LOGI(kTag, "restored %u entity(ies) from NVS%s", static_cast<unsigned>(n),
             migrated ? " (migrated from the switch-only layout)" : "");
  }
}

ZbEntity* find_by_entity(std::uint16_t entity_id) {
  for (auto& e : g_ent) {
    if (e.in_use && e.entity_id == entity_id) return &e;
  }
  return nullptr;
}

ZbEntity* find_by_addr(std::uint16_t short_addr, std::uint8_t endpoint) {
  for (auto& e : g_ent) {
    if (e.in_use && e.short_addr == short_addr && e.endpoint == endpoint) return &e;
  }
  return nullptr;
}

ZbEntity* find_by_ieee_ep(const esp_zb_ieee_addr_t ieee, std::uint8_t endpoint) {
  for (auto& e : g_ent) {
    if (e.in_use && e.endpoint == endpoint &&
        std::memcmp(e.ieee, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) {
      return &e;
    }
  }
  return nullptr;
}

constexpr std::uint32_t kColorBits = zcl::kCapColorTemp | zcl::kCapColorXy;

// Identity is the stable IEEE address plus the endpoint: a re-paired device keeps its entity_id
// and just updates its (volatile) short address.
ZbEntity* register_entity(std::uint16_t short_addr, const esp_zb_ieee_addr_t ieee,
                          std::uint8_t endpoint, hubproto::DeviceType type,
                          std::uint32_t caps, bool color_pending) {
  if (ZbEntity* e = find_by_ieee_ep(ieee, endpoint)) {
    e->short_addr = short_addr;
    // A lamp's refined colour capabilities survive a re-announce (they are re-read right after).
    if (color_pending && e->type == type) caps = (caps & ~kColorBits) | (e->caps & kColorBits);
    e->type = type;
    e->caps = caps;
    persist_entities();
    return e;
  }
  for (auto& e : g_ent) {
    if (e.in_use) continue;
    std::memset(&e, 0, sizeof(e));
    e.in_use = true;
    e.type = type;
    e.entity_id = g_next_entity_id++;
    e.short_addr = short_addr;
    std::memcpy(e.ieee, ieee, sizeof(esp_zb_ieee_addr_t));
    e.endpoint = endpoint;
    e.caps = caps;
    e.ac_power_mul = e.ac_power_div = 1;
    e.metering_mul = e.metering_div = 1;
    persist_entities();
    return &e;
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

void heartbeat_cb(void* /*arg*/) {
  const std::uint32_t now_s = static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
  hubproto::ChipSelf self{};
  self.uptime_s = now_s;
  self.heap_free = esp_get_free_heap_size();
  self.boot_id = g_boot_id;
  if (g_bus != nullptr) {
    self.rx_frames = static_cast<std::uint32_t>(g_bus->rx_frames());
    self.tx_frames = static_cast<std::uint32_t>(g_bus->tx_frames());
    self.crc_errors = static_cast<std::uint16_t>(g_bus->bad_frames());
  }

  std::uint8_t buf[64];
  auto r = hubproto::encode_chip_self_state(kOwnChipId, self, buf, sizeof(buf));
  if (r.ok) {
    publish_locked(make_header(hubproto::MsgType::State, kOwnChipId, 0),
                   buf, r.len);
  }
  publish_locked(make_header(hubproto::MsgType::Heartbeat, 0x0000, 0), nullptr, 0);
}

void send_hello() {
  publish_locked(make_header(hubproto::MsgType::Hello, 0x0000, 0), nullptr, 0);
  ESP_LOGI(kTag, "sent HELLO (topology changed -> brain will re-QUERY)");
}

void send_declarations() {
  std::uint8_t sbuf[64];
  auto sr = hubproto::encode_chip_self_declare(kOwnChipId, sbuf, sizeof(sbuf));
  if (sr.ok) {
    publish_locked(make_header(hubproto::MsgType::Declare, kOwnChipId, 0),
                   sbuf, sr.len);
  }

  std::size_t declared = 0;
  for (auto& e : g_ent) {
    if (!e.in_use) continue;
    hubproto::AttrDecl attrs[kMaxCache];
    const std::size_t attr_count = zcl::attrs_for(e.caps, attrs, kMaxCache);
    if (attr_count == 0) continue;
    std::uint8_t buf[4 + 3 * kMaxCache];
    auto r = hubproto::encode_declare(e.entity_id, e.type, attrs,
                                      static_cast<std::uint8_t>(attr_count), buf, sizeof(buf));
    if (r.ok) {
      publish_locked(make_header(hubproto::MsgType::Declare, e.entity_id, 0), buf, r.len);
      ++declared;
    }
  }
  publish_locked(make_header(hubproto::MsgType::DeclareEnd, 0x0000, 0), nullptr, 0);
  ESP_LOGI(kTag, "declared %u entity(ies) + DECLARE_END", static_cast<unsigned>(declared));
}

const char* attr_label(std::uint8_t id) {
  switch (id) {
    case A::kOnOff: return "on_off";
    case A::kBrightness: return "brightness";
    case A::kColorXy: return "color_xy";
    case A::kColorTemp: return "color_temp";
    case A::kTemperature: return "temperature";
    case A::kHumidity: return "humidity";
    case A::kIlluminance: return "illuminance";
    case A::kPowerW: return "power_w";
    case A::kEnergyWh: return "energy_wh";
    case A::kBattery: return "battery";
    case A::kSetpoint: return "setpoint";
    case A::kMode: return "mode";
    case A::kOccupied: return "occupied";
    case A::kContact: return "contact";
    case A::kEvent: return "event";
    default: return "attr";
  }
}

long value_for_log(const hubproto::Value& v) {
  switch (v.type) {
    case hubproto::ValueType::Bool: return v.as_bool() ? 1 : 0;
    case hubproto::ValueType::U8: return v.as_u8();
    case hubproto::ValueType::Enum8: return v.as_enum8();
    case hubproto::ValueType::I16: return v.as_i16();
    case hubproto::ValueType::U16: return v.as_u16();
    case hubproto::ValueType::U32: return static_cast<long>(v.as_u32());
    default: return 0;
  }
}

// Publish STATE for one attribute. Unless `force`, an unchanged value is not sent again.
void publish_attr(ZbEntity& e, std::uint8_t attr, const hubproto::Value& v, bool force = false) {
  Cached* slot = nullptr;
  for (auto& c : e.cache) {
    if (c.valid && c.attr == attr) {
      slot = &c;
      break;
    }
  }
  if (slot == nullptr) {
    for (auto& c : e.cache) {
      if (!c.valid) {
        slot = &c;
        break;
      }
    }
  }
  if (slot != nullptr) {
    if (!force && slot->valid && slot->attr == attr && slot->v == v) return;
    slot->valid = true;
    slot->attr = attr;
    slot->v = v;
  }
  const hubproto::StateEntry entry[1] = {{attr, v}};
  std::uint8_t buf[16];
  auto r = hubproto::encode_state(e.entity_id, entry, 1, buf, sizeof(buf));
  if (!r.ok) return;
  publish_locked(make_header(hubproto::MsgType::State, e.entity_id, 0), buf, r.len);
  if (v.type == hubproto::ValueType::XyColor) {
    std::uint16_t x = 0, y = 0;
    v.as_xy(x, y);
    ESP_LOGI(kTag, "STATE entity 0x%04x %s=(%u,%u)", static_cast<unsigned>(e.entity_id),
             attr_label(attr), static_cast<unsigned>(x), static_cast<unsigned>(y));
  } else {
    ESP_LOGI(kTag, "STATE entity 0x%04x %s=%ld", static_cast<unsigned>(e.entity_id),
             attr_label(attr), value_for_log(v));
  }
}

// ---- scaling for the metering clusters --------------------------------------
// Scale raw CurrentSummationDelivered (uint48, lower 32 bits) by Metering
// Multiplier/Divisor -> Wh. UnitOfMeasure=0 means kWh per ZCL spec, but IKEA uses divisor=1 and
// the raw value already in Wh. We don't read UnitOfMeasure; just apply raw * mul / div.
std::uint32_t scale_summation(const ZbEntity& s, std::uint64_t raw) {
  if (s.metering_div == 0) return 0;
  const std::uint64_t scaled =
      raw * static_cast<std::uint64_t>(s.metering_mul) /
      static_cast<std::uint64_t>(s.metering_div);
  return scaled > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(scaled);
}

// Apply the cached ACPower multiplier/divisor to a raw ActivePower reading
// (spec: `real_power = raw * multiplier / divisor` in Watts). Clamps to u16.
std::uint16_t scale_active_power(const ZbEntity& s, std::int16_t raw) {
  if (s.ac_power_div == 0) return 0;
  const std::int32_t scaled =
      (static_cast<std::int32_t>(raw) * static_cast<std::int32_t>(s.ac_power_mul)) /
      static_cast<std::int32_t>(s.ac_power_div);
  if (scaled < 0) return 0;
  if (scaled > 0xFFFF) return 0xFFFF;
  return static_cast<std::uint16_t>(scaled);
}

std::uint16_t le16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// ---- COMMAND (brain -> device) : runs on the UART RX task -----------------
void send_ack(const hubproto::Header& h, std::uint16_t entity_id, hubproto::CmdAckStatus st) {
  std::uint8_t ack[1];
  hubproto::encode_cmd_ack(st, ack, sizeof(ack));
  publish_locked(make_header(hubproto::MsgType::CmdAck, entity_id, h.seq), ack, 1);
}

bool entity_has_attr(const ZbEntity& e, std::uint8_t attr) {
  hubproto::AttrDecl a[kMaxCache];
  const std::size_t n = zcl::attrs_for(e.caps, a, kMaxCache);
  for (std::size_t i = 0; i < n; ++i) {
    if (a[i].attr_id == attr && (a[i].flags & hubproto::kFlagWritable) != 0) return true;
  }
  return false;
}

void handle_command(const hubproto::Header& h, const std::uint8_t* payload,
                    std::size_t len) {
  if (len < 3) return;
  const std::uint16_t entity_id = le16(payload);
  const std::uint8_t attr = payload[2];

  ZbEntity* s = find_by_entity(entity_id);
  if (s == nullptr) {
    ESP_LOGW(kTag, "COMMAND for unknown entity 0x%04x", static_cast<unsigned>(entity_id));
    send_ack(h, entity_id, hubproto::CmdAckStatus::Unreachable);
    return;
  }
  hubproto::ValueType vt{};
  if (!zcl::attr_value_type(attr, vt) || !entity_has_attr(*s, attr)) {
    ESP_LOGW(kTag, "COMMAND entity 0x%04x: attr 0x%02x not writable here",
             static_cast<unsigned>(entity_id), static_cast<unsigned>(attr));
    send_ack(h, entity_id, hubproto::CmdAckStatus::Unsupported);
    return;
  }
  hubproto::Command cmd{};
  if (!hubproto::decode_command(payload, len, vt, cmd)) {
    send_ack(h, entity_id, hubproto::CmdAckStatus::Fail);
    return;
  }

  PendingCmd* p = alloc_pending();
  if (p == nullptr) {
    ESP_LOGW(kTag, "pending cmd table full; dropping command");
    send_ack(h, entity_id, hubproto::CmdAckStatus::Fail);
    return;
  }
  std::memset(p, 0, sizeof(*p));

  // Range checks: a value the device would reject never leaves the chip.
  if (attr == A::kBrightness && !hubproto::units::level_valid(cmd.value.as_u8())) {
    send_ack(h, entity_id, hubproto::CmdAckStatus::Fail);
    return;
  }
  if (attr == A::kMode && !hubproto::units::mode_valid(cmd.value.as_enum8())) {
    send_ack(h, entity_id, hubproto::CmdAckStatus::Fail);
    return;
  }

  const std::uint16_t dst = s->short_addr;
  const std::uint8_t ep = s->endpoint;
  std::uint8_t tsn = 0;
  bool sent = true;

  // ZCL calls must run under the stack lock (we are on the UART task here). Reply to the brain
  // only after the device's response arrives — no optimistic Ok.
  if (!esp_zb_lock_acquire(portMAX_DELAY)) {
    send_ack(h, entity_id, hubproto::CmdAckStatus::Fail);
    return;
  }
  switch (attr) {
    case A::kOnOff: {
      esp_zb_zcl_on_off_cmd_t req{};
      req.zcl_basic_cmd.dst_addr_u.addr_short = dst;
      req.zcl_basic_cmd.dst_endpoint = ep;
      req.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
      req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
      req.on_off_cmd_id = cmd.value.as_bool() ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
                                               : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
      tsn = esp_zb_zcl_on_off_cmd_req(&req);
      break;
    }
    case A::kBrightness: {
      esp_zb_zcl_move_to_level_cmd_t req{};
      req.zcl_basic_cmd.dst_addr_u.addr_short = dst;
      req.zcl_basic_cmd.dst_endpoint = ep;
      req.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
      req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
      req.level = cmd.value.as_u8();
      req.transition_time = kLevelTransitionDs;
      tsn = esp_zb_zcl_level_move_to_level_cmd_req(&req);
      break;
    }
    case A::kColorTemp: {
      esp_zb_zcl_color_move_to_color_temperature_cmd_t req{};
      req.zcl_basic_cmd.dst_addr_u.addr_short = dst;
      req.zcl_basic_cmd.dst_endpoint = ep;
      req.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
      req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
      req.color_temperature = cmd.value.as_u16();
      req.transition_time = kLevelTransitionDs;
      tsn = esp_zb_zcl_color_move_to_color_temperature_cmd_req(&req);
      break;
    }
    case A::kColorXy: {
      std::uint16_t x = 0, y = 0;
      cmd.value.as_xy(x, y);
      esp_zb_zcl_color_move_to_color_cmd_t req{};
      req.zcl_basic_cmd.dst_addr_u.addr_short = dst;
      req.zcl_basic_cmd.dst_endpoint = ep;
      req.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
      req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
      req.color_x = x;
      req.color_y = y;
      req.transition_time = kLevelTransitionDs;
      tsn = esp_zb_zcl_color_move_to_color_cmd_req(&req);
      break;
    }
    case A::kSetpoint:
    case A::kMode: {
      // Thermostat attributes are written (the answer is a Write Attributes Response).
      p->wattr.id = (attr == A::kSetpoint) ? kAttrHeatingSetpoint : kAttrSystemMode;
      if (attr == A::kSetpoint) {
        const std::int16_t centi = hubproto::units::temp_to_centi(cmd.value.as_i16());
        p->wbuf[0] = static_cast<std::uint8_t>(centi & 0xFF);
        p->wbuf[1] = static_cast<std::uint8_t>((centi >> 8) & 0xFF);
        p->wattr.data.type = ESP_ZB_ZCL_ATTR_TYPE_S16;
        p->wattr.data.size = 2;
      } else {
        p->wbuf[0] = cmd.value.as_enum8();
        p->wattr.data.type = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM;
        p->wattr.data.size = 1;
      }
      p->wattr.data.value = p->wbuf;
      esp_zb_zcl_write_attr_cmd_t req{};
      req.zcl_basic_cmd.dst_addr_u.addr_short = dst;
      req.zcl_basic_cmd.dst_endpoint = ep;
      req.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
      req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
      req.clusterID = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT;
      req.attr_number = 1;
      req.attr_field = &p->wattr;
      tsn = esp_zb_zcl_write_attr_cmd_req(&req);
      break;
    }
    default:
      sent = false;
      break;
  }
  esp_zb_lock_release();
  if (!sent) {
    send_ack(h, entity_id, hubproto::CmdAckStatus::Unsupported);
    return;
  }

  p->in_use = true;
  p->tsn = tsn;
  p->src_short = dst;
  p->entity_id = entity_id;
  p->hubproto_seq = h.seq;
  p->attr = attr;
  p->target = cmd.value;
  p->t_ms = now_ms();
  ESP_LOGI(kTag, "COMMAND entity 0x%04x %s=%ld -> device 0x%04x ep%u tsn=%u",
           static_cast<unsigned>(entity_id), attr_label(attr), value_for_log(cmd.value),
           static_cast<unsigned>(dst), ep, static_cast<unsigned>(tsn));
}

// The device answered a tracked command: CmdAck to the brain, and on success reflect the value.
void finish_pending(PendingCmd* p, std::uint16_t src, std::uint8_t src_ep, bool ok,
                    int status_code) {
  std::uint8_t ack[1];
  hubproto::encode_cmd_ack(ok ? hubproto::CmdAckStatus::Ok : hubproto::CmdAckStatus::Fail, ack,
                           sizeof(ack));
  publish_locked(make_header(hubproto::MsgType::CmdAck, p->entity_id, p->hubproto_seq), ack, 1);
  ESP_LOGI(kTag, "response tsn=%u entity 0x%04x status=%d -> CmdAck %s",
           static_cast<unsigned>(p->tsn), static_cast<unsigned>(p->entity_id), status_code,
           ok ? "Ok" : "Fail");
  if (ok) {
    // Device confirmed the command applied: reflect it as STATE.
    ZbEntity* e = find_by_addr(src, src_ep);
    if (e == nullptr) e = find_by_entity(p->entity_id);
    if (e != nullptr) publish_attr(*e, p->attr, p->target, /*force=*/true);
  }
  p->in_use = false;
}

void read_device_state(ZbEntity& e, bool color_caps);

// esp_zb_scheduler_alarm callback: re-read current state from every mapped
// device (ZCL calls must run under the stack lock; scheduler runs in stack
// context, so we don't need to acquire it explicitly).
void refresh_states_cb(std::uint8_t /*unused*/) {
  for (auto& e : g_ent) {
    if (!e.in_use) continue;
    // The brain has just (re)declared us and knows no values. Unchanged values are normally not
    // sent again, so say what we last heard first (a sleepy sensor may not answer a read at all),
    // then ask for fresh ones.
    for (auto& c : e.cache) {
      if (c.valid) publish_attr(e, c.attr, c.v, true);
    }
    read_device_state(e, false);
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
      ZbEntity* s = find_by_entity(arg);
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
      // A device with several endpoints is several entities: the whole device leaves.
      std::size_t dropped = 0;
      for (auto& other : g_ent) {
        if (other.in_use && std::memcmp(other.ieee, leave.device_address,
                                        sizeof(esp_zb_ieee_addr_t)) == 0) {
          other.in_use = false;
          ++dropped;
          // Say so explicitly: the brain mirrors this to the Matter chip, which removes the
          // bridged endpoint (a device with several endpoints must not leave ghosts behind).
          publish_locked(make_header(hubproto::MsgType::Undeclare, other.entity_id, 0), nullptr, 0);
        }
      }
      persist_entities();
      ESP_LOGI(kTag, "meta: dropped %u entity(ies) of that device", static_cast<unsigned>(dropped));
      // The brain only forgets an entity on a fresh HELLO (it clears the chip and re-queries),
      // so without this an unpaired device would linger in the registry and the app.
      send_hello();
      break;
    }
    case kMetaIdentify: {
      ZbEntity* s = find_by_entity(arg);
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
      g_ota_confirm.note_link_ok();  // the brain is talking to us
      send_declarations();
      // Also re-read current state from each device so the brain gets a fresh
      // STATE for every entity after (re-)declaration, not just at first join.
      // Before the stack is up there is nothing to read (and no scheduler yet);
      // rearm_restored re-reads everything once the network is ready.
      if (g_zb_stack_up && esp_zb_lock_acquire(portMAX_DELAY)) {
        esp_zb_scheduler_alarm(refresh_states_cb, 0, 100);
        esp_zb_lock_release();
      }
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
// Bind a cluster from the device to the coordinator so unicast reports (or, for a remote's
// output clusters, its commands) arrive at our endpoint. Fire-and-forget; the ZDO answer isn't
// tracked.
void bind_cluster(const ZbEntity& s, std::uint16_t cluster_id) {
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
// `delta` is the reportable change (nullptr = "any change"; analog types need a real value).
void configure_report(const ZbEntity& s, std::uint16_t cluster_id,
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

// Reportable-change thresholds. The SDK needs a non-null pointer of the attribute's type for
// analog attributes (passing nullptr crashes: memcpy from 0).
std::int16_t kDeltaS16_1 = 1;          // power, 1 W
std::int16_t kDeltaS16_10 = 10;        // thermostat: 0.1 °C
std::int16_t kDeltaS16_20 = 20;        // temperature: 0.2 °C
std::uint16_t kDeltaU16_1 = 1;         // colour temperature, 1 mired
std::uint16_t kDeltaU16_100 = 100;     // humidity 1 %, colour xy ~0.15 %
std::uint16_t kDeltaU16_500 = 500;     // illuminance: ~12 % on the log scale
std::uint8_t kDeltaU8_1 = 1;           // brightness
std::uint8_t kDeltaU8_2 = 2;           // battery: 1 %
std::uint8_t kDeltaU48_1[6] = {1, 0, 0, 0, 0, 0};  // energy: 1 Wh, UINT48 LE

constexpr std::uint8_t kTypeBitmap8 = ESP_ZB_ZCL_ATTR_TYPE_8BITMAP;
constexpr std::uint8_t kTypeEnum8 = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM;

struct ReportRow {
  std::uint32_t cap;      // applies when the entity has this capability ...
  std::uint32_t not_cap;  // ... and none of these
  std::uint16_t cluster;
  std::uint16_t attr;
  std::uint8_t type;
  std::uint16_t min_s, max_s;
  void* delta;
};

ReportRow kReports[] = {
    {zcl::kCapOnOff, 0, zcl::cluster::kOnOff, 0x0000, ESP_ZB_ZCL_ATTR_TYPE_BOOL, 0, 300, nullptr},
    {zcl::kCapLevel, 0, zcl::cluster::kLevel, 0x0000, ESP_ZB_ZCL_ATTR_TYPE_U8, 1, 300, &kDeltaU8_1},
    {zcl::kCapColorTemp, 0, zcl::cluster::kColor, kAttrColorTempMireds, ESP_ZB_ZCL_ATTR_TYPE_U16, 1,
     300, &kDeltaU16_1},
    {zcl::kCapColorXy, 0, zcl::cluster::kColor, kAttrColorX, ESP_ZB_ZCL_ATTR_TYPE_U16, 1, 300,
     &kDeltaU16_100},
    {zcl::kCapColorXy, 0, zcl::cluster::kColor, kAttrColorY, ESP_ZB_ZCL_ATTR_TYPE_U16, 1, 300,
     &kDeltaU16_100},
    // Temperature comes from the thermostat's LocalTemperature when there is one, else from the
    // Temperature Measurement cluster.
    {zcl::kCapTemperature, zcl::kCapThermostat, zcl::cluster::kTemperature, 0x0000,
     ESP_ZB_ZCL_ATTR_TYPE_S16, 30, 3600, &kDeltaS16_20},
    {zcl::kCapThermostat, 0, zcl::cluster::kThermostat, kAttrLocalTemperature,
     ESP_ZB_ZCL_ATTR_TYPE_S16, 10, 600, &kDeltaS16_10},
    {zcl::kCapThermostat, 0, zcl::cluster::kThermostat, kAttrHeatingSetpoint,
     ESP_ZB_ZCL_ATTR_TYPE_S16, 1, 3600, &kDeltaS16_10},
    {zcl::kCapThermostat, 0, zcl::cluster::kThermostat, kAttrSystemMode, kTypeEnum8, 0, 3600,
     nullptr},
    {zcl::kCapHumidity, 0, zcl::cluster::kHumidity, 0x0000, ESP_ZB_ZCL_ATTR_TYPE_U16, 30, 3600,
     &kDeltaU16_100},
    {zcl::kCapIlluminance, 0, zcl::cluster::kIlluminance, 0x0000, ESP_ZB_ZCL_ATTR_TYPE_U16, 10,
     3600, &kDeltaU16_500},
    {zcl::kCapBattery, 0, zcl::cluster::kPowerCfg, kAttrBatteryPercent, ESP_ZB_ZCL_ATTR_TYPE_U8,
     3600, 43200, &kDeltaU8_2},
    {zcl::kCapOccupancy, zcl::kCapIas, zcl::cluster::kOccupancy, 0x0000, kTypeBitmap8, 0, 3600,
     nullptr},
    // ActivePower: report if it moved by >=1 W, at least every 30 s and no more than once per
    // 5 s (min_interval throttles the device).
    {zcl::kCapPower, 0, zcl::cluster::kElecMeas,
     ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, 5, 30,
     &kDeltaS16_1},
    // CurrentSummationDelivered: UINT48 (6 bytes LE); report on any 1 Wh change, at most every 5 s.
    {zcl::kCapEnergy, 0, zcl::cluster::kMetering,
     ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, ESP_ZB_ZCL_ATTR_TYPE_U48, 5, 3600,
     kDeltaU48_1},
};

// Bind + configure reporting for every cluster the entity uses that we translate. Called at
// fresh join and again at boot for restored devices.
void setup_reporting(const ZbEntity& s) {
  std::uint16_t bound[16];
  std::size_t nbound = 0;
  auto bind_once = [&](std::uint16_t cl) {
    for (std::size_t i = 0; i < nbound; ++i) {
      if (bound[i] == cl) return;
    }
    if (nbound < sizeof(bound) / sizeof(bound[0])) bound[nbound++] = cl;
    bind_cluster(s, cl);
  };
  std::size_t configured = 0;
  for (const auto& r : kReports) {
    if ((s.caps & r.cap) == 0 || (s.caps & r.not_cap) != 0) continue;
    bind_once(r.cluster);
    configure_report(s, r.cluster, r.attr, r.type, r.min_s, r.max_s, r.delta);
    ++configured;
  }
  // A remote's commands arrive over a binding of its *output* clusters to the coordinator.
  if ((s.caps & zcl::kCapButton) != 0) {
    bind_once(zcl::cluster::kOnOff);
    bind_once(zcl::cluster::kLevel);
    bind_once(zcl::cluster::kScenes);
  }
  ESP_LOGI(kTag, "bound + configured %u report(s) for entity 0x%04x (caps 0x%04lx)",
           static_cast<unsigned>(configured), static_cast<unsigned>(s.entity_id),
           static_cast<unsigned long>(s.caps));
}

void read_attrs(const ZbEntity& s, std::uint16_t cluster, std::uint16_t* attrs, std::uint8_t n) {
  esp_zb_zcl_read_attr_cmd_t cmd{};
  cmd.zcl_basic_cmd.dst_addr_u.addr_short = s.short_addr;
  cmd.zcl_basic_cmd.dst_endpoint = s.endpoint;
  cmd.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
  cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cmd.clusterID = cluster;
  cmd.attr_number = n;
  cmd.attr_field = attrs;
  esp_zb_zcl_read_attr_cmd_req(&cmd);
}

void write_cie_address(std::uint16_t short_addr, std::uint8_t endpoint) {
  // An IAS Zone device only reports once it knows who the CIE (control and indicating
  // equipment) is: us. Idempotent, so it is repeated on every re-arm.
  static esp_zb_ieee_addr_t coord;
  esp_zb_get_long_address(coord);
  static esp_zb_zcl_attribute_t attr;
  attr.id = kAttrCieAddress;
  attr.data.type = ESP_ZB_ZCL_ATTR_TYPE_IEEE_ADDR;
  attr.data.size = sizeof(esp_zb_ieee_addr_t);
  attr.data.value = coord;
  esp_zb_zcl_write_attr_cmd_t w{};
  w.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
  w.zcl_basic_cmd.dst_endpoint = endpoint;
  w.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
  w.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  w.clusterID = ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE;
  w.attr_number = 1;
  w.attr_field = &attr;
  esp_zb_zcl_write_attr_cmd_req(&w);
}

// Read the current values of everything the entity carries (plus Basic identity), so the brain
// has a fresh STATE even when reporting is not configured or the device missed a report.
// `color_caps`: also ask a lamp which colour modes it supports.
void read_device_state(ZbEntity& e, bool color_caps) {
  static std::uint16_t basic_attrs[2] = {kAttrManufacturerName, kAttrModelIdentifier};
  read_attrs(e, kBasicCluster, basic_attrs, 2);

  auto one = [&](std::uint32_t cap, std::uint16_t cluster, std::uint16_t attr) {
    if ((e.caps & cap) == 0) return;
    static std::uint16_t a[1];
    a[0] = attr;
    read_attrs(e, cluster, a, 1);
  };
  one(zcl::kCapOnOff, zcl::cluster::kOnOff, 0x0000);
  one(zcl::kCapLevel, zcl::cluster::kLevel, 0x0000);
  one(zcl::kCapHumidity, zcl::cluster::kHumidity, 0x0000);
  one(zcl::kCapIlluminance, zcl::cluster::kIlluminance, 0x0000);
  one(zcl::kCapBattery, zcl::cluster::kPowerCfg, kAttrBatteryPercent);
  if ((e.caps & zcl::kCapOccupancy) != 0 && (e.caps & zcl::kCapIas) == 0) {
    one(zcl::kCapOccupancy, zcl::cluster::kOccupancy, 0x0000);
  }
  if ((e.caps & zcl::kCapThermostat) != 0) {
    static std::uint16_t th[3] = {kAttrLocalTemperature, kAttrHeatingSetpoint, kAttrSystemMode};
    read_attrs(e, zcl::cluster::kThermostat, th, 3);
  } else {
    one(zcl::kCapTemperature, zcl::cluster::kTemperature, 0x0000);
  }
  if ((e.caps & kColorBits) != 0 || color_caps) {
    static std::uint16_t col[5];
    std::uint8_t n = 0;
    if ((e.caps & zcl::kCapColorTemp) != 0) col[n++] = kAttrColorTempMireds;
    if ((e.caps & zcl::kCapColorXy) != 0) {
      col[n++] = kAttrColorX;
      col[n++] = kAttrColorY;
    }
    if (color_caps) col[n++] = kAttrColorCapabilities;
    if (n > 0) read_attrs(e, zcl::cluster::kColor, col, n);
  }
  if ((e.caps & zcl::kCapIas) != 0) {
    write_cie_address(e.short_addr, e.endpoint);
    static std::uint16_t z[1] = {kAttrZoneStatus};
    read_attrs(e, zcl::cluster::kIasZone, z, 1);
  }
  // If this device advertised Electrical Measurement, also read ActivePower + its scaling
  // factors (AC Power Multiplier / Divisor). The response handler caches the factors and, when a
  // subsequent ActivePower value arrives, scales it to Watts and publishes STATE(power_w). This
  // works even without configured reporting.
  if ((e.caps & zcl::kCapPower) != 0) {
    static std::uint16_t em_attrs[3] = {
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,        // 0x050B
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_MULTIPLIER_ID,  // 0x0604
        ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_DIVISOR_ID,     // 0x0605
    };
    read_attrs(e, ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT, em_attrs, 3);
  }
  if ((e.caps & zcl::kCapEnergy) != 0) {
    static std::uint16_t metering_attrs[3] = {
        ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
        ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
    };
    read_attrs(e, ESP_ZB_ZCL_CLUSTER_ID_METERING, metering_attrs, 3);
  }
}

// Callback for boot-time NWK address lookup (esp_zb_zdo_nwk_addr_req).
// ctx = index into g_ent (pointer-as-integer, same pattern as short_addr).
// Runs in Zigbee stack context.
void on_nwk_addr_resp(esp_zb_zdp_status_t status, esp_zb_zdo_nwk_addr_rsp_t* resp,
                      void* ctx) {
  const auto idx = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(ctx));
  if (idx >= kMaxEntities) return;
  ZbEntity& s = g_ent[idx];
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
    persist_entities();
  }

  // No cached STATE is published here: after a chip reboot the first value the brain sees must
  // be the device's real one (the read below), so its reconciler can tell a device that came
  // back in a different state from one that never changed.
  setup_reporting(s);
  read_device_state(s, (s.caps & kColorBits) != 0);
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
  for (std::size_t i = 0; i < kMaxEntities; ++i) {
    ZbEntity& s = g_ent[i];
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

// ---- incoming attribute values (reports and read responses) ----------------
// A button press is a momentary STATE(event=N) followed, kButtonEventMs later, by event=0: a
// rule "event == N" sees an edge on every press even though two presses carry the same code.
void event_idle_cb(std::uint8_t idx) {
  if (idx >= kMaxEntities || !g_ent[idx].in_use) return;
  publish_attr(g_ent[idx], A::kEvent,
               hubproto::Value::make_enum8(static_cast<std::uint8_t>(A::ButtonEvent::Idle)),
               /*force=*/true);
}

void pulse_event(ZbEntity& e, A::ButtonEvent ev) {
  if (ev == A::ButtonEvent::Idle) return;
  publish_attr(e, A::kEvent, hubproto::Value::make_enum8(static_cast<std::uint8_t>(ev)),
               /*force=*/true);
  esp_zb_scheduler_alarm(event_idle_cb, static_cast<std::uint8_t>(&e - g_ent),
                         A::kButtonEventMs);
}

void apply_ias_status(ZbEntity& e, std::uint16_t zone_status) {
  const bool alarm = hubproto::units::ias_alarm(zone_status);
  if (e.type == hubproto::DeviceType::Contact) {
    publish_attr(e, A::kContact, hubproto::Value::make_bool(!alarm));  // alarm1 = opened
  } else if (e.type == hubproto::DeviceType::Occupancy) {
    publish_attr(e, A::kOccupied, hubproto::Value::make_bool(alarm));
  }
}

// The byte length of a ZCL value. The stack's `size` field is not reliably filled in a read
// response, so trust it only when set and otherwise derive the length from the data type.
std::uint16_t value_size_of(const esp_zb_zcl_attribute_data_t& d) {
  if (d.size != 0) return d.size;
  switch (d.type) {
    case ESP_ZB_ZCL_ATTR_TYPE_BOOL:
    case ESP_ZB_ZCL_ATTR_TYPE_8BITMAP:
    case ESP_ZB_ZCL_ATTR_TYPE_U8:
    case ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM:
      return 1;
    case ESP_ZB_ZCL_ATTR_TYPE_16BITMAP:
    case ESP_ZB_ZCL_ATTR_TYPE_U16:
    case ESP_ZB_ZCL_ATTR_TYPE_S16:
    case ESP_ZB_ZCL_ATTR_TYPE_16BIT_ENUM:
      return 2;
    case ESP_ZB_ZCL_ATTR_TYPE_U24:
      return 3;
    case ESP_ZB_ZCL_ATTR_TYPE_U32:
      return 4;
    case ESP_ZB_ZCL_ATTR_TYPE_U48:
      return 6;
    default:
      return 0;
  }
}

// One attribute value from a device -> STATE. `d` points at the little-endian ZCL value of
// `size` bytes; anything shorter than the type needs is ignored.
void handle_attr(ZbEntity& e, std::uint16_t cl, std::uint16_t id, const std::uint8_t* d,
                 std::uint16_t size) {
  using hubproto::Value;
  namespace U = hubproto::units;
  if (d == nullptr || size == 0) return;

  if (cl == zcl::cluster::kOnOff) {
    if (id == 0x0000) publish_attr(e, A::kOnOff, Value::make_bool(d[0] != 0), /*force=*/true);
  } else if (cl == zcl::cluster::kLevel) {
    if (id == 0x0000 && U::level_valid(d[0])) publish_attr(e, A::kBrightness, Value::make_u8(d[0]));
  } else if (cl == zcl::cluster::kColor) {
    if (id == kAttrColorTempMireds && size >= 2) {
      publish_attr(e, A::kColorTemp, Value::make_u16(le16(d)));
    } else if ((id == kAttrColorX || id == kAttrColorY) && size >= 2) {
      (id == kAttrColorX ? e.color_x : e.color_y) = le16(d);
      (id == kAttrColorX ? e.have_x : e.have_y) = true;
      if (e.have_x && e.have_y) publish_attr(e, A::kColorXy, Value::make_xy(e.color_x, e.color_y));
    } else if (id == kAttrColorCapabilities && size >= 2) {
      const std::uint32_t refined = zcl::refine_color_caps(e.caps, le16(d));
      if (refined != e.caps) {
        ESP_LOGI(kTag, "entity 0x%04x colour capabilities 0x%04x -> caps 0x%04lx",
                 static_cast<unsigned>(e.entity_id), static_cast<unsigned>(le16(d)),
                 static_cast<unsigned long>(refined));
        e.caps = refined;
        persist_entities();
        send_hello();  // the entity now declares a different attribute set
      }
    }
  } else if (cl == zcl::cluster::kTemperature) {
    std::int16_t t = 0;
    if (id == 0x0000 && size >= 2 && U::temp_from_centi(static_cast<std::int16_t>(le16(d)), t)) {
      publish_attr(e, A::kTemperature, Value::make_i16(t));
    }
  } else if (cl == zcl::cluster::kHumidity) {
    std::uint16_t h = 0;
    if (id == 0x0000 && size >= 2 && U::humidity_from_centi(le16(d), h)) {
      publish_attr(e, A::kHumidity, Value::make_u16(h));
    }
  } else if (cl == zcl::cluster::kIlluminance) {
    std::uint16_t lux = 0;
    if (id == 0x0000 && size >= 2 && U::lux_from_measured(le16(d), lux)) {
      publish_attr(e, A::kIlluminance, Value::make_u16(lux));
    }
  } else if (cl == zcl::cluster::kPowerCfg) {
    std::uint8_t pct = 0;
    if (id == kAttrBatteryPercent && U::battery_from_half_percent(d[0], pct)) {
      publish_attr(e, A::kBattery, Value::make_u8(pct));
    }
  } else if (cl == zcl::cluster::kOccupancy) {
    if (id == 0x0000) publish_attr(e, A::kOccupied, Value::make_bool((d[0] & 0x01) != 0));
  } else if (cl == zcl::cluster::kThermostat) {
    std::int16_t t = 0;
    if (id == kAttrLocalTemperature && size >= 2 &&
        U::temp_from_centi(static_cast<std::int16_t>(le16(d)), t)) {
      publish_attr(e, A::kTemperature, Value::make_i16(t));
    } else if (id == kAttrHeatingSetpoint && size >= 2 &&
               U::temp_from_centi(static_cast<std::int16_t>(le16(d)), t)) {
      publish_attr(e, A::kSetpoint, Value::make_i16(t));
    } else if (id == kAttrSystemMode && U::mode_valid(d[0])) {
      publish_attr(e, A::kMode, Value::make_enum8(d[0]));
    }
  } else if (cl == zcl::cluster::kIasZone) {
    if (id == kAttrZoneStatus && size >= 2) apply_ias_status(e, le16(d));
  } else if (cl == zcl::cluster::kMultistateIn) {
    if (id == kAttrPresentValue && size >= 2) pulse_event(e, zcl::multistate_event(le16(d)));
  } else if (cl == zcl::cluster::kElecMeas) {
    if (size < 2) return;
    switch (id) {
      case ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_MULTIPLIER_ID:
        e.ac_power_mul = le16(d) == 0 ? 1 : le16(d);
        persist_entities();
        ESP_LOGI(kTag, "entity 0x%04x ACPowerMultiplier=%u", static_cast<unsigned>(e.entity_id),
                 static_cast<unsigned>(e.ac_power_mul));
        break;
      case ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_DIVISOR_ID:
        e.ac_power_div = le16(d) == 0 ? 1 : le16(d);
        persist_entities();
        ESP_LOGI(kTag, "entity 0x%04x ACPowerDivisor=%u", static_cast<unsigned>(e.entity_id),
                 static_cast<unsigned>(e.ac_power_div));
        break;
      case ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID: {
        const auto raw = static_cast<std::int16_t>(le16(d));
        publish_attr(e, A::kPowerW, Value::make_u16(scale_active_power(e, raw)));
        break;
      }
      default:
        break;
    }
  } else if (cl == zcl::cluster::kMetering) {
    switch (id) {
      case ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID:
      case ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID: {
        if (size < 3) return;
        // ZCL spec: UINT24, 3 bytes LE (not UINT32)
        const std::uint32_t val = static_cast<std::uint32_t>(d[0]) |
                                  (static_cast<std::uint32_t>(d[1]) << 8) |
                                  (static_cast<std::uint32_t>(d[2]) << 16);
        if (id == ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID) {
          e.metering_mul = val == 0 ? 1u : val;
        } else {
          e.metering_div = val == 0 ? 1u : val;
        }
        persist_entities();
        ESP_LOGI(kTag, "entity 0x%04x Metering %s=%lu", static_cast<unsigned>(e.entity_id),
                 id == ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID ? "Multiplier" : "Divisor",
                 static_cast<unsigned long>(id == ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID
                                                ? e.metering_mul
                                                : e.metering_div));
        break;
      }
      case ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID: {
        if (size < 6) return;
        // CurrentSummationDelivered: UINT48, 6 bytes LE
        std::uint64_t summation = 0;
        for (int b = 5; b >= 0; --b) summation = (summation << 8) | d[b];
        publish_attr(e, A::kEnergyWh, Value::make_u32(scale_summation(e, summation)));
        break;
      }
      default:
        break;
    }
  }
}

// ---- Zigbee discovery -----------------------------------------------------
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

// An entity just became real (new, or a refreshed re-announce): arm it and tell the brain.
void bring_up_entity(ZbEntity& e, bool color_caps) {
  setup_reporting(e);
  read_device_state(e, color_caps);
  send_hello();  // topology changed -> brain re-QUERYs and gets this entity
}

void begin_ias(std::uint16_t short_addr, std::uint8_t endpoint, const zcl::Profile& p) {
  // An IAS endpoint we already know (a re-announce) needs no new zone-type read.
  if (ZbEntity* known = find_by_ieee_ep(g_pending_ieee, endpoint)) {
    known->short_addr = short_addr;
    persist_entities();
    bring_up_entity(*known, false);
    return;
  }
  for (auto& slot : g_ias_pending) {
    if (slot.used && slot.short_addr == short_addr && slot.endpoint == endpoint) return;
  }
  for (auto& slot : g_ias_pending) {
    if (slot.used) continue;
    slot.used = true;
    slot.short_addr = short_addr;
    slot.endpoint = endpoint;
    std::memcpy(slot.ieee, g_pending_ieee, sizeof(esp_zb_ieee_addr_t));
    slot.profile = p;
    write_cie_address(short_addr, endpoint);
    static std::uint16_t zt[1] = {kAttrZoneType};
    esp_zb_zcl_read_attr_cmd_t cmd{};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    cmd.zcl_basic_cmd.dst_endpoint = endpoint;
    cmd.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE;
    cmd.attr_number = 1;
    cmd.attr_field = zt;
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    ESP_LOGI(kTag, "device 0x%04x ep%u is an IAS zone: reading its zone type", short_addr,
             static_cast<unsigned>(endpoint));
    return;
  }
  ESP_LOGW(kTag, "too many IAS zones joining at once; 0x%04x ep%u ignored", short_addr,
           static_cast<unsigned>(endpoint));
}

void finish_ias(std::uint16_t short_addr, std::uint8_t endpoint, std::uint16_t zone_type) {
  for (auto& slot : g_ias_pending) {
    if (!slot.used || slot.short_addr != short_addr || slot.endpoint != endpoint) continue;
    slot.used = false;
    zcl::Profile p = slot.profile;
    if (!zcl::resolve_ias(zone_type, p)) {
      ESP_LOGI(kTag, "device 0x%04x ep%u: IAS zone type 0x%04x is not modelled — ignored",
               short_addr, static_cast<unsigned>(endpoint), static_cast<unsigned>(zone_type));
      return;
    }
    ZbEntity* e = register_entity(short_addr, slot.ieee, endpoint, p.type, p.caps, false);
    if (e == nullptr) {
      ESP_LOGE(kTag, "entity table full; cannot map device 0x%04x", short_addr);
      return;
    }
    ESP_LOGI(kTag, "mapped device 0x%04x ep%u -> %s entity 0x%04x (IAS zone type 0x%04x)",
             short_addr, static_cast<unsigned>(endpoint),
             p.type == hubproto::DeviceType::Contact ? "contact" : "occupancy",
             static_cast<unsigned>(e->entity_id), static_cast<unsigned>(zone_type));
    bring_up_entity(*e, false);
    return;
  }
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
  for (std::uint8_t i = 0; i < desc->app_input_cluster_count; ++i) {
    ESP_LOGI(kTag, "    [in ] cluster 0x%04x", static_cast<unsigned>(clusters[i]));
  }
  for (std::uint8_t i = 0; i < desc->app_output_cluster_count; ++i) {
    const std::uint16_t c = clusters[desc->app_input_cluster_count + i];
    ESP_LOGI(kTag, "    [out] cluster 0x%04x", static_cast<unsigned>(c));
  }

  // What this endpoint is, in the hub's terms (core/zcl_profile.hpp).
  const zcl::Profile p = zcl::classify(clusters, desc->app_input_cluster_count,
                                       clusters + desc->app_input_cluster_count,
                                       desc->app_output_cluster_count);
  if (p.ias) {
    begin_ias(short_addr, desc->endpoint, p);
    return;
  }
  if (!p.valid) {
    ESP_LOGI(kTag, "  device 0x%04x ep %u: nothing the hub models — skipped", short_addr,
             static_cast<unsigned>(desc->endpoint));
    return;
  }
  ZbEntity* e = register_entity(short_addr, g_pending_ieee, desc->endpoint, p.type, p.caps,
                                p.color_caps_pending);
  if (e == nullptr) {
    ESP_LOGE(kTag, "entity table full; cannot map device 0x%04x", short_addr);
    return;
  }
  ESP_LOGI(kTag, "mapped device 0x%04x ep%u -> type 0x%02x entity 0x%04x (caps 0x%04lx)",
           short_addr, static_cast<unsigned>(desc->endpoint),
           static_cast<unsigned>(p.type), static_cast<unsigned>(e->entity_id),
           static_cast<unsigned long>(e->caps));
  bring_up_entity(*e, p.color_caps_pending);
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

// ---- ZCL indications (reports / read responses / command answers) -----------
// A remote's commands (On/Off, Level) reach the coordinator as raw ZCL frames: they are not
// addressed to a cluster the coordinator hosts, so the SDK's typed callbacks never see them.
// Runs in stack context. Returns false: the stack still does its normal processing.
bool raw_command_cb(std::uint8_t bufid) {
  auto* hdr = ZB_BUF_GET_PARAM(bufid, zb_zcl_parsed_hdr_t);
  if (hdr == nullptr || hdr->is_common_command || hdr->cmd_direction != 0) return false;
  if (hdr->cluster_id != zcl::cluster::kOnOff && hdr->cluster_id != zcl::cluster::kLevel) {
    return false;
  }
  const std::uint16_t src = ZB_ZCL_PARSED_HDR_SHORT_DATA(hdr).source.u.short_addr;
  const std::uint8_t src_ep = ZB_ZCL_PARSED_HDR_SHORT_DATA(hdr).src_endpoint;
  ZbEntity* e = find_by_addr(src, src_ep);
  if (e == nullptr || (e->caps & zcl::kCapButton) == 0) return false;
  std::uint8_t first = 0;
  if (zb_buf_len(bufid) >= 1) first = *static_cast<std::uint8_t*>(zb_buf_begin(bufid));
  pulse_event(*e, zcl::button_event(hdr->cluster_id, hdr->cmd_id, first));
  return false;
}

esp_err_t zcl_action_handler(esp_zb_core_action_callback_id_t cb_id,
                             const void* message) {
  switch (cb_id) {
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
      const auto* m = static_cast<const esp_zb_zcl_report_attr_message_t*>(message);
      if (m->attribute.data.value == nullptr) break;
      ZbEntity* e = find_by_addr(m->src_address.u.short_addr, m->src_endpoint);
      if (e == nullptr) break;
      handle_attr(*e, m->cluster, m->attribute.id,
                  static_cast<const std::uint8_t*>(m->attribute.data.value),
                  value_size_of(m->attribute.data));
      break;
    }
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID: {
      const auto* m =
          static_cast<const esp_zb_zcl_cmd_default_resp_message_t*>(message);
      PendingCmd* p = find_pending(m->info.header.tsn, m->info.src_address.u.short_addr);
      if (p == nullptr) break;  // not one of our tracked commands
      finish_pending(p, m->info.src_address.u.short_addr, m->info.src_endpoint,
                     m->status_code == ESP_ZB_ZCL_STATUS_SUCCESS,
                     static_cast<int>(m->status_code));
      break;
    }
    case ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID: {
      const auto* m = static_cast<const esp_zb_zcl_cmd_write_attr_resp_message_t*>(message);
      PendingCmd* p = find_pending(m->info.header.tsn, m->info.src_address.u.short_addr);
      if (p == nullptr) break;
      bool ok = m->info.status == ESP_ZB_ZCL_STATUS_SUCCESS;
      int code = static_cast<int>(m->info.status);
      for (const auto* v = m->variables; v != nullptr; v = v->next) {
        if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
          ok = false;
          code = static_cast<int>(v->status);
        }
      }
      finish_pending(p, m->info.src_address.u.short_addr, m->info.src_endpoint, ok, code);
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
          continue;
        }
        if (v->attribute.data.value == nullptr) continue;
        const auto* raw = static_cast<const std::uint8_t*>(v->attribute.data.value);
        if (m->info.cluster == zcl::cluster::kIasZone && v->attribute.id == kAttrZoneType &&
            value_size_of(v->attribute.data) >= 2) {
          finish_ias(src, m->info.src_endpoint, le16(raw));
          continue;
        }
        ZbEntity* e = find_by_addr(src, m->info.src_endpoint);
        if (e == nullptr) continue;
        handle_attr(*e, m->info.cluster, v->attribute.id, raw, value_size_of(v->attribute.data));
      }
      break;
    }
    case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID: {
      // The device asks to be enrolled with us (the CIE). Say yes; the zone id is arbitrary.
      const auto* m = static_cast<const esp_zb_zcl_ias_zone_enroll_request_message_t*>(message);
      esp_zb_zcl_ias_zone_enroll_response_cmd_t resp{};
      resp.zcl_basic_cmd.dst_addr_u.addr_short = m->info.src_address.u.short_addr;
      resp.zcl_basic_cmd.dst_endpoint = m->info.src_endpoint;
      resp.zcl_basic_cmd.src_endpoint = kCoordEndpoint;
      resp.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
      resp.enroll_rsp_code = ESP_ZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS;
      resp.zone_id = static_cast<std::uint8_t>(m->info.src_endpoint);
      esp_zb_zcl_ias_zone_enroll_cmd_resp(&resp);
      ESP_LOGI(kTag, "IAS zone enroll request from 0x%04x (type 0x%04x): enrolled",
               static_cast<unsigned>(m->info.src_address.u.short_addr),
               static_cast<unsigned>(m->zone_type));
      break;
    }
    case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID: {
      const auto* m =
          static_cast<const esp_zb_zcl_ias_zone_status_change_notification_message_t*>(message);
      ZbEntity* e = find_by_addr(m->info.src_address.u.short_addr, m->info.src_endpoint);
      if (e != nullptr) apply_ias_status(*e, m->zone_status);
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
  for (const auto& s : g_ent) {
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
      g_zb_stack_up = true;
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

  // Basic + Identify endpoint so the coordinator is a valid ZCL node, plus the IAS Zone *client*
  // cluster: it is what lets an IAS sensor's enroll request and status notifications reach us.
  esp_zb_cluster_list_t* clusters = esp_zb_zcl_cluster_list_create();
  esp_zb_cluster_list_add_basic_cluster(clusters,
                                        esp_zb_basic_cluster_create(nullptr),
                                        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(clusters,
                                           esp_zb_identify_cluster_create(nullptr),
                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_ias_zone_cluster_cfg_t ias_cfg{};
  esp_zb_cluster_list_add_ias_zone_cluster(clusters, esp_zb_ias_zone_cluster_create(&ias_cfg),
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
  esp_zb_ep_list_t* ep_list = esp_zb_ep_list_create();
  esp_zb_endpoint_config_t ep_cfg{};
  ep_cfg.endpoint = kCoordEndpoint;
  ep_cfg.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  ep_cfg.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
  ep_cfg.app_device_version = 0;
  esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
  esp_zb_device_register(ep_list);

  esp_zb_core_action_handler_register(zcl_action_handler);
  esp_zb_raw_command_handler_register(raw_command_cb);
  esp_zb_set_primary_network_channel_set(kPrimaryChannelMask);
  ESP_ERROR_CHECK(esp_zb_start(false));
  esp_zb_stack_main_loop();
}

}  // namespace

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  g_boot_id = fw::next_boot_id();  // persisted reboot counter (SPEC Phase 5)
  restore_entities();  // re-declare still-joined devices without a re-pair

  // Bring up the UART link + transport to the brain first (Phase 2 transport).
  g_bus_mtx = xSemaphoreCreateMutex();
  static fw::EspUartLink link([] {
    fw::EspUartLink::Config c;
    c.port = UART_NUM_1;
    c.tx_gpio = kUartTxGpio;
    c.rx_gpio = kUartRxGpio;
    c.task_stack = 4096;  // OTA writes flash from this task (docs/ota.md)
    return c;
  }());
  ESP_ERROR_CHECK(link.begin());
  static core::UartTransport bus(link);
  g_bus = &bus;
  bus.subscribe(&on_uart_message, nullptr);

  // Firmware updates from the brain (docs/ota.md). The receiver polls from the
  // UART RX task (link tick) so it is serialized with inbound messages.
  static fw::EspOtaSink ota_sink;
  static core::OtaReceiver ota_rx(bus, ota_sink, [] {
    core::OtaReceiver::Config c;
    c.own_chip_id = kOwnChipId;
    c.boot_id = g_boot_id;
    return c;
  }());
  static auto ota_tick = [](void* ctx) {
    auto* rx = static_cast<core::OtaReceiver*>(ctx);
    const auto uptime_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    g_ota_confirm.tick(uptime_ms);
    rx->poll(uptime_ms);
    if (rx->reboot_pending()) fw::ota_reboot_into_new_image(UART_NUM_1);
  };
  link.set_tick(ota_tick, &ota_rx);
  link.start();
#ifdef KS_TEST_CRASH
  // Test image for the rollback path (docs/ota.md): crash well before the 30 s
  // confirm grace, so the bootloader must revert to the previous slot.
  xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(8000)); abort(); }, "ks_crash", 2048,
              nullptr, 5, nullptr);
#endif

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
  ESP_LOGI(kTag, "Zigbee translator + bus (multi-type)");
  xTaskCreate(esp_zb_task, "zb_main", 4096, nullptr, 5, nullptr);
}
