// Brain (ESP32-S3-Zero): the full brain core over the UART link, plus a wired
// command console for interactive testing.
//
// Wires the portable core to the transport: EntityRegistry + RadioCoordinator
// (HELLO->QUERY->DECLARE), StateMachine (applies STATE), CommandDispatcher
// (COMMAND + CMD_ACK, retries), and a RuleEngine (occupancy 0x0010 -> light
// 0x0020, from the Phase 3-5 fake pipeline). Everything except EspUartLink is
// hardware-agnostic.
//
// Command console (esp_console over USB-Serial-JTAG): type commands on the
// brain's USB port to drive the home for testing —
//   home                    list every entity and its last state
//   get <id>                one entity's state
//   on|off|toggle <id>      switch command (-> CMD_ACK)
//   in <sec> <on|off> <id>  run a switch action after a delay (a timer)
//   timers | cancel <n>     list / cancel pending timers
// Console handlers only enqueue requests; the main loop executes them, so all
// core access stays single-threaded (the UART RX task is the only other writer).
//
// Wiring (crossed, common GND): S3.TX(GPIO7) -> C6.RX(GPIO4), S3.RX(GPIO6) <- C6.TX(GPIO5).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_console.h"
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

void on_cmd_result(void* /*ctx*/, std::uint16_t seq, std::uint16_t entity_id,
                   std::uint8_t attr_id, hubproto::CmdAckStatus status) {
  ESP_LOGI(kTag, "COMMAND result seq=%u entity=0x%04x attr=0x%02x status=%d",
           static_cast<unsigned>(seq), static_cast<unsigned>(entity_id),
           static_cast<unsigned>(attr_id), static_cast<int>(status));
}

// ---- Console <-> main-loop request channel --------------------------------
enum ReqKind { REQ_HOME, REQ_GET, REQ_SETONOFF, REQ_TIMER, REQ_TIMERS, REQ_CANCEL };

struct Req {
  ReqKind kind;
  std::uint16_t id;
  bool val;
  std::uint32_t delay_ms;
  int idx;
  char* out;            // response buffer owned by the caller (console task)
  std::size_t out_cap;
  SemaphoreHandle_t done;
};

QueueHandle_t g_req_q = nullptr;

const char* dt_name(hubproto::DeviceType t) {
  switch (t) {
    case hubproto::DeviceType::Light: return "light";
    case hubproto::DeviceType::Sensor: return "sensor";
    case hubproto::DeviceType::Switch: return "switch";
    case hubproto::DeviceType::Cover: return "cover";
    case hubproto::DeviceType::Lock: return "lock";
    case hubproto::DeviceType::Climate: return "climate";
    case hubproto::DeviceType::Occupancy: return "occupancy";
    case hubproto::DeviceType::Contact: return "contact";
    case hubproto::DeviceType::Button: return "button";
  }
  return "?";
}

// Format an attribute's current value into `buf` (e.g. "on_off=on", "0x02=200").
void fmt_attr(char* buf, std::size_t cap, const core::AttrState& a) {
  const char* name = a.attr_id == kOnOffAttr ? "on_off" : nullptr;
  char idbuf[8];
  if (name == nullptr) {
    std::snprintf(idbuf, sizeof(idbuf), "0x%02x", static_cast<unsigned>(a.attr_id));
    name = idbuf;
  }
  if (!a.has_value) {
    std::snprintf(buf, cap, "%s=?", name);
    return;
  }
  switch (a.value_type) {
    case hubproto::ValueType::Bool:
      std::snprintf(buf, cap, "%s=%s", name, a.value.as_bool() ? "on" : "off");
      break;
    case hubproto::ValueType::U8:
      std::snprintf(buf, cap, "%s=%u", name, static_cast<unsigned>(a.value.as_u8()));
      break;
    case hubproto::ValueType::I16:
      std::snprintf(buf, cap, "%s=%d", name, static_cast<int>(a.value.as_i16()));
      break;
    case hubproto::ValueType::U16:
      std::snprintf(buf, cap, "%s=%u", name, static_cast<unsigned>(a.value.as_u16()));
      break;
    default:
      std::snprintf(buf, cap, "%s=<%u b>", name, static_cast<unsigned>(a.value.len));
      break;
  }
}

// Submit a request to the main loop and wait for it to fill `r.out`.
bool submit(Req& r, int timeout_ms = 1500) {
  if (g_req_q == nullptr) return false;
  r.done = xSemaphoreCreateBinary();
  if (r.done == nullptr) return false;
  Req* p = &r;
  bool ok = xQueueSend(g_req_q, &p, pdMS_TO_TICKS(200)) == pdTRUE &&
            xSemaphoreTake(r.done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
  vSemaphoreDelete(r.done);
  return ok;
}

std::uint16_t parse_id(const char* s) {
  return static_cast<std::uint16_t>(std::strtol(s, nullptr, 0));
}

int cmd_home(int /*argc*/, char** /*argv*/) {
  char buf[512];
  Req r{};
  r.kind = REQ_HOME;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_get(int argc, char** argv) {
  if (argc < 2) { printf("usage: get <id>\n"); return 1; }
  char buf[128];
  Req r{};
  r.kind = REQ_GET;
  r.id = parse_id(argv[1]);
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int set_onoff(std::uint16_t id, bool on) {
  char buf[128];
  Req r{};
  r.kind = REQ_SETONOFF;
  r.id = id;
  r.val = on;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_on(int argc, char** argv) {
  if (argc < 2) { printf("usage: on <id>\n"); return 1; }
  return set_onoff(parse_id(argv[1]), true);
}
int cmd_off(int argc, char** argv) {
  if (argc < 2) { printf("usage: off <id>\n"); return 1; }
  return set_onoff(parse_id(argv[1]), false);
}
int cmd_toggle(int argc, char** argv) {
  if (argc < 2) { printf("usage: toggle <id>\n"); return 1; }
  // Toggle relative to last known state (read it first).
  char buf[64];
  Req q{};
  q.kind = REQ_GET;
  q.id = parse_id(argv[1]);
  q.out = buf;
  q.out_cap = sizeof(buf);
  bool cur_on = submit(q) && std::strstr(buf, "=on") != nullptr;
  return set_onoff(parse_id(argv[1]), !cur_on);
}

int cmd_in(int argc, char** argv) {
  if (argc < 4) { printf("usage: in <sec> <on|off> <id>\n"); return 1; }
  const long sec = std::strtol(argv[1], nullptr, 10);
  const bool on = std::strcmp(argv[2], "on") == 0;
  char buf[96];
  Req r{};
  r.kind = REQ_TIMER;
  r.id = parse_id(argv[3]);
  r.val = on;
  r.delay_ms = static_cast<std::uint32_t>(sec) * 1000u;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_timers(int /*argc*/, char** /*argv*/) {
  char buf[384];
  Req r{};
  r.kind = REQ_TIMERS;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_cancel(int argc, char** argv) {
  if (argc < 2) { printf("usage: cancel <n>\n"); return 1; }
  char buf[64];
  Req r{};
  r.kind = REQ_CANCEL;
  r.idx = static_cast<int>(std::strtol(argv[1], nullptr, 10));
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

void reg_cmd(const char* name, const char* help, esp_console_cmd_func_t fn) {
  esp_console_cmd_t c{};
  c.command = name;
  c.help = help;
  c.func = fn;
  esp_console_cmd_register(&c);
}

void start_console() {
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  rc.prompt = "home>";
  rc.max_cmdline_length = 128;
  rc.task_stack_size = 8192;
  esp_console_dev_usb_serial_jtag_config_t hw =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));

  reg_cmd("home", "list every entity and its last state", &cmd_home);
  reg_cmd("get", "get <id> - one entity's state", &cmd_get);
  reg_cmd("on", "on <id> - turn a switch on", &cmd_on);
  reg_cmd("off", "off <id> - turn a switch off", &cmd_off);
  reg_cmd("toggle", "toggle <id> - flip a switch", &cmd_toggle);
  reg_cmd("in", "in <sec> <on|off> <id> - run a switch action after a delay", &cmd_in);
  reg_cmd("timers", "list pending timers", &cmd_timers);
  reg_cmd("cancel", "cancel <n> - cancel a pending timer", &cmd_cancel);

  ESP_ERROR_CHECK(esp_console_start_repl(repl));
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

  disp.on_result(&on_cmd_result, nullptr);

  core::Rule on_rule{kOccupancy, kOccupiedAttr, &is_true, kLight, kOnOffAttr,
                     hubproto::Value::make_bool(true)};
  core::Rule off_rule{kOccupancy, kOccupiedAttr, &is_false, kLight, kOnOffAttr,
                      hubproto::Value::make_bool(false)};
  rules.add_rule(on_rule);
  rules.add_rule(off_rule);

  sm.on_change(&Rules::on_state_change, &rules);
  sm.on_change(&on_state_change, nullptr);

  link.start();

  // Pending timers (owned by this task only).
  struct Timer { bool active; std::uint32_t when_ms; std::uint16_t id; bool val; };
  static Timer timers[8] = {};

  g_req_q = xQueueCreate(8, sizeof(Req*));
  start_console();
  ESP_LOGI(kTag, "brain up; console ready (type 'help'), awaiting HELLO from chips");

  bool announced = false;
  while (true) {
    const std::uint32_t t = now_ms(nullptr);
    disp.poll(t);
    if (!announced && coord.chip_ready(0x01)) {
      announced = true;
      ESP_LOGI(kTag, "chip 0x01 declared; registry has %u entities",
               static_cast<unsigned>(reg.size()));
    }

    // Fire due timers.
    for (auto& tm : timers) {
      if (tm.active && static_cast<std::int32_t>(t - tm.when_ms) >= 0) {
        tm.active = false;
        std::uint16_t seq = 0;
        disp.send(tm.id, kOnOffAttr, hubproto::Value::make_bool(tm.val), t, seq);
        ESP_LOGI(kTag, "timer fired: entity 0x%04x on_off=%d (seq=%u)",
                 static_cast<unsigned>(tm.id), tm.val ? 1 : 0,
                 static_cast<unsigned>(seq));
      }
    }

    // Drain console requests (single-threaded core access here).
    Req* r = nullptr;
    while (xQueueReceive(g_req_q, &r, 0) == pdTRUE) {
      switch (r->kind) {
        case REQ_HOME: {
          std::size_t off = 0;
          const std::size_t n = reg.size();
          off += static_cast<std::size_t>(
              std::snprintf(r->out, r->out_cap, "%u entities:\n",
                            static_cast<unsigned>(n)));
          for (std::size_t i = 0; i < n && off < r->out_cap; ++i) {
            const auto* e = reg.entity_at(i);
            if (e == nullptr) break;
            off += static_cast<std::size_t>(std::snprintf(
                r->out + off, r->out_cap - off, "  0x%04x %s chip%u:",
                static_cast<unsigned>(e->entity_id), dt_name(e->device_type),
                static_cast<unsigned>(e->chip_id)));
            for (std::uint8_t a = 0; a < e->attr_count && off < r->out_cap; ++a) {
              char vb[32];
              fmt_attr(vb, sizeof(vb), e->attrs[a]);
              off += static_cast<std::size_t>(
                  std::snprintf(r->out + off, r->out_cap - off, " %s", vb));
            }
            off += static_cast<std::size_t>(
                std::snprintf(r->out + off, r->out_cap - off, "\n"));
          }
          break;
        }
        case REQ_GET: {
          const auto* e = reg.find(r->id);
          if (e == nullptr) {
            std::snprintf(r->out, r->out_cap, "0x%04x: not found\n",
                          static_cast<unsigned>(r->id));
          } else {
            std::size_t off = static_cast<std::size_t>(std::snprintf(
                r->out, r->out_cap, "0x%04x %s:", static_cast<unsigned>(r->id),
                dt_name(e->device_type)));
            for (std::uint8_t a = 0; a < e->attr_count && off < r->out_cap; ++a) {
              char vb[32];
              fmt_attr(vb, sizeof(vb), e->attrs[a]);
              off += static_cast<std::size_t>(
                  std::snprintf(r->out + off, r->out_cap - off, " %s", vb));
            }
            std::snprintf(r->out + off, r->out_cap - off, "\n");
          }
          break;
        }
        case REQ_SETONOFF: {
          if (reg.find(r->id) == nullptr) {
            std::snprintf(r->out, r->out_cap, "0x%04x: not found\n",
                          static_cast<unsigned>(r->id));
          } else {
            std::uint16_t seq = 0;
            bool ok = disp.send(r->id, kOnOffAttr,
                                hubproto::Value::make_bool(r->val), t, seq);
            std::snprintf(r->out, r->out_cap, "%s 0x%04x -> %s (seq=%u)\n",
                          r->val ? "on" : "off", static_cast<unsigned>(r->id),
                          ok ? "sent" : "FAILED", static_cast<unsigned>(seq));
          }
          break;
        }
        case REQ_TIMER: {
          int slot = -1;
          for (int i = 0; i < 8; ++i) {
            if (!timers[i].active) { slot = i; break; }
          }
          if (slot < 0) {
            std::snprintf(r->out, r->out_cap, "no free timer slot\n");
          } else {
            timers[slot] = {true, t + r->delay_ms, r->id, r->val};
            std::snprintf(r->out, r->out_cap,
                          "timer #%d: 0x%04x -> %s in %us\n", slot,
                          static_cast<unsigned>(r->id), r->val ? "on" : "off",
                          static_cast<unsigned>(r->delay_ms / 1000u));
          }
          break;
        }
        case REQ_TIMERS: {
          std::size_t off = 0;
          bool any = false;
          for (int i = 0; i < 8; ++i) {
            if (!timers[i].active) continue;
            any = true;
            const std::uint32_t left = (timers[i].when_ms - t) / 1000u;
            off += static_cast<std::size_t>(std::snprintf(
                r->out + off, r->out_cap - off, "#%d: 0x%04x -> %s in %us\n", i,
                static_cast<unsigned>(timers[i].id), timers[i].val ? "on" : "off",
                static_cast<unsigned>(left)));
            if (off >= r->out_cap) break;
          }
          if (!any) std::snprintf(r->out, r->out_cap, "no pending timers\n");
          break;
        }
        case REQ_CANCEL: {
          if (r->idx >= 0 && r->idx < 8 && timers[r->idx].active) {
            timers[r->idx].active = false;
            std::snprintf(r->out, r->out_cap, "cancelled timer #%d\n", r->idx);
          } else {
            std::snprintf(r->out, r->out_cap, "no timer #%d\n", r->idx);
          }
          break;
        }
      }
      xSemaphoreGive(r->done);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
