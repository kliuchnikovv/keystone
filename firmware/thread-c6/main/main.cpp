// Matter Bridge co-processor (ESP32-C6, chip_id 0x02).
//
// C6#2 is a Matter Bridge (Device Type 0x000E Aggregator) over Thread. Every
// entity the brain announces (currently: Zigbee switches on chip 0x01) shows
// up in Apple/Google/Alexa as its own bridged Matter accessory. Mapping is
// per-entity (BridgedAddr = {chip_id, entity_id}) and persisted in NVS by
// esp-matter's app_bridge machinery, so endpoint ids survive reboots and
// Apple Home doesn't lose devices.
//
// Data flow:
//   - DECLARE(entity_id, device_type) from the brain → allocate a bridged
//     endpoint (on_off_light cluster for a Switch DeviceType) if we don't
//     already have one, remember the mapping.
//   - STATE from the brain → find the endpoint by entity_id, attribute::update().
//   - Matter attribute PRE_UPDATE from a controller → look up the entity
//     behind the endpoint, send COMMAND to the brain, which forwards it to
//     the owning chip (routing done in brain-s3::on_bus_forward).
//
// UART bridge: chip 0x02, HELLO on boot, heartbeat every 5s, DECLARE_END on
// QUERY (Matter endpoints live on the C6, not in the brain's registry).
//
// Wiring: C6.TX(GPIO5) -> S3.RX(GPIO9), C6.RX(GPIO4) <- S3.TX(GPIO8).
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_matter.h"
#include "esp_matter_ota.h"
#include "esp_openthread.h"
#include "esp_openthread_types.h"
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/OpenthreadLauncher.h>

#include <app_bridged_device.h>
#include "keystone_bridged_device.hpp"

#include "esp_uart_link.hpp"
#include "nvs_boot_id.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

namespace {

const char* kTag = "matter-c6";

constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x02;
constexpr std::uint8_t kAttrOnOff = 0x01;

core::UartTransport* g_bus = nullptr;
std::uint8_t g_boot_id = 0;
node_t* g_node = nullptr;
std::uint16_t g_aggregator_endpoint_id = 0;
std::uint16_t g_next_seq = 1;

hubproto::Header header(hubproto::MsgType type,
                        std::uint16_t entity = hubproto::kSelfEntityId,
                        std::uint16_t seq = 0) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kOwnChipId;
  h.boot_id = g_boot_id;
  h.seq = seq ? seq : g_next_seq++;
  h.entity_id = entity;
  return h;
}

// Map a keystone DeviceType to a Matter Device Type ID. Extend as new device
// types get first-class Matter representations (SPEC §6.1 mapping table).
std::uint32_t map_device_type(hubproto::DeviceType dt) {
  switch (dt) {
    case hubproto::DeviceType::Switch:
    case hubproto::DeviceType::Light:
      return ESP_MATTER_ON_OFF_LIGHT_DEVICE_TYPE_ID;
    default:
      return 0;  // unsupported: skip
  }
}

// Send COMMAND(on_off) for a bridged entity toward the brain.
void send_command_on_off(std::uint16_t entity_id, bool on) {
  hubproto::Command cmd{};
  cmd.entity_id = entity_id;
  cmd.attr_id = kAttrOnOff;
  cmd.value = hubproto::Value::make_bool(on);
  std::uint8_t buf[16];
  auto r = hubproto::encode_command(cmd, buf, sizeof(buf));
  if (!r.ok) return;
  g_bus->publish(header(hubproto::MsgType::Command, entity_id), buf, r.len);
  ESP_LOGI(kTag, "COMMAND -> entity=0x%04x on_off=%u", entity_id,
           static_cast<unsigned>(on ? 1 : 0));
}

// Bridged endpoint creation callback: populate the newly created endpoint
// (either freshly created via app_bridge_create_new_device, or restored from
// NVS at boot) with the cluster set for its device type.
esp_err_t create_bridged_clusters(esp_matter::endpoint_t* ep,
                                  std::uint32_t matter_dt_id,
                                  void* /*priv*/) {
  if (matter_dt_id == ESP_MATTER_ON_OFF_LIGHT_DEVICE_TYPE_ID) {
    on_off_light::config_t conf;
    return on_off_light::add(ep, &conf);
  }
  return ESP_ERR_INVALID_ARG;
}

app_bridged_device_t* alloc_bridged(node_t* /*node*/, std::uint16_t /*endpoint*/) {
  return chip::Platform::New<keystone::BridgedDevice>();
}
void free_bridged(app_bridged_device_t* d) {
  chip::Platform::Delete(static_cast<keystone::BridgedDevice*>(d));
}

// Look up (or create) the Matter endpoint that mirrors a given hubproto entity.
std::uint16_t ensure_bridged_endpoint(std::uint8_t chip_id, std::uint16_t entity_id,
                                      hubproto::DeviceType dt) {
  keystone::BridgedAddr addr{chip_id, entity_id};
  std::uint16_t ep = app_bridge_get_endpoint(&addr);
  if (ep != chip::kInvalidEndpointId) return ep;
  const std::uint32_t matter_dt = map_device_type(dt);
  if (matter_dt == 0) return chip::kInvalidEndpointId;
  app_bridged_device_t* bd = nullptr;
  esp_err_t err = app_bridge_create_new_device(g_node, g_aggregator_endpoint_id,
                                               matter_dt, &addr, &bd);
  if (err != ESP_OK || !bd) {
    ESP_LOGE(kTag, "app_bridge_create_new_device failed for entity 0x%04x: %d",
             entity_id, err);
    return chip::kInvalidEndpointId;
  }
  const std::uint16_t new_ep =
      esp_matter::endpoint::get_id(bd->get_matter_device()->endpoint);
  ESP_LOGI(kTag, "bridged: entity 0x%04x (chip 0x%02x) -> Matter endpoint %u",
           entity_id, chip_id, static_cast<unsigned>(new_ep));
  return new_ep;
}

// STATE from the brain — apply to the mirrored Matter attribute.
void apply_state(std::uint8_t chip_id, const hubproto::Header& h,
                 const std::uint8_t* payload, std::size_t len) {
  keystone::BridgedAddr addr{chip_id, h.entity_id};
  const std::uint16_t ep = app_bridge_get_endpoint(&addr);
  if (ep == chip::kInvalidEndpointId) return;
  auto rd = hubproto::state_reader(payload, len);
  if (!rd.valid) return;
  while (rd.remaining > 0 && rd.valid) {
    std::uint8_t attr_id = 0;
    if (!hubproto::state_next_attr(rd, attr_id)) break;
    if (attr_id != kAttrOnOff) break;  // unknown attr — can't skip w/o value_type
    hubproto::Value v{};
    if (!hubproto::state_consume(rd, hubproto::ValueType::Bool, v)) break;
    esp_matter_attr_val_t mv = esp_matter_bool(v.as_bool());
    attribute::update(ep, OnOff::Id, OnOff::Attributes::OnOff::Id, &mv);
    ESP_LOGI(kTag, "STATE -> Matter ep=%u OnOff=%u", static_cast<unsigned>(ep),
             static_cast<unsigned>(v.as_bool() ? 1 : 0));
    return;
  }
}

void on_bus_message(void* /*ctx*/, const hubproto::Header& h,
                    const std::uint8_t* payload, std::size_t len) {
  switch (h.type) {
    case hubproto::MsgType::Query:
      g_bus->publish(header(hubproto::MsgType::DeclareEnd), nullptr, 0);
      return;
    case hubproto::MsgType::Declare: {
      hubproto::DeclareView view{};
      if (!hubproto::decode_declare(payload, len, view)) return;
      ensure_bridged_endpoint(h.chip_id, view.entity_id, view.device_type);
      return;
    }
    case hubproto::MsgType::State:
      apply_state(h.chip_id, h, payload, len);
      return;
    default:
      return;
  }
}

esp_err_t attribute_update_cb(callback_type_t type, uint16_t endpoint_id,
                              uint32_t cluster_id, uint32_t attribute_id,
                              esp_matter_attr_val_t* val, void* /*priv*/) {
  if (type != PRE_UPDATE) return ESP_OK;
  if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id) return ESP_OK;
  app_bridged_device_t* bd = app_bridge_get_device(endpoint_id);
  if (!bd) return ESP_OK;
  auto* kbd = static_cast<keystone::BridgedDevice*>(bd);
  keystone::BridgedAddr addr = kbd->addr();
  if (addr.entity_id == 0) return ESP_OK;
  send_command_on_off(addr.entity_id, val->val.b);
  return ESP_OK;
}

esp_err_t identification_cb(identification::callback_type_t, uint16_t, uint8_t,
                            uint8_t, void*) {
  return ESP_OK;
}

void event_cb(const ChipDeviceEvent* event, intptr_t) {
  switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
      ESP_LOGI(kTag, "Matter: commissioning complete");
      break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
      ESP_LOGI(kTag, "Matter: commissioning window opened");
      break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
      // Reopen the commissioning window so the user can re-add us without
      // reflashing (SPEC-friendly behavior; light example does the same).
      if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        auto& mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
        if (!mgr.IsCommissioningWindowOpen()) {
          mgr.OpenBasicCommissioningWindow(
              chip::System::Clock::Seconds16(300),
              chip::CommissioningWindowAdvertisement::kDnssdOnly);
        }
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace

extern "C" void app_main() {
  ESP_ERROR_CHECK(nvs_flash_init());
  g_boot_id = fw::next_boot_id();

  // --- UART bridge to the brain ---
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
  bus.subscribe(&on_bus_message, nullptr);
  link.start();

  ESP_LOGI(kTag, "chip 0x%02x up (Matter Bridge); boot_id=%u", kOwnChipId,
           static_cast<unsigned>(g_boot_id));
  bus.publish(header(hubproto::MsgType::Hello), nullptr, 0);

  // --- Matter data model: Root + Aggregator + dynamic bridged endpoints ---
  node::config_t node_config;
  g_node = node::create(&node_config, attribute_update_cb, identification_cb);
  if (!g_node) { ESP_LOGE(kTag, "node::create failed"); return; }

  aggregator::config_t agg_conf;
  endpoint_t* agg = aggregator::create(g_node, &agg_conf, ENDPOINT_FLAG_NONE, nullptr);
  if (!agg) { ESP_LOGE(kTag, "aggregator::create failed"); return; }
  g_aggregator_endpoint_id = esp_matter::endpoint::get_id(agg);
  ESP_LOGI(kTag, "Aggregator endpoint id=%u",
           static_cast<unsigned>(g_aggregator_endpoint_id));

  // OpenThread platform config.
  esp_openthread_platform_config_t ot_config{};
  ot_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
  ot_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
  ot_config.port_config.storage_partition_name = "nvs";
  ot_config.port_config.netif_queue_size = 10;
  ot_config.port_config.task_queue_size = 10;
  set_openthread_platform_config(&ot_config);

  esp_err_t err = esp_matter::start(event_cb);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_matter::start failed: %d", static_cast<int>(err));
    return;
  }

  // Bring bridged endpoints back from NVS. Fires create_bridged_clusters for
  // each restored endpoint. New devices from live DECLAREs go through the
  // same callback via app_bridge_create_new_device.
  err = app_bridge_initialize(g_node, create_bridged_clusters, alloc_bridged,
                              free_bridged);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "app_bridge_initialize: %d", err);
  }

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    bus.publish(header(hubproto::MsgType::Heartbeat), nullptr, 0);
  }
}
