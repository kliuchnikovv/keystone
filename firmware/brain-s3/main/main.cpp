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
#include "core/rule_engine.hpp"
#include "core/timer_table.hpp"
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
    esp_netif_create_default_wifi_sta();

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

const char* kRuleUsage =
    "usage:\n"
    "  rule                     - list rules\n"
    "  rule add <entity> <attr> <op> <value> then <entity> <attr> <value>\n"
    "  rule add <entity> <attr> any then <entity> <attr> <value>   (any change)\n"
    "  rule del <n>\n"
    "  <op>: == != < <= > >=   <attr>: on_off power_w energy_wh or a number\n"
    "  e.g. rule add plug power_w > 1500 then plug on_off off\n";

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
          out + off, cap - off, "  #%u  %s %s %s  ->  %s = %s\n",
          static_cast<unsigned>(i), t, op_str(ru->op), ov, a, av));
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
    if (argc != base + 4 || std::strcmp(argv[base], "then") != 0) {
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

int cmd_in(int argc, char** argv) {
  if (argc < 4) { printf("usage: in <sec> <on|off> <id_or_name>\n"); return 1; }
  const long sec = std::strtol(argv[1], nullptr, 10);
  const bool on = std::strcmp(argv[2], "on") == 0;
  char buf[128];
  Req r{};
  r.kind = REQ_TIMER;
  r.target = argv[3];
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

  disp.on_result(&on_cmd_result, nullptr);
  bus.subscribe(&on_bus_traffic, nullptr);  // per-chip rx counter (silent unless `log on`)
  bus.subscribe(&on_bus_forward, &bus);     // Matter <-> Zigbee cross-chip mirror

  restore_rules(rules);  // user rules from NVS (`rule add ...`)

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
      std::uint32_t now_ms;
      bool          fired;
    };
    TimerFireContext fc{&disp, t, false};
    timer_table.poll(t, s_time_synced ? static_cast<std::uint64_t>(time(nullptr)) : 0,
                     [](void* ctx, std::size_t slot, std::uint16_t entity_id,
                        std::uint8_t attr_id, const hubproto::Value& val) {
                       auto* c = static_cast<TimerFireContext*>(ctx);
                       c->fired = true;
                       std::uint16_t seq = 0;
                       c->disp->send(entity_id, attr_id, val, c->now_ms, seq);
                       ESP_LOGI(kTag, "timer #%u fired: entity 0x%04x attr 0x%02x (seq=%u)",
                                static_cast<unsigned>(slot), static_cast<unsigned>(entity_id),
                                static_cast<unsigned>(attr_id), static_cast<unsigned>(seq));
                     }, &fc);
    if (fc.fired) {
      persist_timers(timer_table, t);
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
          int slot = timer_table.schedule(id, kOnOffAttr,
                                          hubproto::Value::make_bool(r->val),
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
                            static_cast<unsigned>(id), name, r->val ? "on" : "off",
                            static_cast<unsigned>(r->delay_ms / 1000u));
            } else {
              std::snprintf(r->out, r->out_cap,
                            "timer #%d: 0x%04x -> %s in %us\n", slot,
                            static_cast<unsigned>(id), r->val ? "on" : "off",
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
                  tm->value.as_bool() ? "on" : "off", static_cast<unsigned>(left)));
            } else {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "#%u: 0x%04x -> %s in %us\n",
                  static_cast<unsigned>(i), static_cast<unsigned>(tm->entity_id),
                  tm->value.as_bool() ? "on" : "off", static_cast<unsigned>(left)));
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
