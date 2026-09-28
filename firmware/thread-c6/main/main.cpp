// Matter co-processor (ESP32-C6, chip_id 0x02).
//
// The C6#2 owns the Matter fabric: it advertises one On/Off Light endpoint
// (Matter Device Type 0x0100 On/Off Light — used as our first plug endpoint
// while the legacy esp-matter API doesn't expose on_off_plugin_unit) that
// Apple Home / Google Home / Alexa can commission over BLE, then reach over
// Thread. Every On/Off write from a Matter controller becomes a
// COMMAND(entity_id=0x0100, attr=on_off) on our UART bus toward the brain,
// which routes it to the Zigbee C6 → the real IKEA plug. STATE going the
// other way is applied to the Matter attribute so subscriptions see it.
//
// UART bridge (Stage A/B behavior, preserved):
//   - chip_id 0x02, HELLO at boot, heartbeat every 5s, DECLARE_END on QUERY.
//   - We do NOT declare entities from the C6 side (Matter endpoints are its
//     own data model). Wiring:
//       C6.TX(GPIO5) -> S3.RX(GPIO9),  C6.RX(GPIO4) <- S3.TX(GPIO8).
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
constexpr std::uint8_t kOwnChipId = 0x02;    // Zigbee is 0x01; Matter/Thread is 0x02.
constexpr std::uint8_t kZigbeeChipId = 0x01; // Where the actual plug lives.

// MVP: hardcode the mapping — one Matter endpoint mirrors the first switch
// entity that the Zigbee coordinator declared (kZigbeeSwitchBase, SPEC §6.1).
constexpr std::uint16_t kMirroredEntityId = 0x0100;
constexpr std::uint8_t kAttrOnOff = 0x01;

core::UartTransport* g_bus = nullptr;
std::uint8_t g_boot_id = 0;
std::uint16_t g_matter_endpoint_id = 0;
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

// Send COMMAND(on_off) toward the Zigbee chip via the brain.
void send_command_on_off(bool on) {
  // Route via the brain: header.chip_id is our own (0x02) — the brain then
  // forwards this to the Zigbee chip (0x01) using its ChipRouter/RuleEngine.
  hubproto::Command cmd{};
  cmd.entity_id = kMirroredEntityId;
  cmd.attr_id = kAttrOnOff;
  cmd.value = hubproto::Value::make_bool(on);

  std::uint8_t buf[16];
  auto r = hubproto::encode_command(cmd, buf, sizeof(buf));
  if (!r.ok) return;
  g_bus->publish(header(hubproto::MsgType::Command, kMirroredEntityId), buf, r.len);
  ESP_LOGI(kTag, "COMMAND -> entity=0x%04x on_off=%u",
           kMirroredEntityId, static_cast<unsigned>(on ? 1 : 0));
}

// Apply a STATE(on_off) coming up from the brain to our Matter attribute.
void apply_state_on_off_to_matter(bool on) {
  esp_matter_attr_val_t val = esp_matter_bool(on);
  attribute::update(g_matter_endpoint_id, OnOff::Id,
                    OnOff::Attributes::OnOff::Id, &val);
  ESP_LOGI(kTag, "STATE -> Matter endpoint=%u OnOff=%u",
           static_cast<unsigned>(g_matter_endpoint_id),
           static_cast<unsigned>(on));
}

void on_bus_message(void* /*ctx*/, const hubproto::Header& h,
                    const std::uint8_t* payload, std::size_t len) {
  if (h.type == hubproto::MsgType::Query) {
    g_bus->publish(header(hubproto::MsgType::DeclareEnd), nullptr, 0);
    ESP_LOGI(kTag, "answered QUERY with DECLARE_END (Matter endpoints live on C6, not the registry)");
    return;
  }
  if (h.type == hubproto::MsgType::State && h.entity_id == kMirroredEntityId) {
    // Pull-parse the STATE payload and look for our on_off attribute.
    auto rd = hubproto::state_reader(payload, len);
    if (!rd.valid) return;
    while (rd.remaining > 0 && rd.valid) {
      std::uint8_t attr_id = 0;
      if (!hubproto::state_next_attr(rd, attr_id)) break;
      if (attr_id == kAttrOnOff) {
        hubproto::Value v{};
        if (hubproto::state_consume(rd, hubproto::ValueType::Bool, v)) {
          apply_state_on_off_to_matter(v.as_bool());
        }
        return;
      }
      // Unknown attr for us — we can't skip without knowing its value_type.
      break;
    }
  }
}

esp_err_t attribute_update_cb(callback_type_t type, uint16_t endpoint_id,
                              uint32_t cluster_id, uint32_t attribute_id,
                              esp_matter_attr_val_t* val, void* /*priv*/) {
  if (type != PRE_UPDATE) return ESP_OK;
  if (endpoint_id != g_matter_endpoint_id) return ESP_OK;
  if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
    send_command_on_off(val->val.b);
  }
  return ESP_OK;
}

esp_err_t identification_cb(identification::callback_type_t /*type*/,
                            uint16_t /*endpoint_id*/, uint8_t /*effect_id*/,
                            uint8_t /*effect_variant*/, void* /*priv*/) {
  return ESP_OK;
}

void event_cb(const ChipDeviceEvent* event, intptr_t /*arg*/) {
  switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
      ESP_LOGI(kTag, "Matter: commissioning complete");
      break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
      ESP_LOGI(kTag, "Matter: commissioning window opened");
      break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
      ESP_LOGI(kTag, "Matter: interface IP address changed");
      break;
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

  ESP_LOGI(kTag, "chip 0x%02x up (Matter co-processor); boot_id=%u",
           kOwnChipId, static_cast<unsigned>(g_boot_id));
  bus.publish(header(hubproto::MsgType::Hello), nullptr, 0);

  // --- Matter data model ---
  node::config_t node_config;
  node_t* node = node::create(&node_config, attribute_update_cb, identification_cb);
  if (!node) {
    ESP_LOGE(kTag, "node::create failed");
    return;
  }

  on_off_light::config_t light_config;
  light_config.on_off.on_off = false;
  endpoint_t* endpoint = on_off_light::create(node, &light_config,
                                              ENDPOINT_FLAG_NONE, nullptr);
  if (!endpoint) {
    ESP_LOGE(kTag, "on_off_light::create failed");
    return;
  }
  g_matter_endpoint_id = endpoint::get_id(endpoint);
  ESP_LOGI(kTag, "Matter On/Off endpoint id=%u", static_cast<unsigned>(g_matter_endpoint_id));

  // OpenThread platform config (esp-matter drives the stack). The
  // ESP_OPENTHREAD_DEFAULT_* macros only live in the light example's app_priv.h,
  // so inline the same values here.
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

  // Heartbeat to the brain.
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    bus.publish(header(hubproto::MsgType::Heartbeat), nullptr, 0);
  }
}
