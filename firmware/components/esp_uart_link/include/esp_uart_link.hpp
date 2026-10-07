// EspUartLink: a core::ByteLink backed by the ESP-IDF UART driver.
//
// Outbound bytes go through uart_write_bytes(); inbound bytes are pumped by a
// background FreeRTOS task that reads the UART and forwards them to the
// registered receiver (the UartTransport). This is the only place the transport
// touches ESP-IDF — the core and protocol stay portable (SPEC Invariants 3 & 5).
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/uart.h"
#include "esp_err.h"

#include "core/bus.hpp"

namespace fw {

class EspUartLink final : public core::ByteLink {
 public:
  struct Config {
    uart_port_t port = UART_NUM_1;  // UART0 is the USB console; use UART1 for the link
    int tx_gpio = -1;
    int rx_gpio = -1;
    int baud = 115200;
    int rx_buffer = 1024;  // must be > 128
    int tx_buffer = 1024;
    int rx_chunk = 256;    // bytes read per uart_read_bytes() call
    int task_stack = 3072;
    unsigned task_prio = 10;
  };

  explicit EspUartLink(const Config& cfg) noexcept : cfg_(cfg) {}

  // Install and configure the UART driver. Call once before start().
  esp_err_t begin() noexcept;

  // Spawn the background RX task that pumps inbound bytes to the receiver.
  void start() noexcept;

  bool write(const std::uint8_t* data, std::size_t len) noexcept override;
  void set_receiver(ReceiveFn fn, void* ctx) noexcept override;

  // Optional periodic hook, run in the RX task at least every ~20 ms (once per
  // read timeout). Because it shares the RX task with inbound delivery, work done
  // here is serialized with message handling and needs no locking against it.
  using TickFn = void (*)(void* ctx);
  void set_tick(TickFn fn, void* ctx) noexcept {
    tick_ = fn;
    tick_ctx_ = ctx;
  }

 private:
  static void rx_task(void* arg);
  void run_rx();

  Config cfg_;
  ReceiveFn rx_ = nullptr;
  void* rx_ctx_ = nullptr;
  TickFn tick_ = nullptr;
  void* tick_ctx_ = nullptr;
};

}  // namespace fw
