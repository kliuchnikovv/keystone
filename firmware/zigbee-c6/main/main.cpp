// Phases 3-4 (co-processor, ESP32-C6): a FAKE device driving the brain.
//
// Stands in for a real radio chip (SPEC Phase 3): HELLO until queried, then
// declares two entities (occupancy 0x0010, light 0x0020), periodically reports
// occupancy STATE (simulated motion), and ACKs COMMANDs to the light. The real
// esp-zigbee-sdk coordinator + translator replace this in Phase 6.
//
// Wiring (crossed, common GND): C6.TX(GPIO5) -> S3.RX(GPIO6), C6.RX(GPIO4) <- S3.TX(GPIO7).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "esp_uart_link.hpp"
#include "nvs_boot_id.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "zigbee-c6";

constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x01;
std::uint8_t g_boot_id = 0;  // set from NVS at boot (SPEC Phase 5)

constexpr std::uint16_t kOccupancy = 0x0010;
constexpr std::uint8_t  kOccupiedAttr = 0x50;
constexpr std::uint16_t kLight = 0x0020;
constexpr std::uint8_t  kOnOffAttr = 0x01;

core::UartTransport* g_bus = nullptr;
bool g_declared = false;
bool g_occupied = false;
bool g_light_on = false;

hubproto::Header header(hubproto::MsgType type, std::uint16_t entity_id,
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

void declare_entity(std::uint16_t entity_id, hubproto::DeviceType dt,
                    std::uint8_t attr_id, std::uint8_t flags) {
  const hubproto::AttrDecl attrs[1] = {{attr_id, hubproto::ValueType::Bool, flags}};
  std::uint8_t buf[16];
  auto r = hubproto::encode_declare(entity_id, dt, attrs, 1, buf, sizeof(buf));
  if (r.ok) g_bus->publish(header(hubproto::MsgType::Declare, entity_id, 0), buf, r.len);
}

void send_declarations() {
  const std::uint8_t rd = hubproto::kFlagReadable | hubproto::kFlagReportable;
  declare_entity(kOccupancy, hubproto::DeviceType::Occupancy, kOccupiedAttr, rd);
  declare_entity(kLight, hubproto::DeviceType::Light, kOnOffAttr,
                 rd | hubproto::kFlagWritable);
  g_bus->publish(header(hubproto::MsgType::DeclareEnd, 0x0000, 0), nullptr, 0);
  g_declared = true;
  ESP_LOGI(kTag, "declared occupancy 0x%04x and light 0x%04x",
           static_cast<unsigned>(kOccupancy), static_cast<unsigned>(kLight));
}

void handle_command(const hubproto::Header& h, const std::uint8_t* payload,
                    std::size_t len) {
  hubproto::Command cmd{};
  if (!hubproto::decode_command(payload, len, hubproto::ValueType::Bool, cmd)) return;
  g_light_on = cmd.value.as_bool();
  ESP_LOGI(kTag, "COMMAND light 0x%04x on_off=%d -> ACK",
           static_cast<unsigned>(cmd.entity_id), g_light_on ? 1 : 0);
  std::uint8_t ack[1];
  hubproto::encode_cmd_ack(hubproto::CmdAckStatus::Ok, ack, sizeof(ack));
  g_bus->publish(header(hubproto::MsgType::CmdAck, cmd.entity_id, h.seq), ack, 1);
}

void on_message(void* /*ctx*/, const hubproto::Header& h,
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

}  // namespace

extern "C" void app_main(void) {
  static fw::EspUartLink link([] {
    fw::EspUartLink::Config c;
    c.port = UART_NUM_1;
    c.tx_gpio = kUartTxGpio;
    c.rx_gpio = kUartRxGpio;
    return c;
  }());
  ESP_ERROR_CHECK(link.begin());

  g_boot_id = fw::next_boot_id();  // persisted reboot counter (SPEC Phase 5)

  static core::UartTransport bus(link);
  g_bus = &bus;
  bus.subscribe(&on_message, nullptr);
  link.start();

  ESP_LOGI(kTag, "chip 0x%02x up; boot_id=%u", kOwnChipId,
           static_cast<unsigned>(g_boot_id));

  while (true) {
    if (!g_declared) {
      bus.publish(header(hubproto::MsgType::Hello, 0x0000, 0), nullptr, 0);
      ESP_LOGI(kTag, "sent HELLO (awaiting QUERY)");
    } else {
      g_occupied = !g_occupied;  // simulate motion toggling
      const hubproto::StateEntry e[1] = {
          {kOccupiedAttr, hubproto::Value::make_bool(g_occupied)}};
      std::uint8_t buf[16];
      auto r = hubproto::encode_state(kOccupancy, e, 1, buf, sizeof(buf));
      if (r.ok) bus.publish(header(hubproto::MsgType::State, kOccupancy, 0), buf, r.len);
      ESP_LOGI(kTag, "STATE occupancy=%d", g_occupied ? 1 : 0);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
