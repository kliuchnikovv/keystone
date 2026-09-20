// Phases 3-4 (brain, ESP32-S3-Zero): the full brain core over the UART link.
//
// Wires the portable core to the transport: EntityRegistry + RadioCoordinator
// (HELLO->QUERY->DECLARE), StateMachine (applies STATE), CommandDispatcher
// (COMMAND + CMD_ACK, retries), and a RuleEngine. Rule: occupancy 0x0010 →
// drive light 0x0020. Everything except EspUartLink is hardware-agnostic.
//
// Wiring (crossed, common GND): S3.TX(GPIO7) -> C6.RX(GPIO4), S3.RX(GPIO6) <- C6.TX(GPIO5).
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_uart_link.hpp"
#include "core/uart_transport.hpp"
#include "core/entity_registry.hpp"
#include "core/radio_coordinator.hpp"
#include "core/state_machine.hpp"
#include "core/command_dispatcher.hpp"
#include "core/rule_engine.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "brain-s3";

constexpr int kUartTxGpio = 7;
constexpr int kUartRxGpio = 6;

constexpr std::uint16_t kOccupancy = 0x0010;
constexpr std::uint8_t  kOccupiedAttr = 0x50;
constexpr std::uint16_t kLight = 0x0020;
constexpr std::uint8_t  kOnOffAttr = 0x01;

using Registry = core::EntityRegistry<>;
using Dispatcher = core::CommandDispatcher<Registry>;
using Rules = core::RuleEngine<Dispatcher>;

std::uint32_t now_ms(void* /*ctx*/) {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

bool is_true(const hubproto::Value& v) { return v.as_bool(); }
bool is_false(const hubproto::Value& v) { return !v.as_bool(); }

void on_state_change(void* /*ctx*/, std::uint16_t entity_id, std::uint8_t attr_id,
                     const hubproto::Value& v) {
  ESP_LOGI(kTag, "STATE entity=0x%04x attr=0x%02x value=%u",
           static_cast<unsigned>(entity_id), static_cast<unsigned>(attr_id),
           static_cast<unsigned>(v.raw[0]));
}

void on_raw(void* /*ctx*/, const hubproto::Header& h, const std::uint8_t* /*p*/,
            std::size_t /*len*/) {
  ESP_LOGI(kTag, "rx type=0x%02x chip=%u boot=%u",
           static_cast<unsigned>(static_cast<std::uint8_t>(h.type)),
           static_cast<unsigned>(h.chip_id), static_cast<unsigned>(h.boot_id));
}

void on_cmd_result(void* /*ctx*/, std::uint16_t seq, std::uint16_t entity_id,
                   std::uint8_t attr_id, hubproto::CmdAckStatus status) {
  ESP_LOGI(kTag, "COMMAND result seq=%u entity=0x%04x attr=0x%02x status=%d",
           static_cast<unsigned>(seq), static_cast<unsigned>(entity_id),
           static_cast<unsigned>(attr_id), static_cast<int>(status));
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

  static Registry reg;
  static core::UartTransport bus(link);
  static core::RadioCoordinator<Registry> coord(bus, reg);
  static core::StateMachine<Registry> sm(bus, reg);
  static Dispatcher disp(bus, reg);
  static Rules rules(disp, &now_ms, nullptr);

  bus.subscribe(&on_raw, nullptr);  // 4th/last subscriber: raw inbound trace
  disp.on_result(&on_cmd_result, nullptr);

  // Rule: motion on -> light on; motion off -> light off.
  core::Rule on_rule{kOccupancy, kOccupiedAttr, &is_true, kLight, kOnOffAttr,
                     hubproto::Value::make_bool(true)};
  core::Rule off_rule{kOccupancy, kOccupiedAttr, &is_false, kLight, kOnOffAttr,
                      hubproto::Value::make_bool(false)};
  rules.add_rule(on_rule);
  rules.add_rule(off_rule);

  sm.on_change(&Rules::on_state_change, &rules);  // rules react to state
  sm.on_change(&on_state_change, nullptr);        // and we log it

  link.start();
  ESP_LOGI(kTag, "brain up; core online, awaiting HELLO from chips");

  bool announced = false;
  while (true) {
    disp.poll(now_ms(nullptr));  // drive command timeouts/retries
    if (!announced && coord.chip_ready(0x01)) {
      announced = true;
      ESP_LOGI(kTag, "chip 0x01 declared; registry has %u entities",
               static_cast<unsigned>(reg.size()));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
