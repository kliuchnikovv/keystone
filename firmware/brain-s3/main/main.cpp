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
//   at <HH:MM> <on|off|toggle> <id> [days]   recurring local-time action (schedules, unschedule)
// Console handlers only enqueue requests; the main loop executes them, so all
// core access stays single-threaded (the UART RX task is the only other writer).
//
// Wiring (crossed, common GND — SPEC §10 + docs/hardware-wiring.md):
//   UART1 to Zigbee C6 (chip 0x01): S3.TX(GPIO7)  -> C6.RX(GPIO4),
//                                   S3.RX(GPIO6) <- C6.TX(GPIO5).
//   UART2 to Thread C6  (chip 0x02): S3.TX(GPIO8)  -> C6.RX(GPIO4),
//                                    S3.RX(GPIO9) <- C6.TX(GPIO5).
//
// The two UARTs are two independent core::UartTransports; a small ChipRouter in
// this file glues them behind a single Bus so all core components (registry,
// coordinator, state machine, dispatcher, rules) still see just one bus and
// don't care where a message came from. Outbound is routed by header.chip_id.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mdns.h"

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
#include "core/chip_monitor.hpp"
#include "core/command_dispatcher.hpp"
#include "core/reconciler.hpp"
#include "core/ota_verifier.hpp"
#include "core/rule_engine.hpp"
#include "core/timer_table.hpp"
#include "core/schedule_table.hpp"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "app_server.hpp"
#include "core/app_protocol.hpp"
#include "core/app_requests.hpp"
#include "ota_job.hpp"

// Wi-Fi credentials live in main/wifi_secrets.h (gitignored; see the .example.h).
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define KEYSTONE_WIFI_SSID ""
#define KEYSTONE_WIFI_PASSWORD ""
#endif
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace {

const char* kTag = "brain-s3";

static bool s_sntp_initialized = false;
static bool s_time_synced = false;
static bool s_clock_just_synced = false;

static void time_sync_notification_cb(struct timeval *tv) {
  s_time_synced = true;
  s_clock_just_synced = true;
  time_t now = tv->tv_sec;
  struct tm timeinfo{};
  localtime_r(&now, &timeinfo);
  char strftime_buf[64];
  strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  ESP_LOGI(kTag, "Time synchronized via SNTP: %s", strftime_buf);
}

static void sntp_init(void) {
  if (s_sntp_initialized) return;
  setenv("TZ", "UTC", 0);
  tzset();

  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  config.sync_cb = time_sync_notification_cb;
  esp_err_t err = esp_netif_sntp_init(&config);
  if (err == ESP_OK) {
    s_sntp_initialized = true;
    ESP_LOGI(kTag, "SNTP initialized with pool.ntp.org");
  } else {
    ESP_LOGW(kTag, "SNTP init failed: 0x%x", err);
  }
}

// Name the hub answers to on the LAN: keystone.local, and the service _keystone._tcp that
// the app browses for (docs/app-protocol.md §1).
static constexpr const char* kHubHostname = "keystone";

// Обработчик событий WiFi
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(kTag, "WiFi STA Start. Connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(kTag, "WiFi disconnected. Reason: %d. Retrying connection...", disconn->reason);
        esp_wifi_connect(); // Попытка переподключения
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(kTag, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        sntp_init();
    }
}

// Функция для инициализации WiFi
static void wifi_init(void) {
    // Инициализация NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Инициализация TCP/IP стека
    ESP_ERROR_CHECK(esp_netif_init());
    // Создание default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Создание default Wi-Fi station
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, kHubHostname);

    // Конфигурация Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Регистрация обработчика событий WiFi
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Настройка Wi-Fi в режиме Station
    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, KEYSTONE_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, KEYSTONE_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(kTag, "Wi-Fi initialization complete. Attempting to connect to %s", wifi_config.sta.ssid);
}

// UART1 = Zigbee coordinator (chip 0x01).
constexpr int kZbTxGpio = 7;
constexpr int kZbRxGpio = 6;
// UART2 = Thread border router / Matter (chip 0x02).
constexpr int kThTxGpio = 8;
constexpr int kThRxGpio = 9;

constexpr std::uint8_t kZigbeeChipId = 0x01;
constexpr std::uint8_t kThreadChipId = 0x02;

constexpr std::uint8_t  kOnOffAttr = 0x01;

using Registry = core::EntityRegistry<>;
void app_on_state(void* ctx, std::uint16_t entity, std::uint8_t attr, const hubproto::Value& v);
void app_on_cmd_result(std::uint16_t seq, hubproto::CmdAckStatus status);
using Dispatcher = core::CommandDispatcher<Registry>;
using Rules = core::RuleEngine<Dispatcher>;
using Reconciler = core::Reconciler<Dispatcher>;

// Multi-UART glue: presents itself to the core as a single core::Bus, routes
// outgoing publish() to the underlying UartTransport whose bound chip_id
// matches header.chip_id, and fans every inbound message from any underlying
// transport out to all subscribers. Kept private to this app for now; if a
// second app ever needs it we'll move it into core with host tests.
class ChipRouter final : public core::Bus {
 public:
  // Bind an underlying bus to a chip_id. Subscribes internally so its inbound
  // messages get forwarded to whoever subscribed to this router.
  bool bind(std::uint8_t chip_id, core::Bus& bus) noexcept {
    if (n_binds_ >= kMaxBinds) return false;
    binds_[n_binds_] = {chip_id, &bus};
    ++n_binds_;
    return bus.subscribe(&ChipRouter::on_child_msg, this);
  }

  bool publish(const hubproto::Header& h, const std::uint8_t* payload,
               std::size_t len) noexcept override {
    // UartTransport::publish encodes into a shared buffer, so concurrent
    // publishers (main loop, UART RX tasks forwarding, the OTA task) must not
    // interleave. The router is the single funnel, so lock here.
    xSemaphoreTakeRecursive(pub_mtx_, portMAX_DELAY);
    bool ok = false;
    for (int i = 0; i < n_binds_; ++i) {
      if (binds_[i].chip_id == h.chip_id) { ok = binds_[i].bus->publish(h, payload, len); break; }
    }
    xSemaphoreGiveRecursive(pub_mtx_);
    return ok;  // false for an unknown chip: nowhere to send
  }

  bool subscribe(MessageFn fn, void* ctx) noexcept override {
    if (n_subs_ >= kMaxSubs) return false;
    subs_[n_subs_] = {fn, ctx};
    ++n_subs_;
    return true;
  }

 private:
  static constexpr int kMaxBinds = 4;
  static constexpr int kMaxSubs = 8;

  struct Bind { std::uint8_t chip_id; core::Bus* bus; };
  struct Sub { MessageFn fn; void* ctx; };

  static void on_child_msg(void* self, const hubproto::Header& h,
                           const std::uint8_t* p, std::size_t len) noexcept {
    static_cast<ChipRouter*>(self)->fan_out(h, p, len);
  }
  void fan_out(const hubproto::Header& h, const std::uint8_t* p,
               std::size_t len) noexcept {
    for (int i = 0; i < n_subs_; ++i) subs_[i].fn(subs_[i].ctx, h, p, len);
  }

  SemaphoreHandle_t pub_mtx_ = xSemaphoreCreateRecursiveMutex();
  Bind binds_[kMaxBinds] = {};
  int n_binds_ = 0;
  Sub subs_[kMaxSubs] = {};
  int n_subs_ = 0;
};

std::uint32_t now_ms(void* /*ctx*/) {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

// Background events are noisy, so they're at DEBUG level by default (off).
// Toggle with the `log` console command. Command results and one-shot events
// (a chip declared, a chip lost) stay at INFO so they aren't drowned out.
void on_state_change(void* /*ctx*/, std::uint16_t entity_id, std::uint8_t attr_id,
                     const hubproto::Value& v) {
  app_on_state(nullptr, entity_id, attr_id, v);
  ESP_LOGD(kTag, "STATE entity=0x%04x attr=0x%02x value=%u",
           static_cast<unsigned>(entity_id), static_cast<unsigned>(attr_id),
           static_cast<unsigned>(v.raw[0]));
}

// Silent frame counter per chip — feeds the `chips` console command so we can
// see traffic without polluting the log. Indexed by chip_id; small enough to
// leave static.
struct ChipStats {
  std::uint32_t rx_frames;
  std::uint32_t last_rx_ms;
};
ChipStats g_chip_stats[16] = {};

core::ChipMonitor* g_monitor = nullptr;  // set in app_main

const char* chip_label(std::uint8_t id) {
  return id == 1 ? "zigbee" : id == 2 ? "thread" : "?";
}

void on_chip_event(void* /*ctx*/, std::uint8_t chip, core::ChipMonitor::Event ev,
                   std::uint32_t value) {
  switch (ev) {
    case core::ChipMonitor::Event::Lost:
      ESP_LOGW(kTag, "chip 0x%02x (%s) LOST: silent for %lu s — commands to it fail fast",
               chip, chip_label(chip), static_cast<unsigned long>(value / 1000));
      break;
    case core::ChipMonitor::Event::Resumed:
      ESP_LOGI(kTag, "chip 0x%02x (%s) is back after %lu s", chip, chip_label(chip),
               static_cast<unsigned long>(value / 1000));
      break;
    case core::ChipMonitor::Event::Rebooted:
      ESP_LOGI(kTag, "chip 0x%02x (%s) rebooted (reboot #%lu seen)", chip,
               chip_label(chip), static_cast<unsigned long>(value));
      break;
    case core::ChipMonitor::Event::Flapping:
      ESP_LOGE(kTag, "chip 0x%02x (%s) is FLAPPING: %lu reboots in 5 min "
               "(crash loop, or the bootloader keeps reverting a bad OTA image)",
               chip, chip_label(chip), static_cast<unsigned long>(value));
      break;
  }
}

void on_bus_traffic(void* /*ctx*/, const hubproto::Header& h,
                    const std::uint8_t* /*p*/, std::size_t /*len*/) {
  if (h.chip_id < 16) {
    g_chip_stats[h.chip_id].rx_frames++;
    g_chip_stats[h.chip_id].last_rx_ms =
        static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
  }
  ESP_LOGD(kTag, "rx type=0x%02x chip=%u seq=%u",
           static_cast<unsigned>(static_cast<std::uint8_t>(h.type)),
           static_cast<unsigned>(h.chip_id), static_cast<unsigned>(h.seq));
}

void on_cmd_result(void* /*ctx*/, std::uint16_t seq, std::uint16_t entity_id,
                   std::uint8_t attr_id, hubproto::CmdAckStatus status) {
  ESP_LOGI(kTag, "COMMAND result seq=%u entity=0x%04x attr=0x%02x status=%d",
           static_cast<unsigned>(seq), static_cast<unsigned>(entity_id),
           static_cast<unsigned>(attr_id), static_cast<int>(status));
  app_on_cmd_result(seq, status);
}

// Desired state (docs/backlog.md § Recovery). Reconciler is touched from the main
// loop and from the UART RX tasks, so every entry point takes this mutex.
const char* attr_name(std::uint8_t id);
void fmt_value(char* buf, std::size_t cap, hubproto::ValueType vt, const hubproto::Value& v);

Reconciler* g_rec = nullptr;
Registry* g_reg = nullptr;
SemaphoreHandle_t g_rec_mu = nullptr;

void rec_note_command(void* /*ctx*/, std::uint16_t entity, std::uint8_t attr,
                      const hubproto::Value& v) {
  xSemaphoreTakeRecursive(g_rec_mu, portMAX_DELAY);
  g_rec->note_command(entity, attr, v);
  xSemaphoreGiveRecursive(g_rec_mu);
}

void rec_on_report(void* /*ctx*/, std::uint16_t entity, std::uint8_t attr,
                   const hubproto::Value& v, bool had, bool changed) {
  xSemaphoreTakeRecursive(g_rec_mu, portMAX_DELAY);
  g_rec->on_report(entity, attr, v, had, changed);
  xSemaphoreGiveRecursive(g_rec_mu);
}

void on_reconcile(void* /*ctx*/, Reconciler::Kind kind, std::uint16_t entity,
                  std::uint8_t attr, const hubproto::Value& actual,
                  const hubproto::Value& desired) {
  char a[24], d[24];
  fmt_value(a, sizeof(a), actual.type, actual);
  fmt_value(d, sizeof(d), desired.type, desired);
  const char* an = attr_name(attr);
  if (kind == Reconciler::Kind::Corrected) {
    ESP_LOGW(kTag, "reconcile: 0x%04x %s is %s but the hub last set %s -> corrective COMMAND sent",
             static_cast<unsigned>(entity), an ? an : "attr", a, d);
  } else {
    ESP_LOGI(kTag, "reconcile: 0x%04x %s changed to %s outside the hub (was %s) -> adopted",
             static_cast<unsigned>(entity), an ? an : "attr", a, d);
  }
}

// Cross-chip forwarding (Phase 8 / matter-backlog step 1): the Matter
// co-processor (chip 0x02) exposes endpoints that mirror registry entities
// living on the Zigbee coordinator (chip 0x01). We re-emit:
//   - STATE from Zigbee → mirrored to Matter, so Matter subscriptions in
//     Apple/Google/Alexa see attribute changes (including plug toggled
//     by hand).
//   - COMMAND from Matter → forwarded to Zigbee, so an Apple Home write
//     reaches the real device.
// ChipRouter fan_out fires only for messages coming IN over a UART, not for
// our own publish() calls, so no loop.
void on_bus_forward(void* ctx, const hubproto::Header& h,
                    const std::uint8_t* p, std::size_t len) {
  core::Bus* bus = static_cast<core::Bus*>(ctx);
  // Zigbee -> Matter mirror: DECLARE (so Matter can create a bridged
  // endpoint), STATE (so it can update the Matter attribute). No need to
  // mirror DECLARE_END — Matter Bridge builds endpoints incrementally as
  // DECLAREs arrive, no batching required.
  if (h.chip_id == kZigbeeChipId &&
      (h.type == hubproto::MsgType::Declare ||
       h.type == hubproto::MsgType::State)) {
    hubproto::Header mirror = h;
    mirror.chip_id = kThreadChipId;
    bus->publish(mirror, p, len);
  } else if (h.type == hubproto::MsgType::Hello && h.chip_id == kThreadChipId) {
    // Matter just came up — kick Zigbee to re-DECLARE so the mirror can
    // create bridged endpoints for every entity we already know about.
    hubproto::Header q{};
    q.version = hubproto::kProtocolVersion;
    q.type = hubproto::MsgType::Query;
    q.chip_id = kZigbeeChipId;
    bus->publish(q, nullptr, 0);
    ESP_LOGI(kTag, "kick Zigbee QUERY because Matter came up");
  } else if (h.type == hubproto::MsgType::Command && h.chip_id == kThreadChipId) {
    // A Matter controller (Apple Home...) asked for this: the hub's intent.
    hubproto::ValueType vt{};
    hubproto::Command cmd{};
    if (len >= 3 && g_reg != nullptr && g_reg->value_type_of(h.entity_id, p[2], vt) &&
        hubproto::decode_command(p, len, vt, cmd)) {
      rec_note_command(nullptr, cmd.entity_id, cmd.attr_id, cmd.value);
    }
    hubproto::Header fwd = h;
    fwd.chip_id = kZigbeeChipId;
    bus->publish(fwd, p, len);
    ESP_LOGI(kTag, "forward Matter COMMAND entity=0x%04x -> Zigbee seq=%u",
             static_cast<unsigned>(h.entity_id), static_cast<unsigned>(h.seq));
  }
}

// ---- Console <-> main-loop request channel --------------------------------
enum ReqKind { REQ_HOME, REQ_GET, REQ_SETONOFF, REQ_TIMER, REQ_TIMERS, REQ_CANCEL,
               REQ_AT, REQ_SCHEDULES, REQ_UNSCHEDULE,
               REQ_CHIPS, REQ_LOG,
               REQ_PAIR, REQ_UNPAIR, REQ_IDENTIFY,
               REQ_WIFI_JOIN, REQ_WIFI_STATUS, REQ_WIFI_SCAN,
               REQ_TIME, REQ_TZ,
               REQ_NAME_SET, REQ_NAME_CLEAR, REQ_NAMES_LIST,
               REQ_RULE, REQ_DESIRED };

struct Req {
  ReqKind kind;
  std::uint16_t id;     // used if resolved early, or for commands that strictly use numeric index (timers)
  bool val;
  std::uint32_t delay_ms;
  int idx;
  char* out;            // response buffer owned by the caller (console task)
  std::size_t out_cap;
  const char* target;   // string target for entity (name or hex)
  const char* ssid;     // for REQ_WIFI_JOIN
  const char* pass;     // for REQ_WIFI_JOIN
  const char* tz;       // for REQ_TZ
  const char* name;     // for REQ_NAME_SET
  int argc;             // for REQ_RULE: the console words, alive until `done`
  char** argv;
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
    case hubproto::DeviceType::Chip: return "chip";
  }
  return "?";
}

const char* attr_name(std::uint8_t id) {
  return id == kOnOffAttr ? "on_off"
         : id == 0x13     ? "power_w"
         : id == 0x14     ? "energy_wh"
                          : nullptr;
}

// Format a bare value (e.g. "on", "200", "(1,2)").
void fmt_value(char* buf, std::size_t cap, hubproto::ValueType vt,
               const hubproto::Value& v) {
  switch (vt) {
    case hubproto::ValueType::Bool:
      std::snprintf(buf, cap, "%s", v.as_bool() ? "on" : "off");
      break;
    case hubproto::ValueType::U8:
      std::snprintf(buf, cap, "%u", static_cast<unsigned>(v.as_u8()));
      break;
    case hubproto::ValueType::I16:
      std::snprintf(buf, cap, "%d", static_cast<int>(v.as_i16()));
      break;
    case hubproto::ValueType::U16:
      std::snprintf(buf, cap, "%u", static_cast<unsigned>(v.as_u16()));
      break;
    case hubproto::ValueType::U32:
      std::snprintf(buf, cap, "%lu", static_cast<unsigned long>(v.as_u32()));
      break;
    case hubproto::ValueType::Enum8:
      std::snprintf(buf, cap, "%u", static_cast<unsigned>(v.as_enum8()));
      break;
    case hubproto::ValueType::F32:
      std::snprintf(buf, cap, "%.2f", static_cast<double>(v.as_f32()));
      break;
    case hubproto::ValueType::XyColor: {
      std::uint16_t x = 0, y = 0;
      v.as_xy(x, y);
      std::snprintf(buf, cap, "(%u,%u)", static_cast<unsigned>(x), static_cast<unsigned>(y));
      break;
    }
  }
}

// Format an attribute's current value into `buf` (e.g. "on_off=on", "0x02=200").
void fmt_attr(char* buf, std::size_t cap, const core::AttrState& a) {
  const char* name = attr_name(a.attr_id);
  char idbuf[8];
  if (name == nullptr) {
    std::snprintf(idbuf, sizeof(idbuf), "0x%02x", static_cast<unsigned>(a.attr_id));
    name = idbuf;
  }
  if (!a.has_value) {
    std::snprintf(buf, cap, "%s=?", name);
    return;
  }
  char vb[24];
  fmt_value(vb, sizeof(vb), a.value_type, a.value);
  std::snprintf(buf, cap, "%s=%s", name, vb);
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

const char* kNvsNamespace = "keystone";

void persist_names(const core::EntityRegistry<>::Names& names) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  // static: this runs in the request-drain path of the main task, whose stack
  // (CONFIG_ESP_MAIN_TASK_STACK_SIZE) is far too small for a 2 KB local.
  static std::uint8_t buf[2048];
  std::size_t written = names.save(buf, sizeof(buf));
  if (written > 0) {
    nvs_set_blob(h, "ent_names", buf, written);
    nvs_commit(h);
  } else {
    nvs_erase_key(h, "ent_names");
    nvs_commit(h);
  }
  nvs_close(h);
}

void restore_names(core::EntityRegistry<>::Names& names) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::size_t sz = 0;
  if (nvs_get_blob(h, "ent_names", nullptr, &sz) == ESP_OK && sz <= 2048) {
    static std::uint8_t buf[2048];
    if (nvs_get_blob(h, "ent_names", buf, &sz) == ESP_OK) {
      if (names.load(buf, sz)) {
        ESP_LOGI(kTag, "restored %u entity name(s) from NVS",
                 static_cast<unsigned>(names.size()));
      }
    }
  }
  nvs_close(h);
}

void persist_timers(const core::TimerTable<8>& timers,
                    std::uint32_t now_monotonic_ms) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  std::uint8_t buf[256];
  std::size_t written = timers.save(buf, sizeof(buf), now_monotonic_ms);
  if (written > 0) {
    nvs_set_blob(h, "timers", buf, written);
    nvs_commit(h);
  } else {
    nvs_erase_key(h, "timers");
    nvs_commit(h);
  }
  nvs_close(h);
}

void restore_timers(core::TimerTable<8>& timers, std::uint32_t now_ms, std::uint64_t now_epoch_s) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::size_t sz = 0;
  if (nvs_get_blob(h, "timers", nullptr, &sz) == ESP_OK && sz <= 256) {
    std::uint8_t buf[256];
    if (nvs_get_blob(h, "timers", buf, &sz) == ESP_OK) {
      if (timers.load(buf, sz, now_ms, now_epoch_s)) {
        ESP_LOGI(kTag, "restored %u timer(s) from NVS",
                 static_cast<unsigned>(timers.active_count()));
      }
    }
  }
  nvs_close(h);
}

using Schedules = core::ScheduleTable<8>;

void persist_schedules(const Schedules& sched) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  std::uint8_t buf[Schedules::kHeaderSize + 8 * Schedules::kEntryWireSize];
  const std::size_t written = sched.save(buf, sizeof(buf));
  if (sched.active_count() > 0 && written > 0) {
    nvs_set_blob(h, "schedules", buf, written);
  } else {
    nvs_erase_key(h, "schedules");
  }
  nvs_commit(h);
  nvs_close(h);
}

void restore_schedules(Schedules& sched) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::uint8_t buf[Schedules::kHeaderSize + 8 * Schedules::kEntryWireSize];
  std::size_t sz = sizeof(buf);
  if (nvs_get_blob(h, "schedules", buf, &sz) == ESP_OK) {
    if (sched.load(buf, sz)) {
      ESP_LOGI(kTag, "restored %u schedule(s) from NVS",
               static_cast<unsigned>(sched.active_count()));
    } else {
      ESP_LOGW(kTag, "stored schedules are corrupt — ignored");
    }
  }
  nvs_close(h);
}

void persist_desired() {
  nvs_handle_t h;
  std::uint8_t buf[core::DesiredState<32>::kHeaderSize + 32 * core::DesiredState<32>::kEntryWireSize];
  xSemaphoreTakeRecursive(g_rec_mu, portMAX_DELAY);
  const std::size_t n = g_rec->desired().count() == 0 ? 0 : g_rec->desired().save(buf, sizeof(buf));
  xSemaphoreGiveRecursive(g_rec_mu);
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  if (n > 0) {
    nvs_set_blob(h, "desired", buf, n);
  } else {
    nvs_erase_key(h, "desired");
  }
  nvs_commit(h);
  nvs_close(h);
}

void restore_desired(Reconciler& rec) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::uint8_t buf[core::DesiredState<32>::kHeaderSize + 32 * core::DesiredState<32>::kEntryWireSize];
  std::size_t sz = sizeof(buf);
  if (nvs_get_blob(h, "desired", buf, &sz) == ESP_OK) {
    if (rec.desired().load(buf, sz, now_ms(nullptr))) {
      ESP_LOGI(kTag, "restored %u desired value(s) from NVS",
               static_cast<unsigned>(rec.desired().count()));
    } else {
      ESP_LOGW(kTag, "stored desired state is corrupt — ignored");
    }
  }
  nvs_close(h);
}

void persist_rules(const Rules& rules) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  std::uint8_t buf[Rules::kHeaderSize + 16 * Rules::kEntryWireSize];
  const std::size_t n = rules.stored_count() == 0 ? 0 : rules.save(buf, sizeof(buf));
  if (n > 0) {
    nvs_set_blob(h, "rules", buf, n);
  } else {
    nvs_erase_key(h, "rules");
  }
  nvs_commit(h);
  nvs_close(h);
}

void restore_rules(Rules& rules) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::uint8_t buf[Rules::kHeaderSize + 16 * Rules::kEntryWireSize];
  std::size_t sz = sizeof(buf);
  if (nvs_get_blob(h, "rules", buf, &sz) == ESP_OK) {
    if (rules.load(buf, sz)) {
      ESP_LOGI(kTag, "restored %u rule(s) from NVS", static_cast<unsigned>(rules.rule_count()));
    } else {
      ESP_LOGW(kTag, "stored rules are corrupt — ignored");
    }
  }
  nvs_close(h);
}

// ---- `rule` console command: parsing + handling (runs on the main loop) ----

bool parse_attr(const char* s, std::uint8_t& out) {
  static const struct { const char* n; std::uint8_t id; } kNames[] = {
      {"on_off", kOnOffAttr}, {"power_w", 0x13}, {"energy_wh", 0x14}};
  for (const auto& e : kNames) {
    if (std::strcmp(s, e.n) == 0) { out = e.id; return true; }
  }
  char* end = nullptr;
  const long v = std::strtol(s, &end, 0);
  if (end == s || *end != '\0' || v < 0 || v > 255) return false;
  out = static_cast<std::uint8_t>(v);
  return true;
}

bool parse_value(const char* s, hubproto::ValueType vt, hubproto::Value& out) {
  char* end = nullptr;
  auto integer = [&](long lo, long hi, long& v) {
    v = std::strtol(s, &end, 0);
    return end != s && *end == '\0' && v >= lo && v <= hi;
  };
  long v = 0;
  switch (vt) {
    case hubproto::ValueType::Bool:
      if (!std::strcmp(s, "on") || !std::strcmp(s, "true") || !std::strcmp(s, "1")) {
        out = hubproto::Value::make_bool(true);
        return true;
      }
      if (!std::strcmp(s, "off") || !std::strcmp(s, "false") || !std::strcmp(s, "0")) {
        out = hubproto::Value::make_bool(false);
        return true;
      }
      return false;
    case hubproto::ValueType::U8:
      if (!integer(0, 255, v)) return false;
      out = hubproto::Value::make_u8(static_cast<std::uint8_t>(v));
      return true;
    case hubproto::ValueType::Enum8:
      if (!integer(0, 255, v)) return false;
      out = hubproto::Value::make_enum8(static_cast<std::uint8_t>(v));
      return true;
    case hubproto::ValueType::I16:
      if (!integer(-32768, 32767, v)) return false;
      out = hubproto::Value::make_i16(static_cast<std::int16_t>(v));
      return true;
    case hubproto::ValueType::U16:
      if (!integer(0, 65535, v)) return false;
      out = hubproto::Value::make_u16(static_cast<std::uint16_t>(v));
      return true;
    case hubproto::ValueType::U32: {
      if (s[0] == '-') return false;
      const unsigned long u = std::strtoul(s, &end, 0);
      if (end == s || *end != '\0') return false;
      out = hubproto::Value::make_u32(static_cast<std::uint32_t>(u));
      return true;
    }
    case hubproto::ValueType::F32: {
      const float f = std::strtof(s, &end);
      if (end == s || *end != '\0') return false;
      out = hubproto::Value::make_f32(f);
      return true;
    }
    case hubproto::ValueType::XyColor:
      return false;  // not expressible on one console word
  }
  return false;
}

bool parse_op(const char* s, core::CmpOp& op) {
  if (!std::strcmp(s, "==") || !std::strcmp(s, "=")) op = core::CmpOp::Eq;
  else if (!std::strcmp(s, "!=")) op = core::CmpOp::Ne;
  else if (!std::strcmp(s, "<")) op = core::CmpOp::Lt;
  else if (!std::strcmp(s, "<=")) op = core::CmpOp::Le;
  else if (!std::strcmp(s, ">")) op = core::CmpOp::Gt;
  else if (!std::strcmp(s, ">=")) op = core::CmpOp::Ge;
  else if (!std::strcmp(s, "any")) op = core::CmpOp::Always;
  else return false;
  return true;
}

const char* op_str(core::CmpOp op) {
  switch (op) {
    case core::CmpOp::Eq: return "==";
    case core::CmpOp::Ne: return "!=";
    case core::CmpOp::Lt: return "<";
    case core::CmpOp::Le: return "<=";
    case core::CmpOp::Gt: return ">";
    case core::CmpOp::Ge: return ">=";
    case core::CmpOp::Always: return "any";
  }
  return "?";
}

void fmt_target(char* buf, std::size_t cap, const Registry& reg, std::uint16_t id,
                std::uint8_t attr) {
  const char* name = reg.name_of(id);
  const char* an = attr_name(attr);
  char eb[24], ab[8];
  if (name != nullptr) {
    std::snprintf(eb, sizeof(eb), "%s", name);
  } else {
    std::snprintf(eb, sizeof(eb), "0x%04x", static_cast<unsigned>(id));
  }
  if (an == nullptr) {
    std::snprintf(ab, sizeof(ab), "0x%02x", static_cast<unsigned>(attr));
    an = ab;
  }
  std::snprintf(buf, cap, "%s.%s", eb, an);
}

// "HH:MM-HH:MM" -> minutes of day; start must differ from end (end < start wraps midnight).
bool parse_window(const char* s, std::uint16_t& start, std::uint16_t& end) {
  int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
  char tail = 0;
  if (std::sscanf(s, "%d:%d-%d:%d%c", &h1, &m1, &h2, &m2, &tail) != 4) return false;
  if (h1 < 0 || h1 > 23 || m1 < 0 || m1 > 59 || h2 < 0 || h2 > 23 || m2 < 0 || m2 > 59) return false;
  const int a = h1 * 60 + m1, b = h2 * 60 + m2;
  if (a == b) return false;
  start = static_cast<std::uint16_t>(a);
  end = static_cast<std::uint16_t>(b);
  return true;
}

const char* kRuleUsage =
    "usage:\n"
    "  rule                     - list rules\n"
    "  rule add <entity> <attr> <op> <value> then <entity> <attr> <value>\n"
    "  rule add <entity> <attr> any then <entity> <attr> <value>   (any change)\n"
    "  rule del <n>\n"
    "  optional, after the action (any order):\n"
    "    between HH:MM-HH:MM      local time window (may wrap midnight)\n"
    "    and <entity> <attr> <op> <value>   extra condition on another entity's current value\n"
    "    cooldown <sec>           at most once per <sec> seconds\n"
    "  <op>: == != < <= > >=   <attr>: on_off power_w energy_wh or a number\n"
    "  e.g. rule add plug power_w > 1500 then plug on_off off\n"
    "       rule add hall occupied == on then lamp on_off on between 18:00-23:00 cooldown 300\n";

void handle_rule(Req* r, const Registry& reg, Rules& rules) {
  const int argc = r->argc;
  char** argv = r->argv;
  char* out = r->out;
  const std::size_t cap = r->out_cap;

  if (argc < 2 || std::strcmp(argv[1], "list") == 0) {
    std::size_t off = static_cast<std::size_t>(std::snprintf(
        out, cap, "%u rule(s):\n", static_cast<unsigned>(rules.rule_count())));
    for (std::size_t i = 0; i < rules.rule_count() && off < cap; ++i) {
      const core::Rule* ru = rules.rule_at(i);
      char t[48], a[48], ov[24], av[24];
      fmt_target(t, sizeof(t), reg, ru->trigger_entity, ru->trigger_attr);
      fmt_target(a, sizeof(a), reg, ru->action_entity, ru->action_attr);
      fmt_value(av, sizeof(av), ru->action_value.type, ru->action_value);
      if (ru->match != nullptr) {
        std::snprintf(ov, sizeof(ov), "<builtin>");
      } else if (ru->op == core::CmpOp::Always) {
        ov[0] = '\0';
      } else {
        fmt_value(ov, sizeof(ov), ru->operand.type, ru->operand);
      }
      off += static_cast<std::size_t>(std::snprintf(
          out + off, cap - off, "  #%u  %s %s %s  ->  %s = %s",
          static_cast<unsigned>(i), t, op_str(ru->op), ov, a, av));
      if (ru->window_start != core::kNoWindow && off < cap) {
        off += static_cast<std::size_t>(std::snprintf(
            out + off, cap - off, "  between %02u:%02u-%02u:%02u",
            static_cast<unsigned>(ru->window_start / 60), static_cast<unsigned>(ru->window_start % 60),
            static_cast<unsigned>(ru->window_end / 60), static_cast<unsigned>(ru->window_end % 60)));
      }
      if (ru->has_cond && off < cap) {
        char ct[48], cv[24];
        fmt_target(ct, sizeof(ct), reg, ru->cond_entity, ru->cond_attr);
        fmt_value(cv, sizeof(cv), ru->cond_operand.type, ru->cond_operand);
        off += static_cast<std::size_t>(std::snprintf(
            out + off, cap - off, "  and %s %s %s", ct, op_str(ru->cond_op), cv));
      }
      if (ru->cooldown_s != 0 && off < cap) {
        off += static_cast<std::size_t>(std::snprintf(
            out + off, cap - off, "  cooldown %us", static_cast<unsigned>(ru->cooldown_s)));
      }
      if (off < cap) off += static_cast<std::size_t>(std::snprintf(out + off, cap - off, "\n"));
    }
    return;
  }

  if (std::strcmp(argv[1], "del") == 0 && argc == 3) {
    char* end = nullptr;
    const long n = std::strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || n < 0 || !rules.remove_rule(static_cast<std::size_t>(n))) {
      std::snprintf(out, cap, "no such rule: %s\n", argv[2]);
      return;
    }
    persist_rules(rules);
    std::snprintf(out, cap, "rule #%ld deleted\n", n);
    return;
  }

  if (std::strcmp(argv[1], "add") == 0 && argc >= 9) {
    const bool any = std::strcmp(argv[4], "any") == 0;
    const int base = any ? 5 : 6;  // index of "then"
    if (argc < base + 4 || std::strcmp(argv[base], "then") != 0) {
      std::snprintf(out, cap, "%s", kRuleUsage);
      return;
    }
    core::Rule ru{};
    ru.match = nullptr;
    std::uint16_t tid = 0, aid = 0;
    hubproto::ValueType tvt{}, avt{};
    if (!reg.resolve(argv[2], tid) || reg.find(tid) == nullptr) {
      std::snprintf(out, cap, "%s: trigger entity not found\n", argv[2]);
      return;
    }
    if (!parse_attr(argv[3], ru.trigger_attr) || !reg.value_type_of(tid, ru.trigger_attr, tvt)) {
      std::snprintf(out, cap, "%s: trigger attribute unknown for that entity\n", argv[3]);
      return;
    }
    ru.trigger_entity = tid;
    if (!parse_op(argv[4], ru.op)) {
      std::snprintf(out, cap, "%s: bad operator\n", argv[4]);
      return;
    }
    if (any) {
      ru.operand.type = tvt;
      ru.operand.len = static_cast<std::uint8_t>(hubproto::value_size(tvt));
    } else if (!parse_value(argv[5], tvt, ru.operand)) {
      std::snprintf(out, cap, "%s: bad value for that attribute\n", argv[5]);
      return;
    }
    if (!reg.resolve(argv[base + 1], aid) || reg.find(aid) == nullptr) {
      std::snprintf(out, cap, "%s: action entity not found\n", argv[base + 1]);
      return;
    }
    if (!parse_attr(argv[base + 2], ru.action_attr) || !reg.value_type_of(aid, ru.action_attr, avt)) {
      std::snprintf(out, cap, "%s: action attribute unknown for that entity\n", argv[base + 2]);
      return;
    }
    ru.action_entity = aid;
    if (!parse_value(argv[base + 3], avt, ru.action_value)) {
      std::snprintf(out, cap, "%s: bad value for that attribute\n", argv[base + 3]);
      return;
    }
    // Optional constraints after the action, in any order.
    for (int k = base + 4; k < argc;) {
      if (std::strcmp(argv[k], "between") == 0 && k + 1 < argc) {
        if (!parse_window(argv[k + 1], ru.window_start, ru.window_end)) {
          std::snprintf(out, cap, "%s: bad window (use HH:MM-HH:MM, start != end)\n", argv[k + 1]);
          return;
        }
        k += 2;
      } else if (std::strcmp(argv[k], "cooldown") == 0 && k + 1 < argc) {
        char* end = nullptr;
        const long sec = std::strtol(argv[k + 1], &end, 10);
        if (end == argv[k + 1] || *end != '\0' || sec < 1 || sec > 65535) {
          std::snprintf(out, cap, "%s: bad cooldown (1..65535 s)\n", argv[k + 1]);
          return;
        }
        ru.cooldown_s = static_cast<std::uint16_t>(sec);
        k += 2;
      } else if (std::strcmp(argv[k], "and") == 0 && k + 4 < argc) {
        std::uint16_t cid = 0;
        hubproto::ValueType cvt{};
        if (!reg.resolve(argv[k + 1], cid) || reg.find(cid) == nullptr) {
          std::snprintf(out, cap, "%s: condition entity not found\n", argv[k + 1]);
          return;
        }
        if (!parse_attr(argv[k + 2], ru.cond_attr) || !reg.value_type_of(cid, ru.cond_attr, cvt)) {
          std::snprintf(out, cap, "%s: condition attribute unknown for that entity\n", argv[k + 2]);
          return;
        }
        if (!parse_op(argv[k + 3], ru.cond_op) || ru.cond_op == core::CmpOp::Always) {
          std::snprintf(out, cap, "%s: bad operator\n", argv[k + 3]);
          return;
        }
        if (!parse_value(argv[k + 4], cvt, ru.cond_operand)) {
          std::snprintf(out, cap, "%s: bad value for that attribute\n", argv[k + 4]);
          return;
        }
        ru.has_cond = true;
        ru.cond_entity = cid;
        k += 5;
      } else {
        std::snprintf(out, cap, "%s", kRuleUsage);
        return;
      }
    }
    if (!rules.add_rule(ru)) {
      std::snprintf(out, cap, "rule table full\n");
      return;
    }
    persist_rules(rules);
    std::snprintf(out, cap, "rule #%u added\n", static_cast<unsigned>(rules.rule_count() - 1));
    return;
  }

  std::snprintf(out, cap, "%s", kRuleUsage);
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
  if (argc < 2) { printf("usage: get <id_or_name>\n"); return 1; }
  char buf[128];
  Req r{};
  r.kind = REQ_GET;
  r.target = argv[1];
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int set_onoff(const char* target, bool on) {
  char buf[128];
  Req r{};
  r.kind = REQ_SETONOFF;
  r.target = target;
  r.val = on;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_on(int argc, char** argv) {
  if (argc < 2) { printf("usage: on <id_or_name>\n"); return 1; }
  return set_onoff(argv[1], true);
}
int cmd_off(int argc, char** argv) {
  if (argc < 2) { printf("usage: off <id_or_name>\n"); return 1; }
  return set_onoff(argv[1], false);
}
int cmd_toggle(int argc, char** argv) {
  if (argc < 2) { printf("usage: toggle <id_or_name>\n"); return 1; }
  // Toggle relative to last known state (read it first).
  char buf[128];
  Req q{};
  q.kind = REQ_GET;
  q.target = argv[1];
  q.out = buf;
  q.out_cap = sizeof(buf);
  bool cur_on = submit(q) && std::strstr(buf, "=on") != nullptr;
  return set_onoff(argv[1], !cur_on);
}

// A timer's action value. "toggle" is stored as U8 2 (a Bool attribute can never
// hold it): it is resolved against the live state when the timer fires.
bool is_toggle(const hubproto::Value& v) {
  return v.type == hubproto::ValueType::U8 && v.raw[0] == 2;
}
const char* timer_action_text(const hubproto::Value& v) {
  return is_toggle(v) ? "toggle" : (v.as_bool() ? "on" : "off");
}

// Send a stored action (timer or schedule). A "toggle" is resolved against the live
// state; an unknown state means there is nothing to flip, so it is skipped.
void run_action(Dispatcher& disp, Registry& reg, std::uint32_t now, const char* kind,
                std::size_t slot, std::uint16_t entity_id, std::uint8_t attr_id,
                const hubproto::Value& val) {
  hubproto::Value v = val;
  if (is_toggle(val)) {
    hubproto::Value cur{};
    if (!reg.get_value(entity_id, attr_id, cur) || cur.type != hubproto::ValueType::Bool) {
      ESP_LOGW(kTag, "%s #%u: toggle skipped, state of 0x%04x unknown", kind,
               static_cast<unsigned>(slot), static_cast<unsigned>(entity_id));
      return;
    }
    v = hubproto::Value::make_bool(!cur.as_bool());
  }
  std::uint16_t seq = 0;
  disp.send(entity_id, attr_id, v, now, seq);
  ESP_LOGI(kTag, "%s #%u fired: entity 0x%04x attr 0x%02x (seq=%u)", kind,
           static_cast<unsigned>(slot), static_cast<unsigned>(entity_id),
           static_cast<unsigned>(attr_id), static_cast<unsigned>(seq));
}

// "daily" | "weekdays" | "weekends" | comma list of sun,mon,tue,wed,thu,fri,sat.
bool parse_days(const char* s, std::uint8_t& mask) {
  if (std::strcmp(s, "daily") == 0) { mask = core::kEveryDay; return true; }
  if (std::strcmp(s, "weekdays") == 0) { mask = core::kWeekdays; return true; }
  if (std::strcmp(s, "weekends") == 0) { mask = core::kWeekend; return true; }
  static const char* const kDay[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
  std::uint8_t m = 0;
  while (*s != '\0') {
    int found = -1;
    for (int d = 0; d < 7; ++d) {
      if (strncasecmp(s, kDay[d], 3) == 0) { found = d; break; }
    }
    if (found < 0) return false;
    m = static_cast<std::uint8_t>(m | (1u << found));
    s += 3;
    if (*s == ',') ++s;
    else if (*s != '\0') return false;
  }
  if (m == 0) return false;
  mask = m;
  return true;
}

const char* days_text(std::uint8_t mask, char* buf, std::size_t cap) {
  if (mask == core::kEveryDay) return "daily";
  if (mask == core::kWeekdays) return "weekdays";
  if (mask == core::kWeekend) return "weekends";
  static const char* const kDay[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
  std::size_t off = 0;
  for (int d = 0; d < 7 && off + 5 < cap; ++d) {
    if ((mask & (1u << d)) == 0) continue;
    off += static_cast<std::size_t>(
        std::snprintf(buf + off, cap - off, "%s%s", off ? "," : "", kDay[d]));
  }
  return buf;
}

// at <HH:MM> <on|off|toggle> <id_or_name> [daily|weekdays|weekends|mon,tue,...]
int cmd_at(int argc, char** argv) {
  static const char* kUsage =
      "usage: at <HH:MM> <on|off|toggle> <id_or_name> [daily|weekdays|weekends|mon,tue,..]\n";
  if (argc < 4 || argc > 5) { printf("%s", kUsage); return 1; }
  int hh = 0, mm = 0;
  char tail = 0;
  if (std::sscanf(argv[1], "%d:%d%c", &hh, &mm, &tail) != 2 || hh < 0 || hh > 23 ||
      mm < 0 || mm > 59) {
    printf("bad time '%s' (use HH:MM, 24 h, local time — see `tz`)\n", argv[1]);
    return 1;
  }
  const bool toggle = std::strcmp(argv[2], "toggle") == 0;
  if (!toggle && std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0) {
    printf("%s", kUsage);
    return 1;
  }
  std::uint8_t days = core::kEveryDay;
  if (argc == 5 && !parse_days(argv[4], days)) {
    printf("bad days '%s'\n%s", argv[4], kUsage);
    return 1;
  }
  char buf[160];
  Req r{};
  r.kind = REQ_AT;
  r.target = argv[3];
  r.val = std::strcmp(argv[2], "on") == 0;
  r.idx = toggle ? 1 : 0;
  r.delay_ms = static_cast<std::uint32_t>(hh * 60 + mm);  // minute of day
  r.id = days;                                           // weekday mask
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

// Free internal heap right now, the lowest it has been since boot, and the largest
// contiguous block (what a single big allocation can still get). Safe to call from
// the console task: the heap API is thread-safe.
int cmd_heap(int /*argc*/, char** /*argv*/) {
  printf("heap: free %u B, lowest since boot %u B, largest block %u B\n",
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
         static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  printf("psram: %u B free (0 = not enabled)\n",
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  return 0;
}

int cmd_schedules(int /*argc*/, char** /*argv*/) {
  char buf[640];
  Req r{};
  r.kind = REQ_SCHEDULES;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_unschedule(int argc, char** argv) {
  if (argc < 2) { printf("usage: unschedule <n>\n"); return 1; }
  char buf[64];
  Req r{};
  r.kind = REQ_UNSCHEDULE;
  r.idx = static_cast<int>(std::strtol(argv[1], nullptr, 10));
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_in(int argc, char** argv) {
  if (argc < 4) { printf("usage: in <sec> <on|off|toggle> <id_or_name>\n"); return 1; }
  const long sec = std::strtol(argv[1], nullptr, 10);
  const bool toggle = std::strcmp(argv[2], "toggle") == 0;
  if (!toggle && std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0) {
    printf("usage: in <sec> <on|off|toggle> <id_or_name>\n");
    return 1;
  }
  const bool on = std::strcmp(argv[2], "on") == 0;
  char buf[128];
  Req r{};
  r.kind = REQ_TIMER;
  r.target = argv[3];
  r.val = on;
  r.idx = toggle ? 1 : 0;  // REQ_TIMER: 1 = toggle
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

int cmd_rule(int argc, char** argv) {
  static char buf[1536];  // only the console task calls this
  Req r{};
  r.kind = REQ_RULE;
  r.argc = argc;
  r.argv = argv;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_desired(int argc, char** argv) {
  static char buf[1024];
  Req r{};
  r.kind = REQ_DESIRED;
  r.argc = argc;
  r.argv = argv;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_pair(int argc, char** argv) {
  const long sec = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 180;
  char buf[64];
  Req r{};
  r.kind = REQ_PAIR;
  r.id = static_cast<std::uint16_t>(sec);
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_unpair(int argc, char** argv) {
  if (argc < 2) { printf("usage: unpair <id_or_name>\n"); return 1; }
  char buf[96];
  Req r{};
  r.kind = REQ_UNPAIR;
  r.target = argv[1];
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_identify(int argc, char** argv) {
  if (argc < 2) { printf("usage: identify <id_or_name>\n"); return 1; }
  char buf[96];
  Req r{};
  r.kind = REQ_IDENTIFY;
  r.target = argv[1];
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_chips(int /*argc*/, char** /*argv*/) {
  char buf[384];
  Req r{};
  r.kind = REQ_CHIPS;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_log(int argc, char** argv) {
  if (argc < 2 || (std::strcmp(argv[1], "on") != 0 && std::strcmp(argv[1], "off") != 0)) {
    printf("usage: log on|off\n");
    return 1;
  }
  char buf[64];
  Req r{};
  r.kind = REQ_LOG;
  r.val = std::strcmp(argv[1], "on") == 0;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_wifi_join(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: wifi_join <ssid> <pass>\n");
    return 1;
  }
  char buf[128];
  Req r{};
  r.kind = REQ_WIFI_JOIN;
  r.ssid = argv[1];
  r.pass = argv[2];
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_wifi_status(int /*argc*/, char** /*argv*/) {
  char buf[128];
  Req r{};
  r.kind = REQ_WIFI_STATUS;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_wifi_scan(int /*argc*/, char** /*argv*/) {
  char buf[512];
  Req r{};
  r.kind = REQ_WIFI_SCAN;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r, 6000) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_time(int /*argc*/, char** /*argv*/) {
  char buf[160];
  Req r{};
  r.kind = REQ_TIME;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_tz(int argc, char** argv) {
  if (argc < 2) {
    const char* cur = getenv("TZ");
    printf("usage: tz <posix_tz> (e.g. UTC, CET-1CEST,M3.5.0,M10.5.0/3, MSK-3)\ncurrent tz: %s\n",
           cur ? cur : "UTC");
    return 1;
  }
  char buf[128];
  Req r{};
  r.kind = REQ_TZ;
  r.tz = argv[1];
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

int cmd_name(int argc, char** argv) {
  if (argc < 2) {
    char buf[512];
    Req r{};
    r.kind = REQ_NAMES_LIST;
    r.out = buf;
    r.out_cap = sizeof(buf);
    printf(submit(r) ? "%s" : "timeout\n", buf);
    return 0;
  }
  if (argc == 3 && std::strcmp(argv[1], "clear") == 0) {
    char buf[128];
    Req r{};
    r.kind = REQ_NAME_CLEAR;
    r.target = argv[2];
    r.out = buf;
    r.out_cap = sizeof(buf);
    printf(submit(r) ? "%s" : "timeout\n", buf);
    return 0;
  }
  if (argc == 3) {
    char buf[128];
    Req r{};
    r.kind = REQ_NAME_SET;
    r.target = argv[1]; // id_or_name to find target entity
    r.name = argv[2];   // new name
    r.out = buf;
    r.out_cap = sizeof(buf);
    printf(submit(r) ? "%s" : "timeout\n", buf);
    return 0;
  }
  printf("usage:\n"
         "  name                     - list named entities\n"
         "  name <id> <name>         - assign a name to an entity\n"
         "  name clear <id_or_name>  - remove a name\n");
  return 1;
}

// ota <chip> <url> <sha256> | ota status | ota abort. Not routed through the
// request queue: OtaJob is self-contained and thread-safe.
fw::OtaJob* g_ota = nullptr;

// After the chip accepts an image, the verifier asks it (once it has rebooted)
// whether the NEW image is the one running, or the bootloader rolled it back.
core::OtaVerifier* g_verifier = nullptr;
struct VerifyRequest {
  volatile bool pending = false;
  std::uint8_t chip = 0;
  std::uint8_t sha[32] = {};
} g_verify_req;

void on_ota_accepted(void* /*ctx*/, std::uint8_t chip, const std::uint8_t* sha) {
  // Runs on the OTA task: just hand the request to the main loop.
  g_verify_req.chip = chip;
  std::memcpy(g_verify_req.sha, sha, 32);
  g_verify_req.pending = true;
}

const char* verdict_text(core::OtaVerifier::Verdict v) {
  switch (v) {
    case core::OtaVerifier::Verdict::Idle: return "";
    case core::OtaVerifier::Verdict::Waiting: return "verifying: waiting for the chip to come back on the new image...";
    case core::OtaVerifier::Verdict::Verified: return "VERIFIED: the new image is running and confirmed";
    case core::OtaVerifier::Verdict::RunningUnconfirmed: return "new image is running but still NOT confirmed (pending verify)";
    case core::OtaVerifier::Verdict::RolledBack: return "ROLLED BACK: the chip is NOT running the new image (the bootloader reverted it)";
    case core::OtaVerifier::Verdict::NoRecord: return "cannot verify: the chip has no OTA record (it was updated by older firmware); the next update will be verifiable";
    case core::OtaVerifier::Verdict::NoAnswer: return "UNKNOWN: the chip did not answer the verification request in time";
  }
  return "";
}

void on_verify_done(void* /*ctx*/, std::uint8_t chip, core::OtaVerifier::Verdict v) {
  if (v == core::OtaVerifier::Verdict::Verified) {
    ESP_LOGI(kTag, "OTA chip 0x%02x: %s", chip, verdict_text(v));
  } else if (v == core::OtaVerifier::Verdict::NoRecord) {
    ESP_LOGW(kTag, "OTA chip 0x%02x: %s", chip, verdict_text(v));
  } else {
    ESP_LOGE(kTag, "OTA chip 0x%02x: %s", chip, verdict_text(v));
  }
}

bool parse_chip(const char* s, std::uint8_t& out) {
  if (std::strcmp(s, "zigbee") == 0 || std::strcmp(s, "1") == 0 || std::strcmp(s, "0x01") == 0) {
    out = kZigbeeChipId;
    return true;
  }
  if (std::strcmp(s, "thread") == 0 || std::strcmp(s, "2") == 0 || std::strcmp(s, "0x02") == 0) {
    out = kThreadChipId;
    return true;
  }
  return false;
}

int cmd_ota(int argc, char** argv) {
  if (g_ota == nullptr) { printf("ota not ready\n"); return 1; }
  if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
    char buf[160];
    g_ota->describe(buf, sizeof(buf));
    printf("%s", buf);
    if (g_verifier != nullptr && g_verifier->verdict() != core::OtaVerifier::Verdict::Idle) {
      printf("chip 0x%02x: %s\n", g_verifier->chip(), verdict_text(g_verifier->verdict()));
    }
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "abort") == 0) {
    g_ota->abort();
    printf("abort requested\n");
    return 0;
  }
  std::uint8_t chip = 0;
  if (argc != 4 || !parse_chip(argv[1], chip)) {
    printf("usage:\n"
           "  ota <zigbee|thread> <http(s)://url> <sha256-hex>  - update a C6 over the link\n"
           "  ota status                                        - progress / last result\n"
           "  ota abort                                         - stop the running update\n");
    return 1;
  }
  char err[96];
  if (!g_ota->start(chip, argv[2], argv[3], err, sizeof(err))) {
    printf("ota: %s\n", err);
    return 1;
  }
  printf("ota started for chip 0x%02x; watch `ota status` (the chip reboots at the end)\n",
         static_cast<unsigned>(chip));
  return 0;
}

int cmd_webtoken(int argc, char** argv);  // defined with the app glue, below
int cmd_webplain(int argc, char** argv);

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
  rc.max_cmdline_length = 384;  // `ota <chip> <url> <sha256>` is ~100-350 chars
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
  reg_cmd("at", "at <HH:MM> <on|off|toggle> <id> [daily|weekdays|weekends|mon,tue,..] - recurring local-time action", &cmd_at);
  reg_cmd("schedules", "list recurring time-of-day schedules", &cmd_schedules);
  reg_cmd("heap", "show free heap (now / lowest since boot / largest block)", &cmd_heap);
  reg_cmd("webtoken", "webtoken [<token>|clear] - app protocol secret (shows status without args)", &cmd_webtoken);
  reg_cmd("webplain", "webplain [on|off] - allow plain ws:// next to TLS (development only)", &cmd_webplain);
  reg_cmd("unschedule", "unschedule <n> - delete a schedule", &cmd_unschedule);
  reg_cmd("chips", "list known co-processor chips + traffic", &cmd_chips);
  reg_cmd("log", "log on|off - enable/disable verbose event log", &cmd_log);
  reg_cmd("pair", "pair [sec] - open Zigbee network for joining (default 180s)", &cmd_pair);
  reg_cmd("unpair", "unpair <id> - drop a device from the network", &cmd_unpair);
  reg_cmd("identify", "identify <id> - blink the device's LED for 10s", &cmd_identify);
  reg_cmd("wifi_join", "wifi_join <ssid> <pass> - connect to a Wi-Fi network", &cmd_wifi_join);
  reg_cmd("wifi_status", "show current Wi-Fi connection status", &cmd_wifi_status);
  reg_cmd("wifi_scan", "scan for nearby Wi-Fi networks", &cmd_wifi_scan);
  reg_cmd("time", "show current wall-clock time and sync status", &cmd_time);
  reg_cmd("tz", "tz <posix_tz> - get or set timezone", &cmd_tz);
  reg_cmd("ota", "ota <zigbee|thread> <url> <sha256> | status | abort - update a C6 over the link", &cmd_ota);
  reg_cmd("desired", "desired [clear <id_or_name>|all] - what the hub last asked each attribute to be", &cmd_desired);
  reg_cmd("rule", "rule [add|del] ... - persistent if-then rules (see `rule help`)", &cmd_rule);
  reg_cmd("name", "name [<id> <name> | clear <id_or_name>] - manage entity names", &cmd_name);

  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

}  // namespace

namespace {

// ---- App protocol glue (docs/app-protocol.md) ---------------------------------------
// Everything here that touches core objects runs in the main loop. The two hooks that
// fire elsewhere (a STATE in the UART RX task, a CMD_ACK in either) only enqueue or
// look up a small table.

struct AppEvent {
  std::uint16_t entity;
  std::uint8_t attr;
  hubproto::Value value;
};
QueueHandle_t g_app_ev_q = nullptr;
std::atomic<bool> g_app_resync{false};  // events were dropped: re-send the snapshot

void app_on_state(void* /*ctx*/, std::uint16_t entity, std::uint8_t attr, const hubproto::Value& v) {
  if (g_app_ev_q == nullptr) return;
  const AppEvent e{entity, attr, v};
  if (xQueueSend(g_app_ev_q, &e, 0) != pdTRUE) g_app_resync = true;  // clients resync, no gap recovery needed
}

// Commands in flight on behalf of a client: CMD_ACK (any task) -> `ack` frame.
struct AppPending {
  bool used = false;
  std::uint16_t seq = 0;
  int fd = -1;
  std::int32_t id = 0;
  std::uint32_t t_ms = 0;
};
AppPending g_app_pending[16];
SemaphoreHandle_t g_app_pending_mu = nullptr;

// While the main loop is inside Dispatcher::send, a fail-fast result (silent chip)
// is delivered synchronously, before send() returns the seq: catch it here so that
// `accepted` still goes out before `ack`.
struct AppSendCtx {
  TaskHandle_t task = nullptr;
  bool delivered = false;
  hubproto::CmdAckStatus status = hubproto::CmdAckStatus::Ok;
};
AppSendCtx g_app_send;

void app_send_ack(int fd, std::int32_t id, hubproto::CmdAckStatus status) {
  char b[96];
  core::JsonWriter w(b, sizeof(b));
  core::app::encode_ack(w, id, status);
  if (w.ok()) appsrv::send(fd, b, w.size());
}

void app_on_cmd_result(std::uint16_t seq, hubproto::CmdAckStatus status) {
  if (g_app_send.task != nullptr && g_app_send.task == xTaskGetCurrentTaskHandle()) {
    g_app_send.delivered = true;
    g_app_send.status = status;
    return;
  }
  if (g_app_pending_mu == nullptr) return;
  int fd = -1;
  std::int32_t id = 0;
  xSemaphoreTake(g_app_pending_mu, portMAX_DELAY);
  for (auto& p : g_app_pending) {
    if (p.used && p.seq == seq) {
      fd = p.fd;
      id = p.id;
      p.used = false;
      break;
    }
  }
  xSemaphoreGive(g_app_pending_mu);
  if (fd >= 0) app_send_ack(fd, id, status);
}

bool app_add_pending(std::uint16_t seq, int fd, std::int32_t id, std::uint32_t now) {
  xSemaphoreTake(g_app_pending_mu, portMAX_DELAY);
  bool ok = false;
  for (auto& p : g_app_pending) {
    if (!p.used) {
      p = AppPending{true, seq, fd, id, now};
      ok = true;
      break;
    }
  }
  xSemaphoreGive(g_app_pending_mu);
  return ok;
}

void app_expire_pending(std::uint32_t now) {  // the dispatcher always reports a result; belt and braces
  xSemaphoreTake(g_app_pending_mu, portMAX_DELAY);
  for (auto& p : g_app_pending) {
    if (p.used && now - p.t_ms > 60000) p.used = false;
  }
  xSemaphoreGive(g_app_pending_mu);
}

const char* app_chip_status(std::uint8_t id, const core::ChipMonitor::Info* ci,
                            const core::RadioCoordinator<Registry>& coord) {
  if (ci != nullptr && ci->silent) return "silent";
  if (coord.chip_ready(id)) return "ready";
  return coord.chip_known(id) ? "seen" : "silent";
}

core::app::ChipView app_chip_view(std::uint8_t id, const Registry& reg,
                                  const core::RadioCoordinator<Registry>& coord) {
  const auto* ci = g_monitor->info(id);
  core::app::ChipView v{id, chip_label(id), app_chip_status(id, ci, coord),
                        static_cast<std::uint8_t>(ci != nullptr ? ci->boot_id : 0),
                        ci != nullptr ? ci->reboots : 0};
  hubproto::Value x{};
  if (reg.get_value(id, hubproto::chip_attr::kHeapFree, x)) { v.has_heap = true; v.heap = x.as_u32(); }
  if (reg.get_value(id, hubproto::chip_attr::kUptimeS, x)) { v.has_uptime = true; v.uptime_s = x.as_u32(); }
  if (reg.get_value(id, hubproto::chip_attr::kTxFrames, x)) { v.has_tx = true; v.tx_frames = x.as_u32(); }
  if (reg.get_value(id, hubproto::chip_attr::kCrcErrors, x)) { v.has_crc = true; v.crc_errors = x.as_u16(); }
  return v;
}

std::uint32_t app_timer_left_s(const core::TimerTable<8>::Timer& tm, std::uint32_t now) {
  if (tm.has_wall_deadline && s_time_synced) {
    const time_t cur = time(nullptr);
    return static_cast<std::uint64_t>(cur) < tm.deadline_epoch_s
               ? static_cast<std::uint32_t>(tm.deadline_epoch_s - static_cast<std::uint64_t>(cur))
               : 0;
  }
  const std::int32_t rem = static_cast<std::int32_t>(tm.when_monotonic_ms - now);
  return rem > 0 ? static_cast<std::uint32_t>(rem / 1000) : 0;
}

// The snapshot's view of the home, adapted to core::app::encode_snapshot_part.
struct AppSource {
  const Registry& reg;
  const Rules& rules;
  const Schedules& sched;
  const core::TimerTable<8>& timers;
  const core::RadioCoordinator<Registry>& coord;
  std::uint32_t now;
  const Registry::Entity* ents[64] = {};
  std::size_t n_ents = 0;
  std::uint8_t chips[15] = {};
  std::size_t n_chips = 0;
  std::uint8_t sched_slots[8] = {};
  std::size_t n_sched = 0;
  std::uint8_t timer_slots[8] = {};
  std::size_t n_timers = 0;

  AppSource(const Registry& r, const Rules& ru, const Schedules& sc, const core::TimerTable<8>& tm,
            const core::RadioCoordinator<Registry>& co, std::uint32_t t)
      : reg(r), rules(ru), sched(sc), timers(tm), coord(co), now(t) {
    for (std::size_t i = 0; i < reg.size() && n_ents < 64; ++i) {
      const auto* e = reg.entity_at(i);
      // A chip's own entity is reported under `chips`, not as a device.
      if (e != nullptr && e->device_type != hubproto::DeviceType::Chip) ents[n_ents++] = e;
    }
    for (std::uint8_t id = 1; id <= 15; ++id) {
      if (g_monitor->info(id) != nullptr || coord.chip_known(id)) chips[n_chips++] = id;
    }
    for (std::uint8_t i = 0; i < 8; ++i) if (sched.get(i) != nullptr) sched_slots[n_sched++] = i;
    for (std::uint8_t i = 0; i < 8; ++i) if (timers.get(i) != nullptr) timer_slots[n_timers++] = i;
  }

  std::size_t count(core::app::Section s) const {
    switch (s) {
      case core::app::Section::Entities: return n_ents;
      case core::app::Section::Chips: return n_chips;
      case core::app::Section::Rules: return rules.rule_count();
      case core::app::Section::Schedules: return n_sched;
      case core::app::Section::Timers: return n_timers;
    }
    return 0;
  }
  void write(core::app::Section s, std::size_t i, core::JsonWriter& w) const {
    switch (s) {
      case core::app::Section::Entities:
        core::app::write_entity(w, *ents[i], reg.name_of(ents[i]->entity_id));
        break;
      case core::app::Section::Chips:
        core::app::write_chip(w, app_chip_view(chips[i], reg, coord));
        break;
      case core::app::Section::Rules:
        core::app::write_rule(w, i, *rules.rule_at(i));
        break;
      case core::app::Section::Schedules:
        core::app::write_schedule(w, sched_slots[i], *sched.get(sched_slots[i]));
        break;
      case core::app::Section::Timers: {
        const auto* tm = timers.get(timer_slots[i]);
        core::app::write_timer(w, core::app::TimerView{timer_slots[i], tm->entity_id, tm->attr_id,
                                                       tm->value, app_timer_left_s(*tm, now)});
        break;
      }
    }
  }

  // A fingerprint of everything a client shows structurally (not live values): when it
  // changes the clients are sent a fresh snapshot instead of many small events.
  std::uint32_t fingerprint() const {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](const void* p, std::size_t n) {
      const auto* b = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    };
    for (std::size_t i = 0; i < n_ents; ++i) {
      const auto& e = *ents[i];
      mix(&e.entity_id, sizeof(e.entity_id));
      mix(&e.chip_id, sizeof(e.chip_id));
      const auto dt = static_cast<std::uint8_t>(e.device_type);
      mix(&dt, 1);
      for (std::uint8_t a = 0; a < e.attr_count; ++a) {
        mix(&e.attrs[a].attr_id, 1);
        const auto vt = static_cast<std::uint8_t>(e.attrs[a].value_type);
        mix(&vt, 1);
        mix(&e.attrs[a].flags, 1);
      }
      const char* name = reg.name_of(e.entity_id);
      if (name != nullptr) mix(name, std::strlen(name) + 1);
    }
    char tmp[512];
    for (std::size_t i = 0; i < rules.rule_count(); ++i) {
      core::JsonWriter w(tmp, sizeof(tmp));
      core::app::write_rule(w, i, *rules.rule_at(i));
      mix(tmp, w.size());
    }
    for (std::size_t i = 0; i < n_sched; ++i) {
      core::JsonWriter w(tmp, sizeof(tmp));
      core::app::write_schedule(w, sched_slots[i], *sched.get(sched_slots[i]));
      mix(tmp, w.size());
    }
    for (std::size_t i = 0; i < n_timers; ++i) {  // no left_s: it changes every second
      const auto* tm = timers.get(timer_slots[i]);
      mix(&tm->entity_id, sizeof(tm->entity_id));
      mix(&tm->attr_id, 1);
      mix(tm->value.raw, 4);
    }
    return h;
  }
};

// Send the snapshot (in parts) to one client, or to all (fd < 0). Main loop only: the
// buffer is shared.
void app_send_snapshot(int fd, const AppSource& src) {
  static char buf[core::app::kMaxFrame];
  core::app::SnapshotCursor cur;
  while (!cur.done()) {
    core::JsonWriter w(buf, sizeof(buf));
    // Items are at most ~1 KB (an entity with 8 attributes); 3000 leaves room for one.
    if (!core::app::encode_snapshot_part(cur, const_cast<AppSource&>(src), w, 3000)) {
      ESP_LOGE(kTag, "app: snapshot part %u did not fit a frame", static_cast<unsigned>(cur.part));
      return;
    }
    if (fd < 0) appsrv::broadcast(buf, w.size());
    else appsrv::send(fd, buf, w.size());
  }
}

void app_handle_cmd(const appsrv::Inbound& in, const Registry& reg, Dispatcher& disp, std::uint32_t now) {
  using namespace core::app;
  char b[160];
  core::JsonWriter w(b, sizeof(b));
  auto fail = [&](Err e, const char* msg = nullptr) {
    encode_error(w, in.id, e, msg);
    appsrv::send(in.fd, b, w.size());
  };
  const auto entity = static_cast<std::uint16_t>(in.entity);
  const auto attr = static_cast<std::uint8_t>(in.attr);
  const Registry::Entity* e = reg.find(entity);
  if (e == nullptr || e->device_type == hubproto::DeviceType::Chip) return fail(Err::NotFound, "entity");
  const core::AttrState* a = nullptr;
  for (std::uint8_t i = 0; i < e->attr_count; ++i) if (e->attrs[i].attr_id == attr) a = &e->attrs[i];
  if (a == nullptr) return fail(Err::NotFound, "attr");
  if ((a->flags & hubproto::kFlagWritable) == 0) return fail(Err::ReadOnly);
  hubproto::Value v{};
  if (!to_value(in.value, a->value_type, v)) {
    char msg[40];
    std::snprintf(msg, sizeof(msg), "value does not fit %s", vt_name(a->value_type));
    return fail(Err::BadArgs, msg);
  }

  g_app_send = AppSendCtx{xTaskGetCurrentTaskHandle(), false, hubproto::CmdAckStatus::Ok};
  std::uint16_t seq = 0;
  const bool sent = disp.send(entity, attr, v, now, seq);
  const AppSendCtx done = g_app_send;
  g_app_send = AppSendCtx{};
  if (!sent && !done.delivered) return fail(Err::Internal, "command queue full");

  encode_accepted(w, in.id, seq);
  appsrv::send(in.fd, b, w.size());
  ESP_LOGI(kTag, "app: cmd id=%d entity=0x%04x attr=0x%02x seq=%u", static_cast<int>(in.id),
           static_cast<unsigned>(entity), static_cast<unsigned>(attr), static_cast<unsigned>(seq));
  if (done.delivered) {
    app_send_ack(in.fd, in.id, done.status);
  } else if (!app_add_pending(seq, in.fd, in.id, now)) {
    app_send_ack(in.fd, in.id, hubproto::CmdAckStatus::Fail);  // no room to track it: say so
  }
}

std::atomic<bool> g_app_force_check{false};  // a mutation just happened: look for structural change at once

// A management request (docs/app-protocol.md §5), run in the main loop. The same
// containers the console edits, so both surfaces see each other's changes.
void app_handle_req(const appsrv::Inbound& in, Registry& reg, Rules& rules, Schedules& sched,
                    core::TimerTable<8>& timers, std::uint32_t now) {
  using namespace core::app;
  core::JsonReader reader;
  const ClientMsg m = decode_client(reader, in.text, in.len);
  char b[192];
  core::JsonWriter w(b, sizeof(b));

  Outcome o = Outcome::fail(Err::Unsupported, m.op);
  const char* op = m.kind == ClientMsg::Kind::Req ? m.op : "";
  if (m.kind != ClientMsg::Kind::Req) {
    o = Outcome::fail(Err::BadArgs, "not a request");
  } else if (!std::strcmp(op, "name.set")) {
    o = name_set(reg, reader, m.args);
    if (o.ok) persist_names(reg.names());
  } else if (!std::strcmp(op, "name.clear")) {
    o = name_clear(reg, reader, m.args);
    if (o.ok) persist_names(reg.names());
  } else if (!std::strcmp(op, "rule.add")) {
    o = rule_add(reg, rules, reader, m.args);
    if (o.ok) persist_rules(rules);
  } else if (!std::strcmp(op, "rule.del")) {
    o = rule_del(rules, reader, m.args);
    if (o.ok) persist_rules(rules);
  } else if (!std::strcmp(op, "schedule.add")) {
    o = schedule_add(reg, sched, reader, m.args);
    if (o.ok) persist_schedules(sched);
  } else if (!std::strcmp(op, "schedule.del")) {
    o = schedule_del(sched, reader, m.args);
    if (o.ok) persist_schedules(sched);
  } else if (!std::strcmp(op, "timer.add")) {
    time_t epoch = 0;
    if (s_time_synced) time(&epoch);
    o = timer_add(reg, timers, reader, m.args, now, static_cast<std::uint64_t>(epoch));
    if (o.ok) persist_timers(timers, now);
  } else if (!std::strcmp(op, "timer.cancel")) {
    o = timer_cancel(timers, reader, m.args);
    if (o.ok) persist_timers(timers, now);
  }

  if (o.ok) {
    if (o.has_index) encode_res_ok_index(w, in.id, o.index);
    else encode_res_ok(w, in.id);
    g_app_force_check = true;
  } else {
    encode_res_error(w, in.id, o.err, o.msg[0] != '\0' ? o.msg : nullptr);
  }
  appsrv::send(in.fd, b, w.size());
  ESP_LOGI(kTag, "app: req %s -> %s%s%s", op, o.ok ? "ok" : err_name(o.err), o.msg[0] ? " " : "", o.msg);
}

void app_hub_info(core::app::HubInfo& h) {
  const char* tz = std::getenv("TZ");
  h.name = "home";
  h.fw = esp_app_get_description()->version;
  h.time = s_time_synced ? static_cast<std::int64_t>(time(nullptr)) : 0;
  h.tz = tz != nullptr ? tz : "UTC";
  h.synced = s_time_synced;
}

// mDNS: advertise `_keystone._tcp` on the port the app server actually listens on. Called
// every main-loop pass; it only does work when the server's mode/port changed, so the
// console commands and the token restore need no hooks. TXT carries no secret and nothing
// the client should trust (an attacker on the LAN can answer mDNS too): trust is the
// pinned fingerprint, mDNS only says where to look.
void mdns_sync() {
  static bool inited = false;
  static bool failed = false;
  static int advertised_port = 0;
  static appsrv::Mode advertised_mode = appsrv::Mode::Stopped;
  if (failed) return;
  if (!inited) {
    if (mdns_init() != ESP_OK) {
      ESP_LOGW(kTag, "mdns_init failed");
      failed = true;
      return;
    }
    mdns_hostname_set(kHubHostname);
    mdns_instance_name_set("Keystone hub");
    inited = true;
  }
  const appsrv::Mode mode = appsrv::running() ? appsrv::mode() : appsrv::Mode::Stopped;
  const int port = mode == appsrv::Mode::Stopped ? 0 : appsrv::port();
  if (mode == advertised_mode && port == advertised_port) return;
  if (advertised_mode != appsrv::Mode::Stopped) mdns_service_remove("_keystone", "_tcp");
  advertised_mode = appsrv::Mode::Stopped;
  advertised_port = 0;
  if (mode == appsrv::Mode::Stopped) {
    ESP_LOGI(kTag, "mdns: service withdrawn");
    return;
  }
  core::app::HubInfo info;
  app_hub_info(info);
  mdns_txt_item_t txt[] = {
      {"v", "1"},
      {"name", info.name},
      {"tls", mode == appsrv::Mode::Tls ? "1" : "0"},
  };
  if (mdns_service_add(nullptr, "_keystone", "_tcp", static_cast<std::uint16_t>(port), txt,
                       sizeof(txt) / sizeof(txt[0])) != ESP_OK) {
    ESP_LOGW(kTag, "mdns_service_add failed");  // retried on the next pass
    return;
  }
  ESP_LOGI(kTag, "mdns: %s.local _keystone._tcp port %d (%s)", kHubHostname, port,
           mode == appsrv::Mode::Tls ? "wss" : "ws");
  advertised_mode = mode;
  advertised_port = port;
}

constexpr const char* kWebTokenKey = "webtoken";

void persist_webtoken(const char* token) {  // nullptr clears
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  if (token != nullptr) nvs_set_str(h, kWebTokenKey, token);
  else nvs_erase_key(h, kWebTokenKey);
  nvs_commit(h);
  nvs_close(h);
}

bool restore_webtoken(char* out, std::size_t cap) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
  std::size_t sz = cap;
  const bool ok = nvs_get_str(h, kWebTokenKey, out, &sz) == ESP_OK;
  nvs_close(h);
  return ok;
}

constexpr const char* kWebPlainKey = "webplain";

bool restore_webplain() {  // development switch: serve plain ws:// even though TLS is available
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
  std::uint8_t v = 0;
  const bool on = nvs_get_u8(h, kWebPlainKey, &v) == ESP_OK && v != 0;
  nvs_close(h);
  return on;
}

void persist_webplain(bool on) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  if (on) nvs_set_u8(h, kWebPlainKey, 1);
  else nvs_erase_key(h, kWebPlainKey);
  nvs_commit(h);
  nvs_close(h);
}

const char* app_mode_text() {
  switch (appsrv::mode()) {
    case appsrv::Mode::Tls: return "wss (TLS)";
    case appsrv::Mode::Plain: return "ws (PLAIN, development only)";
    case appsrv::Mode::Stopped: break;
  }
  return "stopped";
}

// webplain [on|off]: development switch. With TLS available the app server speaks only
// wss:// unless this is on; without a TLS identity in the firmware it is always plain.
int cmd_webplain(int argc, char** argv) {
  char tok[core::app::kMaxToken + 1] = {};
  const bool have = restore_webtoken(tok, sizeof(tok));
  if (argc >= 2) {
    const bool on = std::strcmp(argv[1], "on") == 0;
    if (!on && std::strcmp(argv[1], "off") != 0) {
      printf("usage: webplain [on|off]\n");
      return 1;
    }
    persist_webplain(on);
    if (have) {
      if (!appsrv::start(tok, &app_hub_info, on)) {
        printf("saved, but the app server did not restart\n");
        return 1;
      }
    }
  }
  printf("plain ws:// %s (TLS %s in this firmware); app server: %s on port %d\n",
         restore_webplain() ? "ALLOWED" : "off", appsrv::tls_available() ? "available" : "NOT available",
         app_mode_text(), appsrv::port());
  return 0;
}

// webtoken [<token>|clear]: the shared secret of the app protocol. Never printed.
int cmd_webtoken(int argc, char** argv) {
  char cur[core::app::kMaxToken + 1] = {};
  const bool have = restore_webtoken(cur, sizeof(cur));
  if (argc == 1) {
    printf("web token: %s; app server: %s on port %d, %u client(s) connected\n",
           have ? "set" : "NOT set", app_mode_text(), appsrv::port(), static_cast<unsigned>(appsrv::client_count()));
    if (appsrv::tls_available()) printf("certificate SHA-256 (what apps pin): %s\n", appsrv::cert_fingerprint());
    return 0;
  }
  if (std::strcmp(argv[1], "clear") == 0) {
    persist_webtoken(nullptr);
    appsrv::stop();
    printf("web token cleared; app server stopped\n");
    return 0;
  }
  const std::size_t n = std::strlen(argv[1]);
  if (n < 8 || n > core::app::kMaxToken) {
    printf("token must be 8..%u characters\n", static_cast<unsigned>(core::app::kMaxToken));
    return 1;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (argv[1][i] < 0x21 || argv[1][i] > 0x7E) {
      printf("token: printable ASCII without spaces only\n");
      return 1;
    }
  }
  persist_webtoken(argv[1]);
  if (!appsrv::start(argv[1], &app_hub_info, restore_webplain())) {
    printf("token saved, but the app server did not start\n");
    return 1;
  }
  printf("token set; app server (re)started: %s on port %d — existing connections were closed\n",
         app_mode_text(), appsrv::port());
  if (appsrv::mode() == appsrv::Mode::Tls) printf("certificate SHA-256: %s\n", appsrv::cert_fingerprint());
  return 0;
}

}  // namespace

extern "C" void app_main(void) {
  // Инициализация Wi-Fi
  wifi_init();

  // Zigbee link (UART1).
  static fw::EspUartLink link_zb([] {
    fw::EspUartLink::Config c;
    c.port = UART_NUM_1;
    c.tx_gpio = kZbTxGpio;
    c.rx_gpio = kZbRxGpio;
    return c;
  }());
  ESP_ERROR_CHECK(link_zb.begin());

  // Thread link (UART2).
  static fw::EspUartLink link_th([] {
    fw::EspUartLink::Config c;
    c.port = UART_NUM_2;
    c.tx_gpio = kThTxGpio;
    c.rx_gpio = kThRxGpio;
    return c;
  }());
  ESP_ERROR_CHECK(link_th.begin());

  static core::UartTransport bus_zb(link_zb);
  static core::UartTransport bus_th(link_th);

  static Registry reg;
  restore_names(reg.names());
  static ChipRouter bus;                            // presents itself as one Bus
  bus.bind(kZigbeeChipId, bus_zb);                  // publish(chip=0x01) -> UART1
  bus.bind(kThreadChipId, bus_th);                  // publish(chip=0x02) -> UART2

  static core::RadioCoordinator<Registry> coord(bus, reg);
  static core::StateMachine<Registry> sm(bus, reg);
  static Dispatcher disp(bus, reg);
  static core::ChipMonitor monitor(bus, &now_ms, nullptr);
  g_monitor = &monitor;
  monitor.on_event(&on_chip_event, nullptr);
  disp.set_alive_filter(&core::ChipMonitor::alive_thunk, &monitor);
  static Rules rules(disp, &now_ms, nullptr);

  static fw::OtaJob ota_job(bus);
  g_ota = &ota_job;
  static core::OtaVerifier verifier(bus);
  g_verifier = &verifier;
  verifier.on_done(&on_verify_done, nullptr);
  ota_job.on_accepted(&on_ota_accepted, nullptr);

  disp.on_result(&on_cmd_result, nullptr);
  bus.subscribe(&on_bus_traffic, nullptr);  // per-chip rx counter (silent unless `log on`)
  bus.subscribe(&on_bus_forward, &bus);     // Matter <-> Zigbee cross-chip mirror

  restore_rules(rules);  // user rules from NVS (`rule add ...`)
  // Rule constraints: local minute of day (-1 until SNTP has synced) and the
  // current value of the entity a rule's `and ...` condition looks at.
  rules.set_clock(
      [](void*) -> int {
        if (!s_time_synced) return -1;
        const time_t wall = time(nullptr);
        struct tm lt{};
        localtime_r(&wall, &lt);
        return lt.tm_hour * 60 + lt.tm_min;
      },
      nullptr);
  rules.set_state_reader(
      [](void* ctx, std::uint16_t entity, std::uint8_t attr, hubproto::Value& out) {
        return static_cast<Registry*>(ctx)->get_value(entity, attr, out);
      },
      &reg);

  g_rec_mu = xSemaphoreCreateRecursiveMutex();
  g_reg = &reg;
  static Reconciler reconciler(disp, &now_ms, nullptr);
  g_rec = &reconciler;
  restore_desired(reconciler);
  reconciler.on_event(&on_reconcile, nullptr);
  disp.on_send(&rec_note_command, nullptr);
  sm.on_report(&rec_on_report, nullptr);

  sm.on_change(&Rules::on_state_change, &rules);
  sm.on_change(&on_state_change, nullptr);

  link_zb.start();
  link_th.start();

  // Persistent timers table (survives reboots via NVS).
  static core::TimerTable<8> timer_table;
  time_t boot_now = 0;
  if (s_time_synced) time(&boot_now);
  restore_timers(timer_table, now_ms(nullptr), static_cast<std::uint64_t>(boot_now));

  // Recurring time-of-day actions (`at HH:MM …`); they only run once the clock is
  // synced, in local time (see `tz`).
  static Schedules schedule_table;
  restore_schedules(schedule_table);

  // App protocol: live events from the UART RX task, CMD_ACK bookkeeping, and the
  // server itself (only when a token has been set with `webtoken`).
  g_app_ev_q = xQueueCreate(32, sizeof(AppEvent));
  g_app_pending_mu = xSemaphoreCreateMutex();
  {
    char tok[core::app::kMaxToken + 1] = {};
    if (restore_webtoken(tok, sizeof(tok))) {
      if (!appsrv::start(tok, &app_hub_info, restore_webplain())) ESP_LOGE(kTag, "app server failed to start");
    } else {
      ESP_LOGI(kTag, "app server off: no token (set one with `webtoken <token>`)");
    }
  }
  std::uint32_t app_last_check_ms = 0;
  std::uint32_t app_last_fp = 0;
  struct ChipSent {
    char status[8] = {};
    std::uint8_t boot_id = 0;
    std::uint32_t reboots = 0;
    std::uint32_t t_ms = 0;
    bool any = false;
  } app_chip_sent[16];

  g_req_q = xQueueCreate(8, sizeof(Req*));
  start_console();
  ESP_LOGI(kTag, "brain up; console ready (type 'help'), awaiting HELLO from chips");

  bool announced_zb = false;
  bool announced_th = false;

  // Self-heal QUERY: if a bound chip hasn't declared yet (e.g. brain rebooted
  // after the chip, so we missed its HELLO), poke it every so often. On a QUERY
  // the co-processor re-sends DECLARE + DECLARE_END; a ready chip is skipped.
  const std::uint32_t kSelfHealMs = 5000;
  std::uint32_t last_query_ms = 0;
  const std::uint8_t kBoundChips[] = {kZigbeeChipId, kThreadChipId};
  while (true) {
    const std::uint32_t t = now_ms(nullptr);
    disp.poll(t);
    monitor.poll(t);
    if (g_verify_req.pending) {
      g_verify_req.pending = false;
      const auto* ci = monitor.info(g_verify_req.chip);
      verifier.start(g_verify_req.chip, g_verify_req.sha, ci != nullptr ? ci->boot_id : 0, t);
    }
    verifier.poll(t);
    {
      xSemaphoreTakeRecursive(g_rec_mu, portMAX_DELAY);
      const bool dirty = reconciler.take_dirty();
      xSemaphoreGiveRecursive(g_rec_mu);
      if (dirty) persist_desired();
    }

    if (s_clock_just_synced) {
      s_clock_just_synced = false;
      time_t now = 0;
      time(&now);
      timer_table.on_clock_synced(static_cast<std::uint64_t>(now), t);
      persist_timers(timer_table, t);
      {
        char b[128];
        core::JsonWriter w(b, sizeof(b));
        const char* tz = std::getenv("TZ");
        core::app::encode_time(w, static_cast<std::int64_t>(now), tz != nullptr ? tz : "UTC", true);
        if (w.ok()) appsrv::broadcast(b, w.size());
      }
    }

    if (!announced_zb && coord.chip_ready(kZigbeeChipId)) {
      announced_zb = true;
      ESP_LOGI(kTag, "chip 0x%02x (zigbee) declared; registry has %u entities",
               kZigbeeChipId, static_cast<unsigned>(reg.size()));
    }
    if (!announced_th && coord.chip_ready(kThreadChipId)) {
      announced_th = true;
      ESP_LOGI(kTag, "chip 0x%02x (thread) declared; registry has %u entities",
               kThreadChipId, static_cast<unsigned>(reg.size()));
      // Mirror the whole registry as DECLAREs toward the Matter Bridge so it
      // can allocate a bridged endpoint per entity. Without this the Bridge
      // only sees entities that declare live *after* it comes up.
      for (std::size_t i = 0; i < reg.size(); ++i) {
        const auto* e = reg.entity_at(i);
        if (!e) continue;
        hubproto::AttrDecl attrs[8];
        const std::uint8_t n = e->attr_count > 8 ? 8 : e->attr_count;
        for (std::uint8_t k = 0; k < n; ++k) {
          attrs[k].attr_id = e->attrs[k].attr_id;
          attrs[k].value_type = e->attrs[k].value_type;
          attrs[k].flags = e->attrs[k].flags;
        }
        std::uint8_t buf[64];
        auto r = hubproto::encode_declare(e->entity_id, e->device_type, attrs, n,
                                          buf, sizeof(buf));
        if (!r.ok) continue;
        hubproto::Header d{};
        d.version = hubproto::kProtocolVersion;
        d.type = hubproto::MsgType::Declare;
        d.chip_id = kThreadChipId;
        d.entity_id = e->entity_id;
        bus.publish(d, buf, r.len);
      }
    }

    if (t - last_query_ms >= kSelfHealMs) {
      last_query_ms = t;
      for (auto id : kBoundChips) {
        if (coord.chip_ready(id)) continue;
        hubproto::Header q{};
        q.version = hubproto::kProtocolVersion;
        q.type = hubproto::MsgType::Query;
        q.chip_id = id;
        q.entity_id = hubproto::kSelfEntityId;
        bus.publish(q, nullptr, 0);
        ESP_LOGD(kTag, "self-heal QUERY -> chip 0x%02x", id);
      }
    }

    // Fire due timers.
    struct TimerFireContext {
      Dispatcher*   disp;
      Registry*     reg;
      std::uint32_t now_ms;
      bool          fired;
    };
    TimerFireContext fc{&disp, &reg, t, false};
    timer_table.poll(t, s_time_synced ? static_cast<std::uint64_t>(time(nullptr)) : 0,
                     [](void* ctx, std::size_t slot, std::uint16_t entity_id,
                        std::uint8_t attr_id, const hubproto::Value& val) {
                       auto* c = static_cast<TimerFireContext*>(ctx);
                       c->fired = true;
                       run_action(*c->disp, *c->reg, c->now_ms, "timer", slot, entity_id,
                                  attr_id, val);
                     }, &fc);
    if (fc.fired) {
      persist_timers(timer_table, t);
    }

    // Fire due time-of-day schedules (local time; paused until the clock is synced).
    if (s_time_synced) {
      const time_t wall = time(nullptr);
      struct tm lt{};
      localtime_r(&wall, &lt);
      const core::LocalTime local{lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                                  lt.tm_hour, lt.tm_min, lt.tm_wday};
      const std::size_t n = schedule_table.poll(
          local,
          [](void* ctx, std::size_t slot, const Schedules::Schedule& s) {
            auto* c = static_cast<TimerFireContext*>(ctx);
            run_action(*c->disp, *c->reg, c->now_ms, "schedule", slot, s.entity_id,
                       s.attr_id, s.value);
          },
          &fc);
      if (n > 0) persist_schedules(schedule_table);  // keeps last_fired_day across reboots
    }

    // ---- App protocol: live state, structural resync, chip health, inbound commands ----
    {
      AppEvent ev;
      for (int n = 0; n < 16 && xQueueReceive(g_app_ev_q, &ev, 0) == pdTRUE; ++n) {
        const auto* e = reg.find(ev.entity);
        if (e == nullptr || e->device_type == hubproto::DeviceType::Chip) continue;
        if (appsrv::client_count() == 0) continue;
        char b[160];
        core::JsonWriter w(b, sizeof(b));
        core::app::encode_state(w, ev.entity, ev.attr, ev.value);
        if (w.ok()) appsrv::broadcast(b, w.size());
      }
      mdns_sync();
      if (appsrv::running()) {
        appsrv::tick(t);
        if (appsrv::client_count() > 0 && (t - app_last_check_ms >= 1000 || g_app_force_check.exchange(false))) {
          app_last_check_ms = t;
          app_expire_pending(t);
          AppSource src(reg, rules, schedule_table, timer_table, coord, t);
          const std::uint32_t fp = src.fingerprint();
          const bool overflow = g_app_resync.exchange(false);
          if (overflow || fp != app_last_fp) {
            app_last_fp = fp;
            app_send_snapshot(-1, src);  // structure changed (or events were lost): start over
          }
          for (std::size_t i = 0; i < src.n_chips; ++i) {
            const std::uint8_t id = src.chips[i];
            const auto cv = app_chip_view(id, reg, coord);
            ChipSent& cs = app_chip_sent[id & 15];
            const bool changed = !cs.any || std::strcmp(cs.status, cv.status) != 0 ||
                                 cs.boot_id != cv.boot_id || cs.reboots != cv.reboots;
            if (changed || t - cs.t_ms >= 10000) {
              char b[256];
              core::JsonWriter w(b, sizeof(b));
              core::app::encode_chip_event(w, cv);
              if (w.ok()) appsrv::broadcast(b, w.size());
              std::snprintf(cs.status, sizeof(cs.status), "%s", cv.status);
              cs.boot_id = cv.boot_id;
              cs.reboots = cv.reboots;
              cs.t_ms = t;
              cs.any = true;
            }
          }
        }
        appsrv::Inbound in;
        while (appsrv::next(in)) {
          if (in.kind == appsrv::Inbound::Kind::Connected) {
            AppSource src(reg, rules, schedule_table, timer_table, coord, t);
            app_last_fp = src.fingerprint();
            app_send_snapshot(in.fd, src);
          } else if (in.kind == appsrv::Inbound::Kind::Req) {
            app_handle_req(in, reg, rules, schedule_table, timer_table, t);
            std::free(in.text);
          } else {
            app_handle_cmd(in, reg, disp, t);
          }
        }
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
            const char* name = reg.name_of(e->entity_id);
            if (name != nullptr) {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "  0x%04x (%s) %s chip%u:",
                  static_cast<unsigned>(e->entity_id), name,
                  dt_name(e->device_type), static_cast<unsigned>(e->chip_id)));
            } else {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "  0x%04x %s chip%u:",
                  static_cast<unsigned>(e->entity_id),
                  dt_name(e->device_type), static_cast<unsigned>(e->chip_id)));
            }
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
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id)) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          const auto* e = reg.find(id);
          if (e == nullptr) {
            std::snprintf(r->out, r->out_cap, "0x%04x: not found\n",
                          static_cast<unsigned>(id));
          } else {
            const char* name = reg.name_of(id);
            std::size_t off = 0;
            if (name != nullptr) {
              off = static_cast<std::size_t>(std::snprintf(
                  r->out, r->out_cap, "0x%04x (%s) %s:", static_cast<unsigned>(id),
                  name, dt_name(e->device_type)));
            } else {
              off = static_cast<std::size_t>(std::snprintf(
                  r->out, r->out_cap, "0x%04x %s:", static_cast<unsigned>(id),
                  dt_name(e->device_type)));
            }
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
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id) || reg.find(id) == nullptr) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
          } else {
            std::uint16_t seq = 0;
            bool ok = disp.send(id, kOnOffAttr,
                                hubproto::Value::make_bool(r->val), t, seq);
            const char* name = reg.name_of(id);
            if (name != nullptr) {
              std::snprintf(r->out, r->out_cap, "%s 0x%04x (%s) -> %s (seq=%u)\n",
                            r->val ? "on" : "off", static_cast<unsigned>(id), name,
                            ok ? "sent" : "FAILED", static_cast<unsigned>(seq));
            } else {
              std::snprintf(r->out, r->out_cap, "%s 0x%04x -> %s (seq=%u)\n",
                            r->val ? "on" : "off", static_cast<unsigned>(id),
                            ok ? "sent" : "FAILED", static_cast<unsigned>(seq));
            }
          }
          break;
        }
        case REQ_TIMER: {
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id) || reg.find(id) == nullptr) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          time_t cur_epoch = 0;
          if (s_time_synced) time(&cur_epoch);
          const hubproto::Value action =
              r->idx == 1 ? hubproto::Value::make_u8(2) : hubproto::Value::make_bool(r->val);
          int slot = timer_table.schedule(id, kOnOffAttr, action,
                                          r->delay_ms, t,
                                          static_cast<std::uint64_t>(cur_epoch));
          if (slot < 0) {
            std::snprintf(r->out, r->out_cap, "no free timer slot\n");
          } else {
            persist_timers(timer_table, t);
            const char* name = reg.name_of(id);
            if (name != nullptr) {
              std::snprintf(r->out, r->out_cap,
                            "timer #%d: 0x%04x (%s) -> %s in %us\n", slot,
                            static_cast<unsigned>(id), name, timer_action_text(action),
                            static_cast<unsigned>(r->delay_ms / 1000u));
            } else {
              std::snprintf(r->out, r->out_cap,
                            "timer #%d: 0x%04x -> %s in %us\n", slot,
                            static_cast<unsigned>(id), timer_action_text(action),
                            static_cast<unsigned>(r->delay_ms / 1000u));
            }
          }
          break;
        }
        case REQ_TIMERS: {
          std::size_t off = 0;
          bool any = false;
          for (std::size_t i = 0; i < 8; ++i) {
            const auto* tm = timer_table.get(i);
            if (!tm) continue;
            any = true;
            std::uint32_t left = 0;
            if (tm->has_wall_deadline && s_time_synced) {
              time_t cur = time(nullptr);
              if (static_cast<std::uint64_t>(cur) < tm->deadline_epoch_s) {
                left = static_cast<std::uint32_t>(tm->deadline_epoch_s - static_cast<std::uint64_t>(cur));
              }
            } else {
              std::int32_t rem = static_cast<std::int32_t>(tm->when_monotonic_ms - t);
              left = rem > 0 ? static_cast<std::uint32_t>(rem / 1000) : 0;
            }
            const char* name = reg.name_of(tm->entity_id);
            if (name != nullptr) {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "#%u: 0x%04x (%s) -> %s in %us\n",
                  static_cast<unsigned>(i), static_cast<unsigned>(tm->entity_id), name,
                  timer_action_text(tm->value), static_cast<unsigned>(left)));
            } else {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "#%u: 0x%04x -> %s in %us\n",
                  static_cast<unsigned>(i), static_cast<unsigned>(tm->entity_id),
                  timer_action_text(tm->value), static_cast<unsigned>(left)));
            }
            if (off >= r->out_cap) break;
          }
          if (!any) std::snprintf(r->out, r->out_cap, "no pending timers\n");
          break;
        }
        case REQ_CANCEL: {
          if (r->idx >= 0 && r->idx < 8 && timer_table.cancel(static_cast<std::size_t>(r->idx))) {
            persist_timers(timer_table, t);
            std::snprintf(r->out, r->out_cap, "cancelled timer #%d\n", r->idx);
          } else {
            std::snprintf(r->out, r->out_cap, "no timer #%d\n", r->idx);
          }
          break;
        }
        case REQ_AT: {
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id) || reg.find(id) == nullptr) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          const hubproto::Value action =
              r->idx == 1 ? hubproto::Value::make_u8(2) : hubproto::Value::make_bool(r->val);
          const int minute = static_cast<int>(r->delay_ms);
          const int slot = schedule_table.add(id, kOnOffAttr, action, minute / 60, minute % 60,
                                              static_cast<std::uint8_t>(r->id));
          if (slot < 0) {
            std::snprintf(r->out, r->out_cap, "no free schedule slot\n");
            break;
          }
          persist_schedules(schedule_table);
          char dbuf[40];
          std::snprintf(r->out, r->out_cap, "schedule #%d: %02d:%02d %s 0x%04x -> %s%s\n", slot,
                        minute / 60, minute % 60,
                        days_text(static_cast<std::uint8_t>(r->id), dbuf, sizeof(dbuf)),
                        static_cast<unsigned>(id), timer_action_text(action),
                        s_time_synced ? "" : " (clock not synced yet — paused until it is)");
          break;
        }
        case REQ_SCHEDULES: {
          std::size_t off = 0;
          bool any = false;
          for (std::size_t i = 0; i < 8 && off < r->out_cap; ++i) {
            const auto* s = schedule_table.get(i);
            if (s == nullptr) continue;
            any = true;
            char dbuf[40];
            const char* name = reg.name_of(s->entity_id);
            off += static_cast<std::size_t>(std::snprintf(
                r->out + off, r->out_cap - off, "#%u: %02u:%02u %s  0x%04x%s%s%s -> %s\n",
                static_cast<unsigned>(i), static_cast<unsigned>(s->minute_of_day / 60),
                static_cast<unsigned>(s->minute_of_day % 60),
                days_text(s->weekdays, dbuf, sizeof(dbuf)),
                static_cast<unsigned>(s->entity_id), name != nullptr ? " (" : "",
                name != nullptr ? name : "", name != nullptr ? ")" : "",
                timer_action_text(s->value)));
          }
          if (!any) {
            std::snprintf(r->out, r->out_cap, "no schedules\n");
          } else if (!s_time_synced && off < r->out_cap) {
            std::snprintf(r->out + off, r->out_cap - off,
                          "(clock not synced — schedules are paused)\n");
          }
          break;
        }
        case REQ_UNSCHEDULE: {
          if (r->idx >= 0 && schedule_table.remove(static_cast<std::size_t>(r->idx))) {
            persist_schedules(schedule_table);
            std::snprintf(r->out, r->out_cap, "deleted schedule #%d\n", r->idx);
          } else {
            std::snprintf(r->out, r->out_cap, "no schedule #%d\n", r->idx);
          }
          break;
        }
        case REQ_CHIPS: {
          const std::uint32_t now = now_ms(nullptr);
          std::size_t off = 0;
          for (std::uint8_t id = 1; id <= 15 && off < r->out_cap; ++id) {
            const auto* ci = g_monitor->info(id);
            const bool known = coord.chip_known(id);
            if (ci == nullptr && !known) continue;
            const char* status = ci != nullptr && ci->silent ? "SILENT"
                                 : coord.chip_ready(id)      ? "ready"
                                 : known                     ? "seen (not ready)"
                                                             : "silent";
            const std::uint32_t age = ci != nullptr ? (now - ci->last_rx_ms) : 0;
            off += static_cast<std::size_t>(std::snprintf(
                r->out + off, r->out_cap - off,
                "chip 0x%02x (%s): %s, %lu frames, last rx %lus ago, boot_id %u, "
                "%lu reboot(s) seen (%u in 5 min)\n",
                id, chip_label(id), status,
                static_cast<unsigned long>(ci != nullptr ? ci->rx_frames : 0),
                static_cast<unsigned long>(age / 1000),
                static_cast<unsigned>(ci != nullptr ? ci->boot_id : 0),
                static_cast<unsigned long>(ci != nullptr ? ci->reboots : 0),
                g_monitor->recent_reboots(id, now)));
            hubproto::Value v{};
            if (reg.get_value(id, hubproto::chip_attr::kHeapFree, v)) {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "  telemetry: heap %lu, ",
                  static_cast<unsigned long>(v.as_u32())));
            }
            if (reg.get_value(id, hubproto::chip_attr::kUptimeS, v)) {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "uptime %lu s, ",
                  static_cast<unsigned long>(v.as_u32())));
            }
            if (reg.get_value(id, hubproto::chip_attr::kTxFrames, v)) {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "tx_frames %lu, ",
                  static_cast<unsigned long>(v.as_u32())));
            }
            if (reg.get_value(id, hubproto::chip_attr::kCrcErrors, v)) {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "crc_errors %u\n",
                  static_cast<unsigned>(v.as_u16())));
            } else if (reg.find(id) != nullptr) {
              off += static_cast<std::size_t>(std::snprintf(r->out + off, r->out_cap - off, "\n"));
            }
          }
          if (off == 0) std::snprintf(r->out, r->out_cap, "no chips seen yet\n");
          break;
        }
        case REQ_LOG: {
          esp_log_level_set(kTag, r->val ? ESP_LOG_DEBUG : ESP_LOG_INFO);
          std::snprintf(r->out, r->out_cap, "verbose log %s\n",
                        r->val ? "ON" : "OFF");
          break;
        }
        // Chip-meta commands: build a raw Command targeting entity_id=0 on the
        // owning chip. Convention: attr_id picks the op, u16 value is the arg.
        // Mirrored in zigbee-c6::handle_meta.
        case REQ_PAIR: {
          // No entity to look up — pair always targets the Zigbee coordinator.
          const std::uint16_t sec = r->id;
          std::uint8_t payload[5] = {0, 0, 0x01,
                                     static_cast<std::uint8_t>(sec & 0xFF),
                                     static_cast<std::uint8_t>((sec >> 8) & 0xFF)};
          hubproto::Header h{};
          h.version = hubproto::kProtocolVersion;
          h.type = hubproto::MsgType::Command;
          h.chip_id = kZigbeeChipId;
          h.entity_id = hubproto::kSelfEntityId;
          bus.publish(h, payload, sizeof(payload));
          std::snprintf(r->out, r->out_cap,
                        "pair -> chip 0x%02x for %us (sent)\n",
                        kZigbeeChipId, static_cast<unsigned>(sec));
          break;
        }
        case REQ_UNPAIR:
        case REQ_IDENTIFY: {
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id)) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          const auto* e = reg.find(id);
          if (e == nullptr) {
            std::snprintf(r->out, r->out_cap, "0x%04x: not found\n",
                          static_cast<unsigned>(id));
            break;
          }
          const std::uint8_t meta_attr = (r->kind == REQ_UNPAIR) ? 0x02 : 0x03;
          std::uint8_t payload[5] = {0, 0, meta_attr,
                                     static_cast<std::uint8_t>(id & 0xFF),
                                     static_cast<std::uint8_t>((id >> 8) & 0xFF)};
          hubproto::Header h{};
          h.version = hubproto::kProtocolVersion;
          h.type = hubproto::MsgType::Command;
          h.chip_id = e->chip_id;
          h.entity_id = hubproto::kSelfEntityId;
          bus.publish(h, payload, sizeof(payload));
          const char* name = reg.name_of(id);
          if (name != nullptr) {
            std::snprintf(r->out, r->out_cap, "%s 0x%04x (%s) -> chip 0x%02x (sent)\n",
                          r->kind == REQ_UNPAIR ? "unpair" : "identify",
                          static_cast<unsigned>(id), name, e->chip_id);
          } else {
            std::snprintf(r->out, r->out_cap, "%s 0x%04x -> chip 0x%02x (sent)\n",
                          r->kind == REQ_UNPAIR ? "unpair" : "identify",
                          static_cast<unsigned>(id), e->chip_id);
          }
          break;
        }
        case REQ_WIFI_JOIN: {
          wifi_config_t wifi_config = {};
          std::strncpy((char *)wifi_config.sta.ssid, r->ssid, sizeof(wifi_config.sta.ssid));
          std::strncpy((char *)wifi_config.sta.password, r->pass, sizeof(wifi_config.sta.password));
          wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

          ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
          ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
          esp_wifi_connect();
          std::snprintf(r->out, r->out_cap, "connecting to %s...\n", r->ssid);
          break;
        }
        case REQ_WIFI_STATUS: {
          wifi_ap_record_t ap_info;
          esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
          if (err == ESP_OK) {
            esp_netif_ip_info_t ip_info;
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_get_ip_info(netif, &ip_info);
            std::snprintf(r->out, r->out_cap, "connected to %s, rssi=%d, ip=" IPSTR "\n",
                          ap_info.ssid, ap_info.rssi, IP2STR(&ip_info.ip));
          } else {
            std::snprintf(r->out, r->out_cap, "not connected (err=0x%x)\n", err);
          }
          break;
        }
        case REQ_WIFI_SCAN: {
          wifi_scan_config_t scan_config = {};
          esp_err_t err = esp_wifi_scan_start(&scan_config, true);
          if (err == ESP_OK) {
             uint16_t ap_count = 0;
             esp_wifi_scan_get_ap_num(&ap_count);
             wifi_ap_record_t* ap_list = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
             ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));
             std::size_t off = 0;
             off += std::snprintf(r->out + off, r->out_cap - off, "Found %u APs:\n", ap_count);
             for (int i = 0; i < ap_count && off < r->out_cap; i++) {
                 off += std::snprintf(r->out + off, r->out_cap - off, "  %s (RSSI: %d)\n", ap_list[i].ssid, ap_list[i].rssi);
             }
             if (off >= r->out_cap) std::snprintf(r->out + r->out_cap - 5, 5, "...\n");
             free(ap_list);
          } else {
             std::snprintf(r->out, r->out_cap, "scan failed (err=0x%x)\n", err);
          }
          break;
        }
        case REQ_TIME: {
          time_t now = 0;
          time(&now);
          struct tm timeinfo{};
          localtime_r(&now, &timeinfo);
          char time_str[64];
          strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
          const char* tz = getenv("TZ");
          std::snprintf(r->out, r->out_cap, "time: %s\nsynced: %s\ntz: %s\nuptime: %llus\n",
                        time_str, s_time_synced ? "yes" : "no",
                        tz ? tz : "UTC",
                        static_cast<unsigned long long>(esp_timer_get_time() / 1000000ULL));
          break;
        }
        case REQ_TZ: {
          setenv("TZ", r->tz, 1);
          tzset();
          time_t now = 0;
          time(&now);
          struct tm timeinfo{};
          localtime_r(&now, &timeinfo);
          char time_str[64];
          strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
          std::snprintf(r->out, r->out_cap, "tz set to %s, current time: %s\n", r->tz, time_str);
          break;
        }
        case REQ_RULE:
          handle_rule(r, reg, rules);
          break;
        case REQ_DESIRED: {
          xSemaphoreTakeRecursive(g_rec_mu, portMAX_DELAY);
          auto& des = reconciler.desired();
          if (r->argc >= 2 && std::strcmp(r->argv[1], "clear") == 0 && r->argc == 3) {
            std::uint16_t id = 0;
            if (std::strcmp(r->argv[2], "all") == 0) {
              des.clear_all();
              std::snprintf(r->out, r->out_cap, "all desired values cleared\n");
            } else if (!reg.resolve(r->argv[2], id)) {
              std::snprintf(r->out, r->out_cap, "%s: not found\n", r->argv[2]);
            } else {
              const std::size_t n = des.clear_entity(id);
              std::snprintf(r->out, r->out_cap, "0x%04x: %u desired value(s) cleared\n",
                            static_cast<unsigned>(id), static_cast<unsigned>(n));
            }
            reconciler.note_dirty();
          } else {
            std::size_t off = static_cast<std::size_t>(std::snprintf(
                r->out, r->out_cap, "%u desired value(s):\n",
                static_cast<unsigned>(des.count())));
            for (std::size_t i = 0; i < des.count() && off < r->out_cap; ++i) {
              const auto* e = des.at(i);
              char t[48], v[24];
              fmt_target(t, sizeof(t), reg, e->entity_id, e->attr_id);
              fmt_value(v, sizeof(v), e->value.type, e->value);
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "  %s = %s\n", t, v));
            }
          }
          xSemaphoreGiveRecursive(g_rec_mu);
          break;
        }
        case REQ_NAME_SET: {
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id)) {
            std::snprintf(r->out, r->out_cap, "invalid target: %s\n", r->target);
            break;
          }
          if (reg.set_name(id, r->name)) {
            persist_names(reg.names());
            std::snprintf(r->out, r->out_cap, "0x%04x -> \"%s\"\n",
                          static_cast<unsigned>(id), r->name);
          } else {
            std::snprintf(r->out, r->out_cap,
                          "failed to set name (already in use, invalid or table full)\n");
          }
          break;
        }
        case REQ_NAME_CLEAR: {
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id)) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          if (reg.clear_name(id)) {
            persist_names(reg.names());
            std::snprintf(r->out, r->out_cap, "cleared name for 0x%04x\n",
                          static_cast<unsigned>(id));
          } else {
            std::snprintf(r->out, r->out_cap, "0x%04x has no name\n",
                          static_cast<unsigned>(id));
          }
          break;
        }
        case REQ_NAMES_LIST: {
          const auto& names = reg.names();
          if (names.size() == 0) {
            std::snprintf(r->out, r->out_cap, "no named entities\n");
            break;
          }
          std::size_t off = 0;
          off += static_cast<std::size_t>(std::snprintf(
              r->out + off, r->out_cap - off, "%u named entities:\n",
              static_cast<unsigned>(names.size())));
          for (std::size_t i = 0; i < names.size() && off < r->out_cap; ++i) {
            core::EntityRegistry<>::Names::Entry entry{};
            if (!names.entry_at(i, entry)) break;
            off += static_cast<std::size_t>(std::snprintf(
                r->out + off, r->out_cap - off, "  0x%04x -> %s\n",
                static_cast<unsigned>(entry.id), entry.name));
          }
          break;
        }
      }
      xSemaphoreGive(r->done);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
