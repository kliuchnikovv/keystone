// Phase 8 Stage A (SPEC §9): the Thread/Matter co-processor on the second C6.
//
// This is a bring-up scaffold: bus/transport only, no OpenThread and no Matter
// stack yet. It exists so we can wire up the second UART link between the brain
// (chip_id 0x02) and this board, watch HELLO/HEARTBEAT flow both ways, and
// prove the two-radio topology is healthy (SPEC §10 — 2.4 GHz coexistence, one
// stack per 802.15.4 radio) before layering anything on top.
//
// Wiring (crossed, common GND): C6.TX(GPIO5) -> S3.RX(GPIO9),
//                               C6.RX(GPIO4) <- S3.TX(GPIO8).
// Same GPIO4/5 on the C6 side as the Zigbee coordinator, so both boards can
// hold the same UART pin constants — only the S3-side pins (and UART port)
// change.
#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_uart_link.hpp"
#include "nvs_boot_id.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "thread-c6";

constexpr int kUartTxGpio = 5;
constexpr int kUartRxGpio = 4;
constexpr std::uint8_t kOwnChipId = 0x02;  // Zigbee is 0x01; Thread is 0x02.

core::UartTransport* g_bus = nullptr;
std::uint8_t g_boot_id = 0;  // persisted reboot counter (SPEC Phase 5)

hubproto::Header header(hubproto::MsgType type) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kOwnChipId;
  h.boot_id = g_boot_id;
  h.seq = 0;
  h.entity_id = hubproto::kSelfEntityId;
  return h;
}

void on_message(void* /*ctx*/, const hubproto::Header& h,
                const std::uint8_t* /*payload*/, std::size_t /*len*/) {
  // Stage A: no entities to declare yet. Reply to QUERY with just DECLARE_END
  // so the brain marks the chip ready and moves on.
  if (h.type == hubproto::MsgType::Query) {
    g_bus->publish(header(hubproto::MsgType::DeclareEnd), nullptr, 0);
    ESP_LOGI(kTag, "answered QUERY with DECLARE_END (no entities yet)");
  }
}

}  // namespace

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  g_boot_id = fw::next_boot_id();

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
  bus.subscribe(&on_message, nullptr);
  link.start();

  ESP_LOGI(kTag, "chip 0x%02x up (Thread/Matter co-processor, Stage A); boot_id=%u",
           kOwnChipId, static_cast<unsigned>(g_boot_id));

  // Announce ourselves to the brain and then heartbeat every 5s so a link
  // outage is obvious in the log.
  bus.publish(header(hubproto::MsgType::Hello), nullptr, 0);
  ESP_LOGI(kTag, "sent HELLO");

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    bus.publish(header(hubproto::MsgType::Heartbeat), nullptr, 0);
    ESP_LOGI(kTag, "HEARTBEAT");
  }
}
