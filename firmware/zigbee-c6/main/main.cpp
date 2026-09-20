// Phase 2 (co-processor, ESP32-C6): bring up the UART link to the brain.
//
// Sends HELLO on boot, then HEARTBEAT periodically, and logs whatever the brain
// sends back. Uses the portable core::UartTransport over an EspUartLink. The
// Zigbee coordinator and the Zigbee->DECLARE/STATE translator come in Phase 6.
// Liveness is observed on the serial monitor.
//
// Wiring (crossed, common GND): C6.TX(GPIO5) -> S3.RX, C6.RX(GPIO4) <- S3.TX.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "esp_uart_link.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "zigbee-c6";

constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x01;  // this radio chip's identity
constexpr std::uint8_t kBootId = 1;        // TODO(Phase 5): persist in NVS
constexpr int kHeartbeatMs = 1000;

void on_message(void* /*ctx*/, const hubproto::Header& h,
                const std::uint8_t* /*payload*/, std::size_t len) {
  ESP_LOGI(kTag, "rx type=0x%02x chip=%u boot=%u seq=%u plen=%u",
           static_cast<unsigned>(static_cast<std::uint8_t>(h.type)),
           static_cast<unsigned>(h.chip_id), static_cast<unsigned>(h.boot_id),
           static_cast<unsigned>(h.seq), static_cast<unsigned>(len));
}

hubproto::Header make_header(hubproto::MsgType type) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kOwnChipId;  // chip_id names this radio chip (SPEC 4.2)
  h.boot_id = kBootId;
  h.seq = 0;
  h.entity_id = hubproto::kSelfEntityId;
  return h;
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

  static core::UartTransport bus(link);  // registers itself as the link receiver
  bus.subscribe(&on_message, nullptr);
  link.start();

  bus.publish(make_header(hubproto::MsgType::Hello), nullptr, 0);
  ESP_LOGI(kTag, "chip 0x%02x up; sent HELLO", kOwnChipId);

  while (true) {
    bus.publish(make_header(hubproto::MsgType::Heartbeat), nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(kHeartbeatMs));
  }
}
