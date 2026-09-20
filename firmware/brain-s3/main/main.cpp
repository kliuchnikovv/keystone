// Phase 2 (brain, ESP32-S3-Zero): bring up the UART link to the co-processor.
//
// Listens for messages from the radio chip and logs them, and sends its own
// HEARTBEAT periodically. Uses the portable core::UartTransport over an
// EspUartLink; no protocol or framing logic lives here. Liveness is observed on
// the serial monitor (an LED indicator can be added later).
//
// Wiring (crossed, common GND): S3.TX(GPIO7) -> C6.RX, S3.RX(GPIO6) <- C6.TX.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "esp_uart_link.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "brain-s3";

constexpr int kUartTxGpio = 7;
constexpr int kUartRxGpio = 6;
constexpr std::uint8_t kPeerChipId = 0x01;  // the C6 co-processor
constexpr int kHeartbeatMs = 1000;

void on_message(void* /*ctx*/, const hubproto::Header& h,
                const std::uint8_t* /*payload*/, std::size_t len) {
  ESP_LOGI(kTag, "rx type=0x%02x chip=%u boot=%u seq=%u plen=%u",
           static_cast<unsigned>(static_cast<std::uint8_t>(h.type)),
           static_cast<unsigned>(h.chip_id), static_cast<unsigned>(h.boot_id),
           static_cast<unsigned>(h.seq), static_cast<unsigned>(len));
}

hubproto::Header heartbeat_header() {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = hubproto::MsgType::Heartbeat;
  h.chip_id = kPeerChipId;  // chip_id names the radio chip endpoint (SPEC 4.2)
  h.boot_id = 1;            // TODO(Phase 5): persist a reboot counter in NVS
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

  ESP_LOGI(kTag, "brain up; heartbeating to chip 0x%02x", kPeerChipId);

  while (true) {
    bus.publish(heartbeat_header(), nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(kHeartbeatMs));
  }
}
