// The Matter bridge on the Thread co-processor — see matter_bridge.hpp for the
// protocol. Ported from the bridge that lived on this chip before the F2 fork
// moved the Matter server from the brain onto the co-processor
// (git show b9e8897^:firmware/thread-c6/main/main.cpp).
//
// The brain mirrors the DECLARE / STATE / UNDECLARE frames of the Zigbee chip
// here and forwards our COMMANDs back to it, so the bridge itself only has to
// speak Matter and hubproto.
#include "matter_bridge.hpp"

#ifdef CHIP_HAVE_CONFIG_H

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_matter.h"
// The Electrical Energy Measurement cluster's server-side helper (the
// integration header of that one cluster; see main/CMakeLists.txt).
#include "integration.h"

#include <app/clusters/boolean-state-server/BooleanStateCluster.h>
#include <app/clusters/occupancy-sensor-server/OccupancySensingCluster.h>
#include <app/server/Server.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
// chip::Platform::New / Delete — the same header esp-matter's own bridge
// examples use.
#include "support/CHIPMem.h"

#include <app_bridged_device.h>

#include "core/bus.hpp"
#include "core/matter_plan.hpp"
#include "core/zcl_profile.hpp"
#include "hubproto/attrs.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/units.hpp"
#include "hubproto/version.hpp"
#include "keystone_bridged_device.hpp"

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

namespace {

namespace A = hubproto::attr;
namespace M = core::matter;
namespace U = hubproto::units;

const char* kTag = "matter_bridge";

// Matter Electrical Power Measurement cluster (0x0090).
// ActivePower (0x0008) is nullable int64, milliwatts.
constexpr std::uint32_t kEpmClusterId = 0x0090;
constexpr std::uint32_t kEpmAttrActivePower = 0x0008;
constexpr std::uint32_t kEpmAttrPowerMode = 0x0000;

// The last value the brain told us per bridged endpoint (-1 = unknown).
// Matter calls our PRE_UPDATE callback for OUR OWN attribute::update() too;
// without this the STATE we mirror comes straight back as a COMMAND, which
// re-triggers the device and any rule reacting to it (seen: an on/off
// ping-pong lasting seconds).
struct Mirror {
  std::int32_t onoff = -1;
  std::int32_t level = -1;
  std::int32_t ct = -1;
  std::int32_t x = -1;
  std::int32_t y = -1;
  std::int32_t setpoint = -1;  // hub units (0.1 °C)
  std::int32_t mode = -1;
  // The Matter stack reacts to our own on/off and level updates with changes
  // of its own (turning a lamp on moves CurrentLevel to the stored level,
  // i.e. 1). Those are not a controller's request: for a moment after we
  // mirror an "on", level/colour changes that differ from the hub's value are
  // refused instead of being forwarded to the real lamp.
  std::uint32_t quiet_until_ms = 0;
};
// Endpoint ids only ever grow (a removed endpoint's id is not reused), so the
// table is keyed by endpoint id rather than indexed by it.
struct MirrorSlot {
  bool used;
  std::uint16_t ep;
  Mirror m;
};
constexpr int kMirrorSlots = 32;
MirrorSlot g_mirror_slots[kMirrorSlots];

Mirror* mirror_for(std::uint16_t ep) {
  MirrorSlot* free_slot = nullptr;
  for (auto& s : g_mirror_slots) {
    if (s.used && s.ep == ep) return &s.m;
    if (!s.used && free_slot == nullptr) free_slot = &s;
  }
  if (free_slot == nullptr) return nullptr;
  free_slot->used = true;
  free_slot->ep = ep;
  free_slot->m = Mirror{};
  return &free_slot->m;
}

core::Bus* g_bus = nullptr;
std::uint8_t g_boot_id = 0;
node_t* g_node = nullptr;
std::uint16_t g_aggregator_endpoint_id = 0;
constexpr std::uint32_t kQuietAfterMirrorMs = 600;
// While an endpoint is being created or restored, and for a few seconds after
// boot, the Matter stack initialises attributes (a lamp's CurrentLevel to 1,
// ...). Those writes are not a controller's request and must never reach the
// real device.
constexpr std::uint32_t kBootGraceMs = 8000;
bool g_building = false;
std::uint16_t g_next_seq = 1;

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

hubproto::Header header(hubproto::MsgType type, std::uint16_t entity = hubproto::kSelfEntityId,
                        std::uint16_t seq = 0) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = 0x02;  // us, the Thread/Matter co-processor
  h.boot_id = g_boot_id;
  h.seq = seq ? seq : g_next_seq++;
  h.entity_id = entity;
  return h;
}

// Send a COMMAND for a bridged entity toward the brain.
void send_command(std::uint16_t entity_id, std::uint8_t attr, const hubproto::Value& v) {
  if (g_bus == nullptr) return;
  hubproto::Command cmd{};
  cmd.entity_id = entity_id;
  cmd.attr_id = attr;
  cmd.value = v;
  std::uint8_t buf[16];
  auto r = hubproto::encode_command(cmd, buf, sizeof(buf));
  if (!r.ok) return;
  g_bus->publish(header(hubproto::MsgType::Command, entity_id), buf, r.len);
  if (v.type == hubproto::ValueType::XyColor) {
    std::uint16_t x = 0, y = 0;
    v.as_xy(x, y);
    ESP_LOGI(kTag, "COMMAND -> entity=0x%04x attr=0x%02x xy=(%u,%u)", entity_id,
             static_cast<unsigned>(attr), static_cast<unsigned>(x), static_cast<unsigned>(y));
  } else {
    long n = 0;
    switch (v.type) {
      case hubproto::ValueType::Bool: n = v.as_bool() ? 1 : 0; break;
      case hubproto::ValueType::U8: n = v.as_u8(); break;
      case hubproto::ValueType::Enum8: n = v.as_enum8(); break;
      case hubproto::ValueType::I16: n = v.as_i16(); break;
      case hubproto::ValueType::U16: n = v.as_u16(); break;
      default: break;
    }
    ESP_LOGI(kTag, "COMMAND -> entity=0x%04x attr=0x%02x value=%ld", entity_id,
             static_cast<unsigned>(attr), n);
  }
}

// A controller dragging a slider makes the Matter stack walk CurrentLevel (or
// the colour) through many values; forwarding every step would flood the
// Zigbee device. Commands for everything but on/off are held for kDebounceMs
// and only the latest value per (entity, attribute) is sent.
constexpr std::uint32_t kDebounceMs = 150;
struct QueuedCmd {
  bool used;
  std::uint16_t entity;
  std::uint8_t attr;
  hubproto::Value v;
  std::uint32_t due_ms;
};
QueuedCmd g_q[8] = {};
SemaphoreHandle_t g_q_mu = nullptr;

void queue_command(std::uint16_t entity, std::uint8_t attr, const hubproto::Value& v) {
  if (g_q_mu == nullptr) return;
  xSemaphoreTake(g_q_mu, portMAX_DELAY);
  QueuedCmd* slot = nullptr;
  for (auto& q : g_q) {
    if (q.used && q.entity == entity && q.attr == attr) {
      slot = &q;
      break;
    }
  }
  if (slot == nullptr) {
    for (auto& q : g_q) {
      if (!q.used) {
        slot = &q;
        break;
      }
    }
  }
  if (slot != nullptr) {
    slot->used = true;
    slot->entity = entity;
    slot->attr = attr;
    slot->v = v;
    slot->due_ms = now_ms() + kDebounceMs;
  }
  xSemaphoreGive(g_q_mu);
}

void forward_task(void*) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(40));
    for (;;) {
      QueuedCmd out{};
      bool have = false;
      xSemaphoreTake(g_q_mu, portMAX_DELAY);
      for (auto& q : g_q) {
        if (q.used && static_cast<std::int32_t>(now_ms() - q.due_ms) >= 0) {
          out = q;
          q.used = false;
          have = true;
          break;
        }
      }
      xSemaphoreGive(g_q_mu);
      if (!have) break;
      send_command(out.entity, out.attr, out.v);
    }
  }
}

// ---- endpoint construction ----------------------------------------------------

esp_matter_attr_val_t nullable_u8(std::uint8_t v) {
  esp_matter_attr_val_t a{};
  a.type = ESP_MATTER_VAL_TYPE_NULLABLE_UINT8;
  a.val.u8 = v;
  return a;
}
esp_matter_attr_val_t nullable_i16(std::int16_t v) {
  esp_matter_attr_val_t a{};
  a.type = ESP_MATTER_VAL_TYPE_NULLABLE_INT16;
  a.val.i16 = v;
  return a;
}
esp_matter_attr_val_t nullable_u16(std::uint16_t v) {
  esp_matter_attr_val_t a{};
  a.type = ESP_MATTER_VAL_TYPE_NULLABLE_UINT16;
  a.val.u16 = v;
  return a;
}

// esp-matter defaults StartUpOnOff to Off, so every boot of this chip forced
// the bridged endpoint Off and the PRE_UPDATE hook forwarded that as a
// COMMAND, switching the real device off. The hub owns the state
// (Reconciler): null = "keep the previous value", i.e. no startup action.
template <typename Cfg>
void keep_on_off(Cfg& c) {
  c.on_off_lighting.start_up_on_off = nullable<std::uint8_t>();
}

void add_extras(esp_matter::endpoint_t* ep, std::uint16_t extras) {
  const unsigned epid = esp_matter::endpoint::get_id(ep);
  if (extras & M::kXTemperature) {
    cluster::temperature_measurement::config_t c;
    cluster::temperature_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
  }
  if (extras & M::kXHumidity) {
    cluster::relative_humidity_measurement::config_t c;
    cluster::relative_humidity_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
  }
  if (extras & M::kXIlluminance) {
    cluster::illuminance_measurement::config_t c;
    cluster::illuminance_measurement::create(ep, &c, CLUSTER_FLAG_SERVER);
  }
  if (extras & M::kXBattery) {
    cluster::power_source::config_t c;
    c.status = 1;  // Active
    c.feature_flags = cluster::power_source::feature::battery::get_id();
    c.features.battery.bat_charge_level = 0;  // OK
    c.features.battery.bat_replacement_needed = false;
    c.features.battery.bat_replaceability = 1;  // not replaceable: we cannot tell
    cluster_t* ps = cluster::power_source::create(ep, &c, CLUSTER_FLAG_SERVER);
    if (ps) {
      cluster::power_source::attribute::create_bat_percent_remaining(
          ps, nullable<std::uint8_t>(), nullable<std::uint8_t>(0), nullable<std::uint8_t>(200));
    }
  }
  if (extras & M::kXPower) {
    esp_matter::cluster_t* epm = esp_matter::cluster::create(ep, kEpmClusterId, CLUSTER_FLAG_SERVER);
    if (epm) {
      // PowerMode: AC = 0 (mandatory).
      esp_matter_attr_val_t pm{};
      pm.type = ESP_MATTER_VAL_TYPE_ENUM8;
      pm.val.u8 = 0;
      esp_matter::attribute::create(epm, kEpmAttrPowerMode, ATTRIBUTE_FLAG_NONE, pm);
      // ActivePower: nullable int64, milliwatts.
      esp_matter_attr_val_t ap{};
      ap.type = ESP_MATTER_VAL_TYPE_NULLABLE_INT64;
      ap.val.i64 = 0;
      esp_matter::attribute::create(epm, kEpmAttrActivePower, ATTRIBUTE_FLAG_NULLABLE, ap);
      ESP_LOGI(kTag, "added ElectricalPowerMeasurement cluster to ep %u", epid);
    }
  }
  if (extras & M::kXEnergy) {
    // EEM cluster: ImportedEnergy + CumulativeEnergy features.
    // create() internally adds CumulativeEnergyImported attribute (managed).
    cluster::electrical_energy_measurement::config_t eem_cfg;
    eem_cfg.feature_flags =
        cluster::electrical_energy_measurement::feature::imported_energy::get_id() |
        cluster::electrical_energy_measurement::feature::cumulative_energy::get_id();
    cluster::electrical_energy_measurement::create(ep, &eem_cfg, CLUSTER_FLAG_SERVER);
    ESP_LOGI(kTag, "added ElectricalEnergyMeasurement cluster to ep %u", epid);
  }
}

// Bridged endpoint creation callback: populate the newly created endpoint
// (either freshly created via app_bridge_create_new_device, or restored from
// NVS at boot) with the cluster set for its plan. `packed` is
// core::matter::Plan::packed().
esp_err_t create_bridged_clusters(esp_matter::endpoint_t* ep, std::uint32_t packed,
                                  void* /*priv*/) {
  const std::uint16_t kind = M::kind_of(packed);
  const std::uint16_t extras = M::extras_of(packed);
  esp_err_t err = ESP_ERR_INVALID_ARG;
  switch (kind) {
    case M::kOnOffLight: {
      on_off_light::config_t c;
      keep_on_off(c);
      err = on_off_light::add(ep, &c);
      break;
    }
    case M::kDimmableLight: {
      dimmable_light::config_t c;
      keep_on_off(c);
      c.level_control_lighting.start_up_current_level = nullable<std::uint8_t>();
      err = dimmable_light::add(ep, &c);
      break;
    }
    case M::kColorTemperatureLight: {
      color_temperature_light::config_t c;
      keep_on_off(c);
      c.level_control_lighting.start_up_current_level = nullable<std::uint8_t>();
      c.color_control_color_temperature.start_up_color_temperature_mireds = nullable<std::uint16_t>();
      err = color_temperature_light::add(ep, &c);
      break;
    }
    case M::kExtendedColorLight: {
      extended_color_light::config_t c;
      keep_on_off(c);
      c.level_control_lighting.start_up_current_level = nullable<std::uint8_t>();
      c.color_control_color_temperature.start_up_color_temperature_mireds = nullable<std::uint16_t>();
      err = extended_color_light::add(ep, &c);
      break;
    }
    case M::kTemperatureSensor: {
      temperature_sensor::config_t c;
      err = temperature_sensor::add(ep, &c);
      break;
    }
    case M::kHumiditySensor: {
      humidity_sensor::config_t c;
      err = humidity_sensor::add(ep, &c);
      break;
    }
    case M::kLightSensor: {
      light_sensor::config_t c;
      err = light_sensor::add(ep, &c);
      break;
    }
    case M::kOccupancySensor: {
      occupancy_sensor::config_t c;
      // PIR sensor: type 0 = PIR, bitmap bit0 = PIR.
      c.occupancy_sensing.occupancy_sensor_type = 0;
      c.occupancy_sensing.occupancy_sensor_type_bitmap = 1;
      // The cluster refuses to be created without at least one sensing
      // feature (it aborts).
      c.occupancy_sensing.feature_flags =
          cluster::occupancy_sensing::feature::passive_infrared::get_id();
      err = occupancy_sensor::add(ep, &c);
      break;
    }
    case M::kContactSensor: {
      contact_sensor::config_t c;
      c.boolean_state.state_value = true;  // closed until told otherwise
      err = contact_sensor::add(ep, &c);
      break;
    }
    case M::kThermostat: {
      thermostat::config_t c;
      c.thermostat.control_sequence_of_operation = 2;  // heating only
      c.thermostat.system_mode = 4;                    // heat
      c.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id();
      err = thermostat::add(ep, &c);
      break;
    }
    case M::kGenericSwitch: {
      generic_switch::config_t c;
      c.switch_cluster.number_of_positions = 2;
      c.switch_cluster.feature_flags =
          cluster::switch_cluster::feature::momentary_switch::get_id() |
          cluster::switch_cluster::feature::momentary_switch_release::get_id() |
          cluster::switch_cluster::feature::momentary_switch_long_press::get_id();
      err = generic_switch::add(ep, &c);
      break;
    }
    default:
      ESP_LOGE(kTag, "bridged endpoint: unknown kind 0x%04x", static_cast<unsigned>(kind));
      return ESP_ERR_INVALID_ARG;
  }
  if (err != ESP_OK) return err;
  add_extras(ep, extras);
  return ESP_OK;
}

void forget_endpoint_state(std::uint16_t ep) {
  for (auto& s : g_mirror_slots) {
    if (s.used && s.ep == ep) s.used = false;
  }
}

void remove_endpoint(std::uint16_t ep, app_bridged_device_t* bd, const char* why,
                     std::uint16_t entity_id) {
  // The Matter stack reads endpoints from its own task: take its lock
  // (recursive, so safe if already held).
  esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
  const esp_err_t err = app_bridge_remove_device(bd);
  forget_endpoint_state(ep);  // unknown again
  ESP_LOGI(kTag, "%s entity 0x%04x: removed Matter endpoint %u (%s)", why, entity_id,
           static_cast<unsigned>(ep), err == ESP_OK ? "ok" : esp_err_to_name(err));
}

// Look up (or create) the Matter endpoint that mirrors a given hubproto entity.
std::uint16_t ensure_bridged_endpoint(std::uint8_t chip_id, std::uint16_t entity_id,
                                      const M::Plan& plan) {
  if (plan.kind == M::kNone) return chip::kInvalidEndpointId;
  keystone::BridgedAddr addr{chip_id, entity_id};
  std::uint16_t ep = app_bridge_get_endpoint(&addr);
  if (ep != chip::kInvalidEndpointId) {
    app_bridged_device_t* old = app_bridge_get_device(ep);
    const std::uint32_t stored = old ? old->get_matter_device()->persistent_info.device_type_id : 0;
    if (old == nullptr || M::stored_matches(stored, plan)) return ep;
    // The entity changed shape (a lamp's colour modes were read, a sensor
    // gained a battery...): the cluster set of an endpoint cannot change once
    // created, so replace it. Controllers see a new accessory.
    ESP_LOGW(kTag, "entity 0x%04x changed shape (0x%08lx -> 0x%08lx): re-creating its endpoint",
             entity_id, static_cast<unsigned long>(stored),
             static_cast<unsigned long>(plan.packed()));
    remove_endpoint(ep, old, "reshape", entity_id);
  }
  // The last argument is the device's private data, not an out-parameter: the
  // new endpoint is found again through its address.
  g_building = true;
  const esp_err_t err = app_bridge_create_new_device(g_node, g_aggregator_endpoint_id,
                                                     plan.packed(), &addr, nullptr);
  g_building = false;
  const std::uint16_t new_ep =
      err == ESP_OK ? app_bridge_get_endpoint(&addr) : chip::kInvalidEndpointId;
  if (new_ep == chip::kInvalidEndpointId) {
    ESP_LOGE(kTag, "app_bridge_create_new_device failed for entity 0x%04x: %d", entity_id, err);
    matter_bridge::log_table();
    return chip::kInvalidEndpointId;
  }
  forget_endpoint_state(new_ep);
  ESP_LOGI(kTag, "bridged: entity 0x%04x (chip 0x%02x) -> Matter endpoint %u (kind 0x%04x, extras 0x%02x)",
           entity_id, chip_id, static_cast<unsigned>(new_ep), static_cast<unsigned>(plan.kind),
           static_cast<unsigned>(plan.extras));
  return new_ep;
}

// The entity is gone from the hub (a device was unpaired): remove the Matter
// endpoint that mirrored it, so controllers (Apple Home...) stop listing a
// dead accessory. Also forgets it in NVS (app_bridge_remove_device).
void remove_bridged_endpoint(std::uint8_t chip_id, std::uint16_t entity_id) {
  keystone::BridgedAddr addr{chip_id, entity_id};
  const std::uint16_t ep = app_bridge_get_endpoint(&addr);
  if (ep == chip::kInvalidEndpointId) {
    ESP_LOGI(kTag, "undeclare entity 0x%04x: no bridged endpoint for it", entity_id);
    return;
  }
  app_bridged_device_t* bd = app_bridge_get_device(ep);
  if (bd == nullptr) return;
  remove_endpoint(ep, bd, "undeclare", entity_id);
}

// ---- STATE from the brain -> the mirrored Matter attribute ------------------------

void emit_button_event(std::uint16_t ep, A::ButtonEvent ev) {
  namespace sw = cluster::switch_cluster::event;
  esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
  switch (ev) {
    case A::ButtonEvent::Idle:
      return;
    case A::ButtonEvent::Hold:
      sw::send_initial_press(ep, 1);
      sw::send_long_press(ep, 1);
      break;
    case A::ButtonEvent::Release:
      sw::send_long_release(ep, 0);
      break;
    default:  // press, double, on, off, up, down: a Matter switch only knows "pressed"
      sw::send_initial_press(ep, 1);
      sw::send_short_release(ep, 0);
      break;
  }
  ESP_LOGI(kTag, "STATE -> Matter ep=%u button event %u", static_cast<unsigned>(ep),
           static_cast<unsigned>(ev));
}

void update_attr(std::uint16_t ep, std::uint32_t cluster_id, std::uint32_t attr_id,
                 esp_matter_attr_val_t v, const char* what, long shown) {
  // attribute::update touches the Matter data model — must hold the stack lock
  // when called from a non-Matter task (e.g. the UART RX task), otherwise the
  // commissioning PASE handshake can corrupt state and crash.
  esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
  const esp_err_t err = attribute::update(ep, cluster_id, attr_id, &v);
  ESP_LOGI(kTag, "STATE -> Matter ep=%u %s=%ld%s", static_cast<unsigned>(ep), what, shown,
           err == ESP_OK ? "" : " (update failed)");
}

void apply_value(std::uint16_t ep, std::uint8_t attr_id, const hubproto::Value& v) {
  Mirror dummy;
  Mirror* mp = mirror_for(ep);
  Mirror& m = mp != nullptr ? *mp : dummy;
  const bool tracked = mp != nullptr;
  // Turning a lamp on makes the Matter stack move its level on its own: open
  // the quiet window on an "on" only (level confirmations of a slider drag
  // must not close the door on the next step).
  if (tracked && attr_id == A::kOnOff && v.as_bool()) {
    m.quiet_until_ms = now_ms() + kQuietAfterMirrorMs;
  }
  switch (attr_id) {
    case A::kOnOff: {
      if (tracked) m.onoff = v.as_bool() ? 1 : 0;
      update_attr(ep, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(v.as_bool()),
                  "OnOff", v.as_bool() ? 1 : 0);
      break;
    }
    case A::kBrightness: {
      // Matter CurrentLevel is 1..254 (0 would be out of range).
      const std::uint8_t lvl = v.as_u8() == 0 ? 1 : v.as_u8();
      if (tracked) m.level = lvl;
      update_attr(ep, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id,
                  nullable_u8(lvl), "CurrentLevel", lvl);
      break;
    }
    case A::kColorTemp: {
      if (tracked) m.ct = v.as_u16();
      update_attr(ep, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id,
                  esp_matter_uint16(v.as_u16()), "ColorTemperatureMireds", v.as_u16());
      break;
    }
    case A::kColorXy: {
      std::uint16_t x = 0, y = 0;
      v.as_xy(x, y);
      if (tracked) {
        m.x = x;
        m.y = y;
      }
      update_attr(ep, ColorControl::Id, ColorControl::Attributes::CurrentX::Id,
                  esp_matter_uint16(x), "CurrentX", x);
      update_attr(ep, ColorControl::Id, ColorControl::Attributes::CurrentY::Id,
                  esp_matter_uint16(y), "CurrentY", y);
      break;
    }
    case A::kTemperature: {
      const std::int16_t centi = U::temp_to_centi(v.as_i16());
      // A thermostat carries its temperature in its own cluster; everything
      // else in TemperatureMeasurement. Try the thermostat first.
      if (attribute::get(ep, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id) !=
          nullptr) {
        update_attr(ep, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id,
                    nullable_i16(centi), "LocalTemperature", centi);
      } else {
        update_attr(ep, TemperatureMeasurement::Id,
                    TemperatureMeasurement::Attributes::MeasuredValue::Id, nullable_i16(centi),
                    "Temperature", centi);
      }
      break;
    }
    case A::kHumidity: {
      const std::uint16_t centi = U::humidity_to_centi(v.as_u16());
      update_attr(ep, RelativeHumidityMeasurement::Id,
                  RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, nullable_u16(centi),
                  "Humidity", centi);
      break;
    }
    case A::kIlluminance: {
      const std::uint16_t m16 = U::lux_to_measured(v.as_u16());
      update_attr(ep, IlluminanceMeasurement::Id, IlluminanceMeasurement::Attributes::MeasuredValue::Id,
                  nullable_u16(m16), "Illuminance", m16);
      break;
    }
    case A::kBattery: {
      const std::uint8_t half = U::battery_to_half_percent(v.as_u8());
      update_attr(ep, PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id,
                  nullable_u8(half), "BatPercentRemaining", half);
      // 0 ok, 1 warning (<20 %), 2 critical (<10 %).
      const std::uint8_t level = v.as_u8() < 10 ? 2 : v.as_u8() < 20 ? 1 : 0;
      update_attr(ep, PowerSource::Id, PowerSource::Attributes::BatChargeLevel::Id,
                  esp_matter_enum8(level), "BatChargeLevel", level);
      break;
    }
    case A::kSetpoint: {
      if (tracked) m.setpoint = v.as_i16();
      const std::int16_t centi = U::temp_to_centi(v.as_i16());
      update_attr(ep, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id,
                  esp_matter_int16(centi), "OccupiedHeatingSetpoint", centi);
      break;
    }
    case A::kMode: {
      if (!U::mode_valid(v.as_enum8())) break;
      if (tracked) m.mode = v.as_enum8();
      update_attr(ep, Thermostat::Id, Thermostat::Attributes::SystemMode::Id,
                  esp_matter_enum8(v.as_enum8()), "SystemMode", v.as_enum8());
      break;
    }
    // BooleanState and OccupancySensing are code-driven clusters: their values
    // are not plain attributes (attribute::update is refused), the cluster
    // object has to be told.
    case A::kOccupied: {
      esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
      auto* sci = esp_matter::data_model::provider::get_instance().registry().Get(
          chip::app::ConcreteClusterPath(ep, OccupancySensing::Id));
      if (sci != nullptr) {
        static_cast<chip::app::Clusters::OccupancySensingCluster*>(sci)->SetOccupancy(v.as_bool());
      }
      ESP_LOGI(kTag, "STATE -> Matter ep=%u Occupancy=%d%s", static_cast<unsigned>(ep),
               v.as_bool() ? 1 : 0, sci ? "" : " (no cluster)");
      break;
    }
    case A::kContact: {
      esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
      auto* sci = esp_matter::data_model::provider::get_instance().registry().Get(
          chip::app::ConcreteClusterPath(ep, BooleanState::Id));
      if (sci != nullptr) {
        static_cast<chip::app::Clusters::BooleanStateCluster*>(sci)->SetStateValue(v.as_bool());
      }
      ESP_LOGI(kTag, "STATE -> Matter ep=%u Contact=%d%s", static_cast<unsigned>(ep),
               v.as_bool() ? 1 : 0, sci ? "" : " (no cluster)");
      break;
    }
    case A::kEvent: {
      // A button is an edge: the STATE carries the event code (enum8), and it
      // returns to Idle after ~0.4 s.
      emit_button_event(ep, static_cast<A::ButtonEvent>(v.as_enum8()));
      break;
    }
    case A::kPowerW: {
      const std::int64_t mw = static_cast<std::int64_t>(v.as_u16()) * 1000;
      esp_matter_attr_val_t mv{};
      mv.type = ESP_MATTER_VAL_TYPE_NULLABLE_INT64;
      mv.val.i64 = mw;
      {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        attribute::update(ep, kEpmClusterId, kEpmAttrActivePower, &mv);
      }
      ESP_LOGI(kTag, "STATE -> Matter ep=%u ActivePower=%lld mW", static_cast<unsigned>(ep),
               static_cast<long long>(mw));
      break;
    }
    case A::kEnergyWh: {
      const std::uint32_t wh = v.as_u32();
      using EMS = ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type;
      using NEMS = chip::app::DataModel::Nullable<EMS>;
      EMS imported;
      imported.energy = static_cast<std::int64_t>(wh) * 1000LL;  // Wh -> mWh
      NEMS ni, ne;
      ni.SetNonNull(imported);
      // ne stays null (no export measurement)
      // The cluster registry is touched from here: without the stack lock the
      // SDK aborts ("Chip stack locking error ... Code is unsafe/racy") on the
      // first energy_wh, e.g. 0.
      esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
      ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(ep, ni, ne);
      ESP_LOGI(kTag, "STATE -> Matter ep=%u CumulativeEnergyImported=%u Wh",
               static_cast<unsigned>(ep), static_cast<unsigned>(wh));
      break;
    }
    default:
      break;
  }
}

// The entity id carries its owner in the high byte (0x0100.. = the Zigbee
// chip), so the bridge can tell the entities apart even though the brain
// rewrites the chip id of the mirrored frames to ours.
std::uint8_t owner_chip(std::uint8_t from_chip, std::uint16_t entity_id) {
  const std::uint8_t hi = static_cast<std::uint8_t>(entity_id >> 8);
  return hi != 0 ? hi : from_chip;
}

void apply_state(std::uint8_t chip_id, const hubproto::Header& h, const std::uint8_t* payload,
                 std::size_t len) {
  keystone::BridgedAddr addr{owner_chip(chip_id, h.entity_id), h.entity_id};
  const std::uint16_t ep = app_bridge_get_endpoint(&addr);
  if (ep == chip::kInvalidEndpointId) return;
  auto rd = hubproto::state_reader(payload, len);
  if (!rd.valid) return;
  while (rd.remaining > 0 && rd.valid) {
    std::uint8_t attr_id = 0;
    if (!hubproto::state_next_attr(rd, attr_id)) break;
    hubproto::ValueType vt{};
    if (!core::zcl::attr_value_type(attr_id, vt)) {
      if (attr_id == A::kPowerW) {
        vt = hubproto::ValueType::U16;
      } else if (attr_id == A::kEnergyWh) {
        vt = hubproto::ValueType::U32;
      } else {
        break;  // unknown attr — can't skip w/o value_type
      }
    }
    hubproto::Value v{};
    if (!hubproto::state_consume(rd, vt, v)) break;
    apply_value(ep, attr_id, v);
  }
}

bool quiet(const Mirror& m) {
  return static_cast<std::int32_t>(m.quiet_until_ms - now_ms()) > 0;
}

}  // namespace

namespace matter_bridge {

app_bridged_device_t* alloc_bridged(esp_matter::node_t* /*node*/, std::uint16_t /*endpoint*/) {
  return chip::Platform::New<keystone::BridgedDevice>();
}

void free_bridged(app_bridged_device_t* d) {
  chip::Platform::Delete(static_cast<keystone::BridgedDevice*>(d));
}

void log_table() {
  std::uint16_t eps[MAX_BRIDGED_DEVICE_COUNT]{};
  esp_matter_bridge::get_bridged_endpoint_ids(eps);
  int used = 0;
  for (int i = 0; i < MAX_BRIDGED_DEVICE_COUNT; ++i) {
    if (eps[i] == chip::kInvalidEndpointId || eps[i] == 0) continue;
    ++used;
    auto* bd = static_cast<keystone::BridgedDevice*>(app_bridge_get_device(eps[i]));
    ESP_LOGI(kTag, "  bridge slot %d: endpoint %u -> %s entity 0x%04x", i, static_cast<unsigned>(eps[i]),
             bd ? "device" : "NO DEVICE", bd ? static_cast<unsigned>(bd->addr().entity_id) : 0u);
  }
  ESP_LOGI(kTag, "bridge table: %d of %d slots used", used, MAX_BRIDGED_DEVICE_COUNT);
}

// A controller changed an attribute of a bridged endpoint: forward it to the
// brain as a COMMAND (which routes it to the owning chip). Our own mirror of a
// STATE comes back through here too; the Mirror comparison drops it.
esp_err_t attribute_update_cb(esp_matter::attribute::callback_type_t type, std::uint16_t endpoint_id,
                              std::uint32_t cluster_id, std::uint32_t attribute_id,
                              esp_matter_attr_val_t* val, void* /*priv*/) {
  if (type != PRE_UPDATE) return ESP_OK;
  const bool relevant =
      (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) ||
      (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) ||
      (cluster_id == ColorControl::Id &&
       (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id ||
        attribute_id == ColorControl::Attributes::CurrentX::Id ||
        attribute_id == ColorControl::Attributes::CurrentY::Id)) ||
      (cluster_id == Thermostat::Id &&
       (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id ||
        attribute_id == Thermostat::Attributes::SystemMode::Id));
  if (!relevant) return ESP_OK;
  const bool on_off = cluster_id == OnOff::Id;
  // Level, colour and thermostat writes during endpoint construction or right
  // after boot are the stack's own initialisation (on/off has its own guard:
  // start_up_on_off is null).
  if (!on_off && (g_building || now_ms() < kBootGraceMs)) return ESP_OK;
  app_bridged_device_t* bd = app_bridge_get_device(endpoint_id);
  if (!bd) return ESP_OK;
  auto* kbd = static_cast<keystone::BridgedDevice*>(bd);
  keystone::BridgedAddr addr = kbd->addr();
  if (addr.entity_id == 0) return ESP_OK;
  Mirror dummy;
  Mirror* mp = mirror_for(endpoint_id);
  Mirror& m = mp != nullptr ? *mp : dummy;

  if (cluster_id == OnOff::Id) {
    // Echo of our own mirror of the brain's STATE — not a controller's request.
    if (m.onoff == (val->val.b ? 1 : 0)) return ESP_OK;
    send_command(addr.entity_id, A::kOnOff, hubproto::Value::make_bool(val->val.b));
  } else if (cluster_id == LevelControl::Id) {
    const std::uint8_t lvl = val->val.u8;
    if (lvl == 0xFF || m.level == lvl) return ESP_OK;  // null, or our own echo
    if (quiet(m)) return ESP_ERR_INVALID_STATE;         // the stack's own reaction
    queue_command(addr.entity_id, A::kBrightness, hubproto::Value::make_u8(lvl));
  } else if (cluster_id == ColorControl::Id) {
    const std::uint16_t v16 = val->val.u16;
    if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
      if (m.ct == v16) return ESP_OK;
      if (quiet(m)) return ESP_ERR_INVALID_STATE;
      queue_command(addr.entity_id, A::kColorTemp, hubproto::Value::make_u16(v16));
    } else {
      // X and Y arrive as separate attribute writes; a COMMAND carries the pair.
      const bool is_x = attribute_id == ColorControl::Attributes::CurrentX::Id;
      if ((is_x ? m.x : m.y) == v16) return ESP_OK;
      if (quiet(m)) return ESP_ERR_INVALID_STATE;
      const std::uint16_t other = static_cast<std::uint16_t>(
          (is_x ? m.y : m.x) < 0 ? (is_x ? 0x607d : 0x616b) : (is_x ? m.y : m.x));
      queue_command(addr.entity_id, A::kColorXy,
                    is_x ? hubproto::Value::make_xy(v16, other) : hubproto::Value::make_xy(other, v16));
    }
  } else if (cluster_id == Thermostat::Id) {
    if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
      std::int16_t deci = 0;
      if (!U::temp_from_centi(val->val.i16, deci) || m.setpoint == deci) return ESP_OK;
      queue_command(addr.entity_id, A::kSetpoint, hubproto::Value::make_i16(deci));
    } else {
      const std::uint8_t mode = val->val.u8;
      if (!U::mode_valid(mode) || m.mode == mode) return ESP_OK;
      queue_command(addr.entity_id, A::kMode, hubproto::Value::make_enum8(mode));
    }
  }
  return ESP_OK;
}

void on_bus_message(const hubproto::Header& h, const std::uint8_t* payload, std::size_t len) {
  switch (h.type) {
    case hubproto::MsgType::Declare: {
      hubproto::DeclareView view{};
      if (!hubproto::decode_declare(payload, len, view)) return;
      M::Attrs attrs;
      for (std::uint8_t i = 0; i < view.attr_count; ++i) {
        attrs.add(hubproto::declare_attr_at(view, i).attr_id);
      }
      ensure_bridged_endpoint(owner_chip(h.chip_id, view.entity_id), view.entity_id,
                              M::plan_for(view.device_type, attrs));
      return;
    }
    case hubproto::MsgType::State:
      apply_state(h.chip_id, h, payload, len);
      return;
    case hubproto::MsgType::Undeclare:
      remove_bridged_endpoint(owner_chip(h.chip_id, h.entity_id), h.entity_id);
      return;
    default:
      return;
  }
}

esp_err_t start(core::Bus* bus, esp_matter::node_t* node, std::uint16_t aggregator_ep,
                std::uint8_t boot_id) {
  g_bus = bus;
  g_node = node;
  g_aggregator_endpoint_id = aggregator_ep;
  g_boot_id = boot_id;

  // Controller changes to level/colour/thermostat are debounced before they
  // go to the brain.
  g_q_mu = xSemaphoreCreateMutex();
  if (g_q_mu == nullptr) return ESP_ERR_NO_MEM;
  xTaskCreate(forward_task, "ks_fwd", 3072, nullptr, 4, nullptr);

  // Bring bridged endpoints back from NVS. Fires create_bridged_clusters for
  // each restored endpoint. New devices from live DECLAREs go through the
  // same callback via app_bridge_create_new_device.
  g_building = true;
  const esp_err_t err = app_bridge_initialize(node, create_bridged_clusters, alloc_bridged,
                                               free_bridged);
  g_building = false;
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "app_bridge_initialize: %d", err);
    return err;
  }
  ESP_LOGI(kTag, "bridge up under aggregator endpoint %u", static_cast<unsigned>(aggregator_ep));
  log_table();
  return ESP_OK;
}

}  // namespace matter_bridge

#endif  // CHIP_HAVE_CONFIG_H
