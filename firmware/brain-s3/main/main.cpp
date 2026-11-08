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
//   at <YYYY-MM-DD> <HH:MM> <on|off|toggle> <id>   the same, once, on that date
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
#include <utility>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "esp_mac.h"
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
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "esp_uart_link.hpp"
#include "core/uart_transport.hpp"
#include "device_ota_job.hpp"
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
#include <cmath>
#include "core/scene_table.hpp"
#include "core/sun.hpp"
#include "core/setup_mode.hpp"
#include "core/thread_dataset.hpp"
#include "core/wifi_policy.hpp"
#include "matter_paa.hpp"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "app_server.hpp"
#include "core/app_protocol.hpp"
#include "core/zcl_profile.hpp"
#include "core/app_requests.hpp"
#include "core/event_log.hpp"
#include "ota_job.hpp"
#include "relay_client.hpp"
#include "status_led.hpp"

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

// experiment/commissioner-on-s3: when the Docker build links esp_matter in,
// brain-s3 also runs the Matter controller (CASE/PASE + BLE commissioner).
// PlatformIO builds without KEYSTONE_HAVE_MATTER and skips all of this.
#ifdef KEYSTONE_HAVE_MATTER
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
// lwIP for the IPv6 diagnostics of the Matter path (see cmd_ip6). These have to
// be included here, outside the anonymous namespace: otherwise lwIP's types
// (struct netif, struct ip6_addr_t) would land in it.
#include <arpa/inet.h>
#include "lwip/errno.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#endif

#ifdef KEYSTONE_HAVE_TLS
// The setup code lives in the generated hub_identity.cpp (scripts/gen-hub-cert.sh). An identity made before
// setup mode existed does not have it: the symbol is weak, so that firmware still links and simply offers no
// setup access point.
extern const char kHubSetupCode[] __attribute__((weak));
#endif

namespace {

const char* kTag = "brain-s3";

static bool s_sntp_initialized = false;
static bool s_time_synced = false;
std::atomic<bool> g_hub_dirty{false};  // something the apps show about the hub itself changed (hub event)
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

// ---- Wi-Fi --------------------------------------------------------------------------------
// Credentials: the ones saved in NVS (`wifi_join`, the app's `wifi.set`), else the image's built-in
// default (main/wifi_secrets.h, for development), else none. A change of network is a *trial*: the
// new credentials are saved only once the hub got an address on that network, otherwise it goes
// back to the old ones (core::WifiTrial), so a typo cannot strand the hub.
struct WifiCreds {
  char ssid[core::kMaxSsidLen + 1] = {};
  char pass[core::kMaxPasswordLen + 2] = {};  // 64 hex digits (a raw PSK) fit too
};

static std::atomic<bool> s_wifi_up{false};            // has an address
static std::atomic<unsigned> s_wifi_got_ip{0};        // how many times an address was obtained
static std::atomic<unsigned> s_wifi_attempt{0};       // consecutive failed connects (back-off)
static std::atomic<bool> s_wifi_have_creds{false};
static WifiCreds s_wifi_active;                       // what is configured now (main loop / init only)
static esp_timer_handle_t s_wifi_retry = nullptr;

static void wifi_retry_cb(void*) {
    if (s_wifi_have_creds && !s_wifi_up) esp_wifi_connect();
}

static constexpr const char* kWifiSsidKey = "wifi_ssid";
static constexpr const char* kWifiPassKey = "wifi_pass";

static void wifi_creds_save(const WifiCreds& c) {
    nvs_handle_t h;
    if (nvs_open("keystone", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, kWifiSsidKey, c.ssid);
    nvs_set_str(h, kWifiPassKey, c.pass);
    nvs_commit(h);
    nvs_close(h);
}

static bool wifi_creds_load(WifiCreds& c) {
    nvs_handle_t h;
    if (nvs_open("keystone", NVS_READONLY, &h) != ESP_OK) return false;
    std::size_t sl = sizeof(c.ssid), pl = sizeof(c.pass);
    const bool ok = nvs_get_str(h, kWifiSsidKey, c.ssid, &sl) == ESP_OK && nvs_get_str(h, kWifiPassKey, c.pass, &pl) == ESP_OK &&
                    c.ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static void wifi_creds_erase() {
    nvs_handle_t h;
    if (nvs_open("keystone", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, kWifiSsidKey);
    nvs_erase_key(h, kWifiPassKey);
    nvs_commit(h);
    nvs_close(h);
}

// The Matter controller is created only once the Wi-Fi interface is up and
// has a usable IPv6 link-local address (see matter_controller_start below):
// CHIP binds its mDNS endpoints when the controller is created, and it can
// only do that on an interface that has an address.
#ifdef KEYSTONE_HAVE_MATTER
static void matter_controller_start();
static void matter_controller_task(void* arg);

// Woken when the controller task is done: the main loop waits for it, so that
// the CHIP controller is created while the internal heap is still one big
// block (see matter_controller_task).
static SemaphoreHandle_t g_matter_ctrl_done = nullptr;

// True once the CHIP controller exists. It is created only when the Wi-Fi
// interface has its IPv6 address (see matter_controller_start), so until then
// there is no commissioner and no mDNS endpoint — a request would be accepted
// and then silently dropped.
static bool g_matter_controller_ready = false;

// Why the controller is not up (or that it is). The boot log says it once and
// then the line scrolls away, while the console stays open for hours — so the
// reason is kept and shown with the next commissioning request.
//
// A pointer, not a copy: the strings are literals that are in flash anyway,
// and this image has only a few hundred bytes of room in its app partition.
static const char* g_matter_status = "not started";

static void matter_set_status(const char* what) { g_matter_status = what; }
#endif

// The IPv6 link-local address of the Wi-Fi interface.
//
// lwIP does NOT create it by itself — the application has to ask for it
// (esp_netif_create_ip6_linklocal). Matter needs one, and it is not a detail:
// CHIP's own mDNS server binds one UDP endpoint per interface that has an
// IPv6 address, and it binds them ONCE, when the controller is created. With
// no IPv6 address on the interface it binds nothing at all, and the DNS-SD
// query for the device then fails with CHIP_ERROR_NO_ENDPOINT (0x46) — the
// commissioning died there with "Device connection failed. Error 46".
//
// The address is derived from the interface's hardware address, so it can only
// be made after esp_wifi_init() has given the netif its MAC.
//
// Creating it twice would restart lwIP's duplicate address detection, so it is
// done once and only when the interface is up.
static bool s_wifi_ipv6_created = false;

static void wifi_ipv6_linklocal() {
    if (s_wifi_ipv6_created) {
        return;
    }
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        ESP_LOGW(kTag, "no Wi-Fi netif; no IPv6 link-local address");
        return;
    }
    const esp_err_t err = esp_netif_create_ip6_linklocal(netif);
    if (err != ESP_OK) {
        // The interface is not up yet — the caller tries again later. Quiet
        // here on purpose: the wait below polls, and a warning per poll would
        // bury the boot log.
        return;
    }
    s_wifi_ipv6_created = true;
    ESP_LOGI(kTag, "Wi-Fi IPv6 link-local address created (waiting for DAD)");
}

// Wait until the Wi-Fi interface has a *usable* IPv6 link-local address.
//
// The address is made here if it is not there yet: the interface comes up with
// the Wi-Fi driver, and that happens a moment after the Matter stack starts it
// — so the caller cannot assume the address is there.
//
// A fresh lwIP address is "tentative" until the duplicate address detection
// has finished (about a second), and esp_netif_get_ip6_linklocal() only hands
// out a "preferred" one. CHIP's mDNS skips a tentative address, so the
// controller must not be created before this returns true.
static bool wait_wifi_ipv6_linklocal(std::uint32_t timeout_ms) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        ESP_LOGW(kTag, "no Wi-Fi netif; no IPv6 link-local address");
        return false;
    }
    for (std::uint32_t waited = 0; waited <= timeout_ms; waited += 100) {
        wifi_ipv6_linklocal();
        esp_ip6_addr_t ll{};
        if (esp_netif_get_ip6_linklocal(netif, &ll) == ESP_OK) {
            ESP_LOGI(kTag, "Wi-Fi IPv6 link-local: " IPV6STR, IPV62STR(ll));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(kTag, "the Wi-Fi interface has no usable IPv6 link-local address after %u ms",
             static_cast<unsigned>(timeout_ms));
    return false;
}

// Обработчик событий WiFi
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_have_creds) {
            ESP_LOGI(kTag, "WiFi STA Start. Connecting...");
            esp_wifi_connect();
        } else {
            ESP_LOGW(kTag, "WiFi STA Start: no credentials (set them with `wifi_join <ssid> <pass>`)");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        // esp_netif has already brought the interface up (its own handler runs
        // first), so the IPv6 link-local address can be made now.
        wifi_ipv6_linklocal();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
        s_wifi_up = false;
        g_hub_dirty = true;
        if (!s_wifi_have_creds) return;
        const std::uint32_t delay = core::wifi_reconnect_delay_ms(s_wifi_attempt.fetch_add(1));
        ESP_LOGW(kTag, "WiFi disconnected. Reason: %d. Retrying in %u ms", disconn->reason, static_cast<unsigned>(delay));
        if (s_wifi_retry != nullptr) {
            esp_timer_stop(s_wifi_retry);
            esp_timer_start_once(s_wifi_retry, static_cast<std::uint64_t>(delay) * 1000u);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(kTag, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_attempt = 0;
        s_wifi_up = true;
        ++s_wifi_got_ip;
        g_hub_dirty = true;
        sntp_init();
        // The interface is up now: give it an IPv6 link-local address (Matter
        // needs one, see wifi_ipv6_linklocal). The task that creates the
        // controller waits for that address on its own — CHIP binds its DNS-SD
        // endpoints on the interfaces that are up at the moment the controller
        // is created, and it needs a *usable* (preferred) IPv6 address there.
        wifi_ipv6_linklocal();
    }
}

// Make `c` the network the station joins (now). Does not save anything.
static void wifi_apply(const WifiCreds& c) {
    if (s_wifi_retry != nullptr) esp_timer_stop(s_wifi_retry);
    s_wifi_attempt = 0;
    s_wifi_up = false;
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), c.ssid, sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), c.pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = c.pass[0] != '\0' ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    s_wifi_active = c;
    s_wifi_have_creds = true;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}

// The network the hub is on, for `wifi_status` and the apps (never the password).
static bool wifi_current(char* ssid, std::size_t ssid_cap, int& rssi, char* ip, std::size_t ip_cap) {
    wifi_ap_record_t ap{};
    if (!s_wifi_up || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    std::snprintf(ssid, ssid_cap, "%s", reinterpret_cast<const char*>(ap.ssid));
    rssi = ap.rssi;
    esp_netif_ip_info_t info{};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    std::snprintf(ip, ip_cap, IPSTR, IP2STR(&info.ip));
    return true;
}

// Phase 1: NVS + netif + default event loop + default STA/AP netifs. Cheap
// — none of this allocates the big Wi-Fi driver buffers. Split out of
// wifi_init() so matter_bringup() can run between the two phases: that way
// the BLE controller's ~30 KB contiguous allocation competes with Wi-Fi only
// once, not twice (docs/matter-backlog.md §3, step 1a of the R3 path).
static void wifi_bootstrap(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, kHubHostname);
    esp_netif_create_default_wifi_ap();  // only used while the setup access point is up
}

// Phase 2: Wi-Fi driver init/config/start. On the Matter build, esp_matter::start()
// already called esp_wifi_init() and esp_wifi_start() internally; we skip both
// and only apply our own credentials + event handlers on top. On PlatformIO
// (no Matter) this phase does the full init itself.
static void wifi_start_driver(void) {
#ifndef KEYSTONE_HAVE_MATTER
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
#endif
    // Our own NVS key is the single source of truth, not the driver's copy (a trial must not stick).
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const esp_timer_create_args_t ta = {.callback = &wifi_retry_cb, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "wifi_retry",
                                        .skip_unhandled_events = true};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_wifi_retry));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Credentials: saved ones, else the built-in development default, else none.
    WifiCreds creds;
    const char* source = "saved";
    if (!wifi_creds_load(creds)) {
        creds = WifiCreds{};
        std::strncpy(creds.ssid, KEYSTONE_WIFI_SSID, sizeof(creds.ssid) - 1);
        std::strncpy(creds.pass, KEYSTONE_WIFI_PASSWORD, sizeof(creds.pass) - 1);
        source = "built-in default";
    }
#ifndef KEYSTONE_HAVE_MATTER
    // Matter sets STA mode itself when it starts the Wi-Fi stack.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif
    if (creds.ssid[0] != '\0') {
        wifi_config_t wc = {};
        std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), creds.ssid, sizeof(wc.sta.ssid) - 1);
        std::strncpy(reinterpret_cast<char*>(wc.sta.password), creds.pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = creds.pass[0] != '\0' ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        s_wifi_active = creds;
        s_wifi_have_creds = true;
        ESP_LOGI(kTag, "Wi-Fi: joining %s (%s credentials)", creds.ssid, source);
    } else {
        ESP_LOGW(kTag, "Wi-Fi: no credentials yet; use `wifi_join <ssid> <password>`");
    }
#ifdef KEYSTONE_HAVE_MATTER
    // Matter already called esp_wifi_start() during esp_matter::start(); calling
    // it again returns ESP_ERR_WIFI_NOT_INIT or similar. Just connect with our creds.
    if (creds.ssid[0] != '\0') {
      (void)esp_wifi_connect();  // first attempt; event handler will retry on failure
    }
#else
    ESP_ERROR_CHECK(esp_wifi_start());
#endif
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

  // How many subscribers the fan-out holds right now.
  int subscriber_count() const noexcept { return n_subs_; }

  // How many subscribers fit. Who subscribes today: the radio coordinator, the
  // state machine, the dispatcher, the chip monitor, the OTA verifier, the two
  // OTA senders (they subscribe through the jobs' trampolines), the traffic
  // counter and the cross-chip forwarder — nine. The table was eight, so the
  // last one was silently dropped and the Thread dataset never reached the
  // Matter commissioner.
  static constexpr int kMaxSubs = 12;

  bool subscribe(MessageFn fn, void* ctx) noexcept override {
    if (n_subs_ >= kMaxSubs) {
      // Loud on purpose. A subscriber that does not fit is a feature that is
      // silently broken — that is exactly how the Thread dataset never reached
      // the Matter commissioner (the forwarder was the ninth subscriber of an
      // eight-slot table, and nobody looked at the return value).
      ESP_LOGE(kTag, "the bus fan-out table is full (%d subscribers) — this subscriber is dropped",
               kMaxSubs);
      return false;
    }
    subs_[n_subs_] = {fn, ctx};
    ++n_subs_;
    return true;
  }

 private:
  static constexpr int kMaxBinds = 4;

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
// ---- Event history (core/event_log.hpp) ------------------------------------------------
// One producer path for "what happened": everything that used to be only a log line also builds an
// Event here. The ring answers the app's `history` and the console's `events`; another sink (a flash
// file, the SD recorder) would join the fanout. Producers run in the UART RX tasks and the main loop,
// readers in the main loop and the console task, hence the mutex.
// The ring is ~6 KB of `Event buf_[256]` — plain POD under a virtual-table
// pointer. It deliberately stays in internal DRAM instead of
// `EXT_RAM_BSS_ATTR` (.ext_ram.bss = PSRAM): `EventRing` is polymorphic, and
// a namespace-scope object whose constructor is constexpr-able is
// *constant-initialised* by GCC — the vtable pointer is written into the
// object's own section. .ext_ram.bss is a NOLOAD section (zeroed at boot, no
// image), so that constant never reaches RAM: the vptr stays 0 and the first
// virtual call faults (LoadProhibited at 0x8). That is why only plain data
// arrays and function-local statics carry the attribute below: an array is
// all-zero either way, and a function-local static runs its constructor at
// runtime (under a guard), which writes the vptr properly.
core::EventRing<256> g_ev_ring;
core::EventFanout<4> g_ev_out;
core::StateDedup<64> g_ev_state_seen;  // a restarted chip re-reports what has not changed: not news
core::ChipEventGate g_ev_chip_gate;    // a crash loop must not fill the ring with "rebooted"
SemaphoreHandle_t g_ev_mu = nullptr;

// Who is making the hub send a command, for the `command` event. A command is built and sent in one call
// on one task, so a per-task value set around the call is enough; rules are the one producer that sends from
// inside the engine, so the fire hook sets it and the send observer takes it.
thread_local core::CmdSource t_cmd_src = core::CmdSource::Unknown;
struct CmdSrcScope {
  core::CmdSource prev;
  explicit CmdSrcScope(core::CmdSource s) : prev(t_cmd_src) { t_cmd_src = s; }
  ~CmdSrcScope() { t_cmd_src = prev; }
};
core::CmdSource cmd_src_of(const char* kind) {
  if (std::strcmp(kind, "rule") == 0) return core::CmdSource::Rule;
  if (std::strcmp(kind, "schedule") == 0) return core::CmdSource::Schedule;
  if (std::strcmp(kind, "timer") == 0) return core::CmdSource::Timer;
  return std::strcmp(kind, "app") == 0 ? core::CmdSource::App : core::CmdSource::Console;
}

void ev_emit(core::Event e) {
  if (g_ev_mu == nullptr) return;
  e.t_s = static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
  xSemaphoreTake(g_ev_mu, portMAX_DELAY);
  g_ev_out.emit(e);
  xSemaphoreGive(g_ev_mu);
}

void ev_attr(core::EventKind kind, std::uint16_t entity, std::uint8_t attr, const hubproto::Value* v,
             std::uint8_t code = 0, std::uint16_t ref = 0) {
  core::Event e;
  e.kind = kind;
  e.entity = entity;
  e.attr = attr;
  e.code = code;
  e.ref = ref;
  if (v != nullptr) {
    e.has_value = true;
    e.value = *v;
  }
  ev_emit(e);
}

void ev_plain(core::EventKind kind, std::uint8_t code, std::uint16_t ref, std::uint16_t entity = 0) {
  core::Event e;
  e.kind = kind;
  e.code = code;
  e.ref = ref;
  e.entity = entity;
  ev_emit(e);
}

core::SceneVia scene_via(const char* kind) {
  if (std::strcmp(kind, "rule") == 0) return core::SceneVia::Rule;
  if (std::strcmp(kind, "schedule") == 0) return core::SceneVia::Schedule;
  if (std::strcmp(kind, "timer") == 0) return core::SceneVia::Timer;
  return std::strcmp(kind, "app") == 0 ? core::SceneVia::App : core::SceneVia::Console;
}

// A state report becomes an event only if it is one worth keeping and it is a change.
void ev_state(std::uint16_t entity_id, std::uint8_t attr_id, const hubproto::Value& v) {
  if (g_ev_mu == nullptr || !core::state_is_historic(entity_id, attr_id, v)) return;
  core::Event e;
  e.kind = core::EventKind::State;
  e.entity = entity_id;
  e.attr = attr_id;
  e.has_value = true;
  e.value = v;
  e.t_s = static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
  xSemaphoreTake(g_ev_mu, portMAX_DELAY);
  if (g_ev_state_seen.changed(entity_id, attr_id, v)) g_ev_out.emit(e);
  xSemaphoreGive(g_ev_mu);
}

void on_state_change(void* /*ctx*/, std::uint16_t entity_id, std::uint8_t attr_id,
                     const hubproto::Value& v) {
  app_on_state(nullptr, entity_id, attr_id, v);
  ev_state(entity_id, attr_id, v);
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
// The hub's bus, so a console command can talk to a chip without going through
// the main loop (the Matter commissioner needs to ask the co-processor for its
// Thread dataset and wait for the answer).
core::Bus* g_bus = nullptr;  // set in app_main

const char* chip_label(std::uint8_t id) {
  return id == 1 ? "zigbee" : id == 2 ? "thread" : "?";
}

void on_chip_event(void* /*ctx*/, std::uint8_t chip, core::ChipMonitor::Event ev,
                   std::uint32_t value) {
  {
    // Seconds of silence for Lost/Resumed, the reboot count for Rebooted/Flapping.
    const bool secs = ev == core::ChipMonitor::Event::Lost || ev == core::ChipMonitor::Event::Resumed;
    const hubproto::Value arg = hubproto::Value::make_u32(secs ? value / 1000 : value);
    const core::ChipEv code = ev == core::ChipMonitor::Event::Lost       ? core::ChipEv::Lost
                              : ev == core::ChipMonitor::Event::Resumed  ? core::ChipEv::Resumed
                              : ev == core::ChipMonitor::Event::Rebooted ? core::ChipEv::Rebooted
                                                                         : core::ChipEv::Flapping;
    bool record = true;
    if (g_ev_mu != nullptr) {
      xSemaphoreTake(g_ev_mu, portMAX_DELAY);
      record = g_ev_chip_gate.allow(chip, code, static_cast<std::uint32_t>(esp_timer_get_time() / 1000000));
      xSemaphoreGive(g_ev_mu);
    }
    if (record) ev_attr(core::EventKind::Chip, 0, 0, &arg, static_cast<std::uint8_t>(code), chip);
  }
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
  if (status != hubproto::CmdAckStatus::Ok) {
    ev_attr(core::EventKind::CmdFailed, entity_id, attr_id, nullptr, static_cast<std::uint8_t>(status));
  }
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
  ev_attr(core::EventKind::Command, entity, attr, &v, static_cast<std::uint8_t>(t_cmd_src));
  if (t_cmd_src == core::CmdSource::Rule) t_cmd_src = core::CmdSource::Unknown;  // set by the fire hook for this one send
  xSemaphoreTakeRecursive(g_rec_mu, portMAX_DELAY);
  g_rec->note_command(entity, attr, v);
  xSemaphoreGiveRecursive(g_rec_mu);
}

void rec_on_report(void* /*ctx*/, std::uint16_t entity, std::uint8_t attr,
                   const hubproto::Value& v, bool had, bool changed) {
  CmdSrcScope src(core::CmdSource::Reconcile);  // a corrective command is sent from inside on_report
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
  ev_attr(core::EventKind::Reconcile, entity, attr, &actual,
          static_cast<std::uint8_t>(kind == Reconciler::Kind::Corrected ? core::ReconcileEv::Corrected
                                                                         : core::ReconcileEv::Adopted));
  if (kind == Reconciler::Kind::Corrected) {
    ESP_LOGW(kTag, "reconcile: 0x%04x %s is %s but the hub last set %s -> corrective COMMAND sent",
             static_cast<unsigned>(entity), an ? an : "attr", a, d);
  } else {
    ESP_LOGI(kTag, "reconcile: 0x%04x %s changed to %s outside the hub (was %s) -> adopted",
             static_cast<unsigned>(entity), an ? an : "attr", a, d);
  }
}

// ---- Matter commissioning ---------------------------------------------------------------------
// The phone reads a Matter device over Bluetooth and hands the hub the bytes it read; the Matter
// chip (the only one with a stack that can commission anything) does the work and says how far it
// got. That answer arrives in the UART RX task, so here it is only queued — the main loop owns the
// cache and the clients.
struct MatterUpdate {
  hubproto::MatterState state = hubproto::MatterState::Idle;
  hubproto::MatterCode code = hubproto::MatterCode::Ok;
  std::uint16_t entity = 0;
};
QueueHandle_t g_matter_q = nullptr;
// Has the Matter chip ever said anything? Without it there is nothing to commission with.
std::atomic<bool> g_thread_seen{false};

#ifdef KEYSTONE_HAVE_MATTER
// The Thread operational dataset the commissioner hands to the device over BLE.
// The brain has no 802.15.4 radio, so the dataset comes from the co-processor
// that has one (hubproto ThreadDatasetReq/ThreadDataset). It lives in a
// static, not on a stack: CHIP keeps a pointer to the bytes for the whole
// commissioning, and that takes minutes.
static core::ThreadDataset g_thread_dataset;
static SemaphoreHandle_t g_thread_dataset_sem = nullptr;
#endif

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
  if (h.chip_id == kThreadChipId) g_thread_seen = true;  // the Matter chip is alive
  // Zigbee -> Matter mirror: DECLARE (so Matter can create a bridged
  // endpoint), STATE (so it can update the Matter attribute). No need to
  // mirror DECLARE_END — Matter Bridge builds endpoints incrementally as
  // DECLAREs arrive, no batching required.
  if (h.chip_id == kZigbeeChipId &&
      (h.type == hubproto::MsgType::Declare ||
       h.type == hubproto::MsgType::State ||
       h.type == hubproto::MsgType::Undeclare)) {  // a device left: drop its Matter endpoint
    hubproto::Header mirror = h;
    mirror.chip_id = kThreadChipId;
    bus->publish(mirror, p, len);
  } else if (h.type == hubproto::MsgType::Hello && h.chip_id == kThreadChipId) {
    // Matter just came up — kick Zigbee to re-DECLARE so the mirror can
    // create bridged endpoints for every entity we already know about.
    // Debounce: if thread-c6 is crash-looping, don't QUERY zigbee on every
    // reboot — that can cascade and crash zigbee too.
    static std::uint32_t s_last_kick_ms = 0;
    const std::uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now_ms - s_last_kick_ms >= 10000u || s_last_kick_ms == 0) {
      s_last_kick_ms = now_ms;
      hubproto::Header q{};
      q.version = hubproto::kProtocolVersion;
      q.type = hubproto::MsgType::Query;
      q.chip_id = kZigbeeChipId;
      bus->publish(q, nullptr, 0);
      ESP_LOGI(kTag, "kick Zigbee QUERY because Matter came up");
    } else {
      ESP_LOGD(kTag, "skip Zigbee QUERY (debounce, last kick %u ms ago)",
               static_cast<unsigned>(now_ms - s_last_kick_ms));
    }
  } else if (h.type == hubproto::MsgType::Command && h.chip_id == kThreadChipId) {
    // A Matter controller (Apple Home...) asked for this: the hub's intent.
    hubproto::ValueType vt{};
    hubproto::Command cmd{};
    if (len >= 3 && g_reg != nullptr && g_reg->value_type_of(h.entity_id, p[2], vt) &&
        hubproto::decode_command(p, len, vt, cmd)) {
      CmdSrcScope src(core::CmdSource::Matter);
      rec_note_command(nullptr, cmd.entity_id, cmd.attr_id, cmd.value);
    }
    hubproto::Header fwd = h;
    fwd.chip_id = kZigbeeChipId;
    bus->publish(fwd, p, len);
    ESP_LOGI(kTag, "forward Matter COMMAND entity=0x%04x -> Zigbee seq=%u",
             static_cast<unsigned>(h.entity_id), static_cast<unsigned>(h.seq));
  } else if (h.type == hubproto::MsgType::MatterResult && h.chip_id == kThreadChipId) {
    // How far the commissioning got. Queue it — the main loop owns the cache and the clients.
    hubproto::MatterResult res{};
    if (!hubproto::decode_matter_result(p, len, res)) {
      ESP_LOGW(kTag, "matter: the chip sent a result of %u bytes that makes no sense",
               static_cast<unsigned>(len));
    } else if (g_matter_q != nullptr) {
      const MatterUpdate u{res.state, res.code, res.entity_id};
      if (xQueueSend(g_matter_q, &u, 0) != pdTRUE) {
        ESP_LOGW(kTag, "matter: the result queue is full, dropping a result");
      }
    }
  }
#ifdef KEYSTONE_HAVE_MATTER
  else if (h.type == hubproto::MsgType::ThreadDataset && h.chip_id == kThreadChipId) {
    // The co-processor's Thread network, for the Matter commissioner: the brain has no
    // 802.15.4 radio, so this is where the dataset the device needs comes from.
    hubproto::ThreadDatasetView view{};
    if (!hubproto::decode_thread_dataset(p, len, view)) {
      ESP_LOGW(kTag, "matter: the chip sent a Thread dataset of %u bytes that makes no sense",
               static_cast<unsigned>(len));
    } else if (!g_thread_dataset.set(view.bytes, view.len)) {
      ESP_LOGW(kTag, "matter: the Thread dataset of the chip is not one we can use (%u bytes)",
               static_cast<unsigned>(view.len));
    } else {
      ESP_LOGI(kTag, "matter: got the Thread dataset (%u bytes, channel %u, name '%s', ext pan id %s)",
               static_cast<unsigned>(g_thread_dataset.size()),
               static_cast<unsigned>(g_thread_dataset.channel()),
               g_thread_dataset.network_name(),
               g_thread_dataset.ext_pan_id());
      if (g_thread_dataset_sem != nullptr) xSemaphoreGive(g_thread_dataset_sem);
    }
  }
#endif
}

// ---- Console <-> main-loop request channel --------------------------------
enum ReqKind { REQ_HOME, REQ_GET, REQ_SETONOFF, REQ_TIMER, REQ_TIMERS, REQ_CANCEL,
               REQ_AT, REQ_SCHEDULES, REQ_UNSCHEDULE,
               REQ_CHIPS, REQ_LOG,
               REQ_PAIR, REQ_UNPAIR, REQ_IDENTIFY,
               REQ_WIFI_JOIN, REQ_WIFI_STATUS, REQ_WIFI_SCAN,
               REQ_TIME, REQ_TZ,
               REQ_NAME_SET, REQ_NAME_CLEAR, REQ_NAMES_LIST,
               REQ_RULE, REQ_DESIRED, REQ_SCENE, REQ_INJECT, REQ_DEV_OTA };

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
  const char* url;      // for REQ_DEV_OTA
  const char* sha;      // for REQ_DEV_OTA
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

// The shared SPEC §6.1 table (core/app_protocol.hpp): the console, the app and the rules all use
// the same names, so a new device type needs no change here.
const char* attr_name(std::uint8_t id) { return core::app::attr_name(id); }

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

// The names blob: a header plus one entry per named entity (id + a 32-byte name).
// Sized from the table itself so a change of MaxNameLen cannot overflow it.
constexpr std::size_t kNamesBlobMax =
    core::EntityRegistry<>::Names::kHeaderSize +
    core::EntityRegistry<>::Names::kEntryWireSize * core::EntityRegistry<>::Names::kMaxEntities;

void persist_names(const core::EntityRegistry<>::Names& names) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  // static: this runs in the request-drain path of the main task, whose stack
  // (CONFIG_ESP_MAIN_TASK_STACK_SIZE) is far too small for a 2 KB local.
  // PSRAM: frees internal-DRAM BSS space before heap_init pool.
  static EXT_RAM_BSS_ATTR std::uint8_t buf[kNamesBlobMax];
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
  if (nvs_get_blob(h, "ent_names", nullptr, &sz) == ESP_OK && sz <= kNamesBlobMax) {
    static EXT_RAM_BSS_ATTR std::uint8_t buf[kNamesBlobMax];
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

using Scenes = core::SceneTable<8, 8>;
constexpr std::size_t kSceneBlobMax =
    Scenes::kHeaderSize + 8 * (Scenes::kNameWire + 1 + 8 * Scenes::kActionWire);

// ---- location + sunrise/sunset ------------------------------------------------------------
// The hub's position (for `sunrise`/`sunset` times in schedules and rule windows), in micro-degrees.
// Set from the console (`loc`) or the app (`location.set`), kept in NVS. Read from several tasks, so
// plain atomics; `g_hub_dirty` asks the main loop to tell the connected apps.
std::atomic<std::int32_t> g_loc_lat_e6{0};
std::atomic<std::int32_t> g_loc_lon_e6{0};
std::atomic<bool> g_loc_set{false};

void persist_location() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  if (g_loc_set) {
    const std::int32_t v[2] = {g_loc_lat_e6, g_loc_lon_e6};
    nvs_set_blob(h, "location", v, sizeof(v));
  } else {
    nvs_erase_key(h, "location");
  }
  nvs_commit(h);
  nvs_close(h);
}

void restore_location() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::int32_t v[2] = {};
  std::size_t sz = sizeof(v);
  if (nvs_get_blob(h, "location", v, &sz) == ESP_OK && sz == sizeof(v) && v[0] >= -90000000 && v[0] <= 90000000 &&
      v[1] >= -180000000 && v[1] <= 180000000) {
    g_loc_lat_e6 = v[0];
    g_loc_lon_e6 = v[1];
    g_loc_set = true;
    ESP_LOGI(kTag, "location restored: %.4f, %.4f", v[0] / 1e6, v[1] / 1e6);
  }
  nvs_close(h);
}

void set_location(double lat, double lon) {
  g_loc_lat_e6 = static_cast<std::int32_t>(std::lround(lat * 1e6));
  g_loc_lon_e6 = static_cast<std::int32_t>(std::lround(lon * 1e6));
  g_loc_set = true;
  persist_location();
  g_hub_dirty = true;
}

void clear_location() {
  g_loc_set = false;
  persist_location();
  g_hub_dirty = true;
}

// The wall clock's local date and its UTC offset (DST included), without relying on tm_gmtoff.
bool local_date(int& y, int& m, int& d, int& utc_offset_min) {
  if (!s_time_synced) return false;
  const time_t now = time(nullptr);
  struct tm lt{}, gt{};
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);
  const long l = static_cast<long>(core::civil_day_number(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday)) * 1440 +
                 lt.tm_hour * 60 + lt.tm_min;
  const long g = static_cast<long>(core::civil_day_number(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday)) * 1440 +
                 gt.tm_hour * 60 + gt.tm_min;
  y = lt.tm_year + 1900;
  m = lt.tm_mon + 1;
  d = lt.tm_mday;
  utc_offset_min = static_cast<int>(l - g);
  return true;
}

// Today's sunrise and sunset in local minutes. False while the location is unset, the clock is not
// synced, or the sun does not rise/set today. Cached per (day, offset, location): the schedule poll
// and the rules call it often, the answer changes once a day.
bool sun_today(core::SunTimes& out) {
  static SemaphoreHandle_t mu = xSemaphoreCreateMutex();
  static int c_day = -1, c_off = 0;
  static std::int32_t c_lat = 0, c_lon = 0;
  static core::SunTimes c_sun;
  if (!g_loc_set) return false;
  int y = 0, m = 0, d = 0, off = 0;
  if (!local_date(y, m, d, off)) return false;
  const int day = core::civil_day_number(y, m, d);
  const std::int32_t lat = g_loc_lat_e6, lon = g_loc_lon_e6;
  xSemaphoreTake(mu, portMAX_DELAY);
  if (c_day != day || c_off != off || c_lat != lat || c_lon != lon) {
    c_sun = core::sun_times_local(y, m, d, lat / 1e6, lon / 1e6, off);
    c_day = day;
    c_off = off;
    c_lat = lat;
    c_lon = lon;
  }
  out = c_sun;
  xSemaphoreGive(mu);
  return out.valid;
}

bool sun_cb(void* /*ctx*/, core::SunTimes& out) { return sun_today(out); }

// ---- changing the Wi-Fi network (main loop only) -----------------------------------------------
// A request waits a moment (so the app's reply goes out first), then the new network is applied as a
// trial: it is saved only if the hub gets an address on it within 30 s, otherwise the old network is
// restored. With no working network to go back to, the new credentials are kept at once.
constexpr std::uint32_t kWifiTrialMs = 30000;
struct WifiRequest {
  bool pending = false;
  WifiCreds creds;
  std::uint32_t apply_at = 0;
};
WifiRequest g_wifi_req;
core::WifiTrial g_wifi_trial;
WifiCreds g_wifi_new, g_wifi_prev;
unsigned g_wifi_ip_count_at_apply = 0;

void wifi_request(const char* ssid, const char* pass, std::uint32_t now) {
  g_wifi_req.creds = WifiCreds{};
  std::strncpy(g_wifi_req.creds.ssid, ssid, sizeof(g_wifi_req.creds.ssid) - 1);
  std::strncpy(g_wifi_req.creds.pass, pass, sizeof(g_wifi_req.creds.pass) - 1);
  g_wifi_req.apply_at = now + 700;
  g_wifi_req.pending = true;
}

void wifi_service(std::uint32_t now) {
  if (g_wifi_req.pending && static_cast<std::int32_t>(now - g_wifi_req.apply_at) >= 0) {
    g_wifi_req.pending = false;
    const bool can_revert = s_wifi_up && s_wifi_have_creds;
    g_wifi_prev = s_wifi_active;
    g_wifi_new = g_wifi_req.creds;
    g_wifi_ip_count_at_apply = s_wifi_got_ip;
    ESP_LOGI(kTag, "wifi: trying %s%s", g_wifi_new.ssid, can_revert ? "" : " (no working network to go back to)");
    wifi_apply(g_wifi_new);
    g_wifi_trial.begin(now, kWifiTrialMs, can_revert);
  }
  if (g_wifi_trial.active() && s_wifi_got_ip != g_wifi_ip_count_at_apply) g_wifi_trial.on_connected();
  switch (g_wifi_trial.poll(now)) {
    case core::WifiTrial::Decision::Commit:
      wifi_creds_save(g_wifi_new);
      ESP_LOGI(kTag, "wifi: %s kept", g_wifi_new.ssid);
      g_hub_dirty = true;
      break;
    case core::WifiTrial::Decision::Revert:
      ESP_LOGW(kTag, "wifi: could not join %s in %u s, going back to %s", g_wifi_new.ssid,
               static_cast<unsigned>(kWifiTrialMs / 1000), g_wifi_prev.ssid);
      wifi_apply(g_wifi_prev);
      break;
    case core::WifiTrial::Decision::None:
      break;
  }
}

// The scene store is edited and read only by the main loop (console, app requests, schedules, timers).
// Rules fire in the UART task, so they only queue the scene id (g_scene_q) and the main loop applies it.
Scenes* g_scenes = nullptr;
QueueHandle_t g_scene_q = nullptr;

void rule_fire_hook(void* /*ctx*/, std::size_t slot, const core::Rule& r) {
  if (r.action_entity != core::kSceneTarget) t_cmd_src = core::CmdSource::Rule;  // the engine sends right after
  ev_attr(core::EventKind::Rule, r.action_entity, r.action_attr, &r.action_value, 0, static_cast<std::uint16_t>(slot));
}

bool rule_action_hook(void* /*ctx*/, std::uint16_t entity, std::uint8_t attr, const hubproto::Value& /*v*/,
                      std::uint32_t /*now*/) {
  if (entity != core::kSceneTarget) return false;
  if (g_scene_q == nullptr || xQueueSend(g_scene_q, &attr, 0) != pdTRUE) {
    ESP_LOGW(kTag, "rule: scene queue full, scene %u not applied", static_cast<unsigned>(attr));
  }
  return true;  // handled either way: it is not a command for an entity
}

// The name of the scene an action points at, or nullptr (not a scene action / scene gone).
const char* scene_name_for(std::uint16_t entity, std::uint8_t attr) {
  if (entity != core::kSceneTarget || g_scenes == nullptr) return nullptr;
  const auto* sc = g_scenes->find_by_id(attr);
  return sc != nullptr ? sc->name : nullptr;
}

void persist_scenes(const Scenes& scenes) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  std::uint8_t buf[kSceneBlobMax];
  const std::size_t written = scenes.save(buf, sizeof(buf));
  if (scenes.active_count() > 0 && written > 0) {
    nvs_set_blob(h, "scenes", buf, written);
  } else {
    nvs_erase_key(h, "scenes");
  }
  nvs_commit(h);
  nvs_close(h);
}

void restore_scenes(Scenes& scenes) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  std::uint8_t buf[kSceneBlobMax];
  std::size_t sz = sizeof(buf);
  if (nvs_get_blob(h, "scenes", buf, &sz) == ESP_OK) {
    if (scenes.load(buf, sz)) {
      ESP_LOGI(kTag, "restored %u scene(s) from NVS", static_cast<unsigned>(scenes.active_count()));
    } else {
      ESP_LOGW(kTag, "stored scenes are corrupt — ignored");
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
  for (unsigned id = 0; id < 256; ++id) {
    const char* n = core::app::attr_name(static_cast<std::uint8_t>(id));
    if (n != nullptr && std::strcmp(s, n) == 0) {
      out = static_cast<std::uint8_t>(id);
      return true;
    }
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
  // The name can be as long as the registry allows (32 bytes now) - the buffer has to
  // keep up, or the compiler (rightly) refuses the truncation.
  char eb[Registry::kMaxNameLen + 1], ab[8];
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
    "  rule add <entity> <attr> <op> <value> then scene <name>     (apply a scene)\n"
    "  rule del <n>\n"
    "  optional, after the action (any order):\n"
    "    between HH:MM-HH:MM      local time window (may wrap midnight)\n"
    "    between <from> <to>      each HH:MM or sunrise/sunset[+-min], e.g. between sunset 23:00\n"
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
      if (ru->action_entity == core::kSceneTarget) {
        const char* sn = scene_name_for(ru->action_entity, ru->action_attr);
        std::snprintf(a, sizeof(a), "scene %s", sn != nullptr ? sn : "?");
        av[0] = '\0';
      } else {
        fmt_target(a, sizeof(a), reg, ru->action_entity, ru->action_attr);
        fmt_value(av, sizeof(av), ru->action_value.type, ru->action_value);
      }
      if (ru->match != nullptr) {
        std::snprintf(ov, sizeof(ov), "<builtin>");
      } else if (ru->op == core::CmpOp::Always) {
        ov[0] = '\0';
      } else {
        fmt_value(ov, sizeof(ov), ru->operand.type, ru->operand);
      }
      off += static_cast<std::size_t>(std::snprintf(
          out + off, cap - off, ru->action_entity == core::kSceneTarget ? "  #%u  %s %s %s  ->  %s%s" : "  #%u  %s %s %s  ->  %s = %s",
          static_cast<unsigned>(i), t, op_str(ru->op), ov, a, av));
      if (ru->window_start != core::kNoWindow && off < cap) {
        char w1[24], w2[24];
        core::format_time_point(ru->window_start, w1, sizeof(w1));
        core::format_time_point(ru->window_end, w2, sizeof(w2));
        off += static_cast<std::size_t>(std::snprintf(out + off, cap - off, "  between %s %s", w1, w2));
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

  if (std::strcmp(argv[1], "add") == 0 && argc >= 8) {
    const bool any = argc > 4 && std::strcmp(argv[4], "any") == 0;
    const int base = any ? 5 : 6;  // index of "then"
    const bool to_scene = argc > base + 1 && std::strcmp(argv[base], "then") == 0 && std::strcmp(argv[base + 1], "scene") == 0;
    if (argc < base + (to_scene ? 3 : 4) || std::strcmp(argv[base], "then") != 0) {
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
    int tail_from = base + 4;  // first word after the action
    if (to_scene) {
      const auto* sc = g_scenes != nullptr ? g_scenes->find(argv[base + 2]) : nullptr;
      if (sc == nullptr) {
        std::snprintf(out, cap, "%s: no such scene\n", argv[base + 2]);
        return;
      }
      ru.action_entity = core::kSceneTarget;
      ru.action_attr = sc->id;
      ru.action_value = hubproto::Value::make_bool(true);
      tail_from = base + 3;
    } else {
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
    }
    // Optional constraints after the action, in any order.
    for (int k = tail_from; k < argc;) {
      if (std::strcmp(argv[k], "between") == 0 && k + 1 < argc) {
        if (parse_window(argv[k + 1], ru.window_start, ru.window_end)) {  // between 18:00-23:00
          k += 2;
        } else if (k + 2 < argc && core::parse_time_point(argv[k + 1], ru.window_start) &&
                   core::parse_time_point(argv[k + 2], ru.window_end) && ru.window_start != ru.window_end) {
          k += 3;  // between sunset+30 23:00
        } else {
          std::snprintf(out, cap, "%s: bad window (HH:MM-HH:MM, or two times: HH:MM or sunrise/sunset[+-min], different)\n", argv[k + 1]);
          return;
        }
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
// Apply a scene by id: every action as an ordinary command. Main loop only.
void run_scene(Dispatcher& disp, Registry& reg, std::uint8_t scene_id, std::uint32_t now, const char* kind) {
  const auto* sc = g_scenes != nullptr ? g_scenes->find_by_id(scene_id) : nullptr;
  if (sc == nullptr) {
    ESP_LOGW(kTag, "%s: scene %u no longer exists", kind, static_cast<unsigned>(scene_id));
    return;
  }
  CmdSrcScope src(core::CmdSource::Scene);
  const auto res = core::app::apply_scene(reg, *g_scenes, sc->name,
                                          [&](std::uint16_t e, std::uint8_t a, const hubproto::Value& v) {
                                            std::uint16_t seq = 0;
                                            return disp.send(e, a, v, now, seq);
                                          });
  ev_plain(core::EventKind::Scene, static_cast<std::uint8_t>(scene_via(kind)), scene_id);
  ESP_LOGI(kTag, "%s applied scene %s: %u sent, %u skipped", kind, sc->name,
           static_cast<unsigned>(res.sent), static_cast<unsigned>(res.skipped));
}

void run_action(Dispatcher& disp, Registry& reg, std::uint32_t now, const char* kind,
                std::size_t slot, std::uint16_t entity_id, std::uint8_t attr_id,
                const hubproto::Value& val) {
  const core::EventKind trigger = std::strcmp(kind, "timer") == 0 ? core::EventKind::Timer : core::EventKind::Schedule;
  if (entity_id == core::kSceneTarget) {
    ev_attr(trigger, entity_id, attr_id, nullptr, 0, static_cast<std::uint16_t>(slot));
    run_scene(disp, reg, attr_id, now, kind);
    return;
  }
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
  ev_attr(trigger, entity_id, attr_id, &v, 0, static_cast<std::uint16_t>(slot));
  CmdSrcScope src(cmd_src_of(kind));
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

// "daily" / "mon,wed" for a recurring schedule, "on 2026-10-20" for a one-off.
const char* schedule_when(const Schedules::Schedule& s, char* buf, std::size_t cap) {
  if (s.once_day == 0) return days_text(s.weekdays, buf, cap);
  char d[16];
  core::format_date(s.once_day, d, sizeof(d));
  std::snprintf(buf, cap, "on %s", d);
  return buf;
}

// The hub's local date as a civil_day_number; 0 until the clock is synced.
std::int32_t local_today() {
  if (!s_time_synced) return 0;
  const time_t wall = time(nullptr);
  struct tm lt{};
  localtime_r(&wall, &lt);
  return core::civil_day_number(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

// at [<YYYY-MM-DD>] <HH:MM> <on|off|toggle> <id_or_name> [daily|weekdays|weekends|mon,tue,...]
int cmd_at(int argc, char** argv) {
  static const char* kUsage =
      "usage: at <HH:MM|sunrise|sunset[+-min]> <on|off|toggle> <id_or_name> [daily|weekdays|weekends|mon,tue,..]\n"
      "       at <time> scene <name> [days]      (e.g. at sunset+30 scene evening)\n"
      "       at <YYYY-MM-DD> <time> <on|off|toggle> <id_or_name>   once, on that date (then it deletes itself)\n";
  std::int32_t once = 0;
  if (argc >= 2 && std::strchr(argv[1], '-') != nullptr && argv[1][0] >= '0' && argv[1][0] <= '9' &&
      std::strlen(argv[1]) == 10) {
    if (!core::parse_date(argv[1], once)) { printf("bad date '%s' (use YYYY-MM-DD, 2000..2099)\n", argv[1]); return 1; }
    const std::int32_t today = local_today();
    if (today != 0 && once < today) { printf("%s is in the past\n", argv[1]); return 1; }
    --argc;
    ++argv;
    if (argc == 5) { printf("a date and days exclude each other\n%s", kUsage); return 1; }
  }
  if (argc < 4 || argc > 5) { printf("%s", kUsage); return 1; }
  std::uint16_t point = 0;
  if (!core::parse_time_point(argv[1], point)) {
    printf("bad time '%s' (use HH:MM in local time — see `tz` — or sunrise/sunset with an optional +-minutes, e.g. sunset+30)\n", argv[1]);
    return 1;
  }
  const bool is_scene = std::strcmp(argv[2], "scene") == 0;
  const bool toggle = std::strcmp(argv[2], "toggle") == 0;
  if (!is_scene && !toggle && std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0) {
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
  r.idx = is_scene ? 2 : toggle ? 1 : 0;  // REQ_AT: 2 = apply the scene named by `target`
  r.delay_ms = point;  // a time point: a local minute of the day, or relative to the sun
  r.id = days;                                           // weekday mask
  r.argc = once;                                         // REQ_AT: the date of a one-off (civil_day_number), else 0
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

// Free internal heap right now, the lowest it has been since boot, and the largest
// contiguous block (what a single big allocation can still get). Safe to call from
// the console task: the heap API is thread-safe.
// events [<n>] - the newest events of the history ring (what `history` gives the app), newest first.
int cmd_events(int argc, char** argv) {
  std::size_t want = 20;
  if (argc >= 2) {
    char* end = nullptr;
    const long v = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || v < 1 || v > 256) {
      printf("usage: events [<1..256>]\n");
      return 1;
    }
    want = static_cast<std::size_t>(v);
  }
  static core::Event page[24];
  std::uint32_t before = 0, latest = 0, oldest = 0;
  const std::uint32_t now_s = static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
  std::size_t shown = 0;
  while (shown < want) {
    xSemaphoreTake(g_ev_mu, portMAX_DELAY);
    const std::size_t n = g_ev_ring.read(before, want - shown < 24 ? want - shown : 24, page);
    latest = g_ev_ring.latest();
    oldest = g_ev_ring.oldest();
    xSemaphoreGive(g_ev_mu);
    if (n == 0) break;
    for (std::size_t i = 0; i < n; ++i) {
      const core::Event& e = page[i];
      char val[24] = "";
      if (e.has_value) fmt_value(val, sizeof(val), e.value.type, e.value);
      const char* what = core::app::event_what(e);
      const char* an = e.attr != 0 ? attr_name(e.attr) : nullptr;
      printf("#%-4u -%-6us %-14s", static_cast<unsigned>(e.seq), static_cast<unsigned>(now_s - e.t_s),
             core::app::event_kind_name(e.kind));
      if (what != nullptr) printf(" %s", what);
      if (e.entity != 0 && e.entity != core::kSceneTarget) printf(" 0x%04x", static_cast<unsigned>(e.entity));
      if (an != nullptr && e.entity != core::kSceneTarget) printf(" %s", an);
      if (e.has_value) printf(" = %s", val);
      if (e.kind == core::EventKind::Rule || e.kind == core::EventKind::Schedule || e.kind == core::EventKind::Timer ||
          e.kind == core::EventKind::Scene || e.kind == core::EventKind::Chip) {
        printf(" (#%u)", static_cast<unsigned>(e.ref));
      }
      printf("\n");
    }
    shown += n;
    before = page[n - 1].seq;
  }
  printf("%u event(s) shown; ring holds #%u..#%u\n", static_cast<unsigned>(shown), static_cast<unsigned>(oldest),
         static_cast<unsigned>(latest));
  return 0;
}

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
  if (argc < 4) { printf("usage: in <sec> <on|off|toggle> <id_or_name>\n       in <sec> scene <name>\n"); return 1; }
  const long sec = std::strtol(argv[1], nullptr, 10);
  const bool is_scene = std::strcmp(argv[2], "scene") == 0;
  const bool toggle = std::strcmp(argv[2], "toggle") == 0;
  if (!is_scene && !toggle && std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0) {
    printf("usage: in <sec> <on|off|toggle> <id_or_name>\n       in <sec> scene <name>\n");
    return 1;
  }
  const bool on = std::strcmp(argv[2], "on") == 0;
  char buf[128];
  Req r{};
  r.kind = REQ_TIMER;
  r.target = argv[3];
  r.val = on;
  r.idx = is_scene ? 2 : toggle ? 1 : 0;  // REQ_TIMER: 1 = toggle, 2 = apply the scene named by `target`
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
  static EXT_RAM_BSS_ATTR char buf[1536];  // only the console task calls this
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
  static EXT_RAM_BSS_ATTR char buf[1024];
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

// scene                                   list
// scene <name>                            apply
// scene add <name> <id> <attr> <value> …  create or replace (one or more triples)
// scene del <name>
int cmd_scene(int argc, char** argv) {
  static EXT_RAM_BSS_ATTR char buf[1024];
  Req r{};
  r.kind = REQ_SCENE;
  r.argc = argc;
  r.argv = argv;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

// ---- dev tool: pretend a device exists, towards the Matter chip only -------------------------
// `inject <kind>` declares a made-up device (and sends a first STATE) to the Matter chip as if the
// Zigbee chip had announced it, so the bridged endpoint of every device type can be exercised
// without owning the hardware. The brain's own registry is untouched: it never sees these.
//   inject <dimmer|ctlight|colorlight|sensor|motion|contact|thermostat|button>
//   inject set <entity> <attr> <value>      a STATE (value in hub units; xy not supported)
//   inject clear                            Undeclare every injected device
constexpr std::uint16_t kInjectBase = 0x0F01;
constexpr int kInjectKinds = 8;
bool g_injected[kInjectKinds] = {};

struct InjectSpec {
  const char* name;
  hubproto::DeviceType type;
  std::uint32_t caps;
};
const InjectSpec kInject[kInjectKinds] = {
    {"dimmer", hubproto::DeviceType::Light, core::zcl::kCapOnOff | core::zcl::kCapLevel},
    {"ctlight", hubproto::DeviceType::Light,
     core::zcl::kCapOnOff | core::zcl::kCapLevel | core::zcl::kCapColorTemp},
    {"colorlight", hubproto::DeviceType::Light,
     core::zcl::kCapOnOff | core::zcl::kCapLevel | core::zcl::kCapColorTemp | core::zcl::kCapColorXy},
    {"sensor", hubproto::DeviceType::Sensor,
     core::zcl::kCapTemperature | core::zcl::kCapHumidity | core::zcl::kCapIlluminance | core::zcl::kCapBattery},
    {"motion", hubproto::DeviceType::Occupancy,
     core::zcl::kCapOccupancy | core::zcl::kCapTemperature | core::zcl::kCapBattery},
    {"contact", hubproto::DeviceType::Contact, core::zcl::kCapContact | core::zcl::kCapBattery},
    {"thermostat", hubproto::DeviceType::Climate,
     core::zcl::kCapThermostat | core::zcl::kCapTemperature | core::zcl::kCapBattery},
    {"button", hubproto::DeviceType::Button, core::zcl::kCapButton | core::zcl::kCapBattery},
};

void inject_send(core::Bus& bus, hubproto::MsgType type, std::uint16_t entity,
                 const std::uint8_t* payload, std::size_t len) {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = kThreadChipId;
  h.entity_id = entity;
  bus.publish(h, payload, len);
}

void inject_state(core::Bus& bus, std::uint16_t entity, std::uint8_t attr, const hubproto::Value& v) {
  const hubproto::StateEntry e[1] = {{attr, v}};
  std::uint8_t buf[16];
  const auto r = hubproto::encode_state(entity, e, 1, buf, sizeof(buf));
  if (r.ok) inject_send(bus, hubproto::MsgType::State, entity, buf, r.len);
}

void handle_inject(core::Bus& bus, Req* r) {
  namespace A = hubproto::attr;
  if (r->argc < 2) {
    std::snprintf(r->out, r->out_cap,
                  "usage: inject <dimmer|ctlight|colorlight|sensor|motion|contact|thermostat|button>\n"
                  "       inject set <entity> <attr> <value> | inject clear\n");
    return;
  }
  const char* what = r->argv[1];
  if (std::strcmp(what, "clear") == 0) {
    int n = 0;
    for (int i = 0; i < kInjectKinds; ++i) {
      if (!g_injected[i]) continue;
      inject_send(bus, hubproto::MsgType::Undeclare, static_cast<std::uint16_t>(kInjectBase + i), nullptr, 0);
      g_injected[i] = false;
      ++n;
    }
    std::snprintf(r->out, r->out_cap, "removed %d injected device(s)\n", n);
    return;
  }
  if (std::strcmp(what, "set") == 0) {
    std::uint8_t attr = 0;
    hubproto::ValueType vt{};
    hubproto::Value v{};
    char* end = nullptr;
    const long ent = r->argc >= 3 ? std::strtol(r->argv[2], &end, 0) : -1;
    if (r->argc != 5 || ent < 0 || !parse_attr(r->argv[3], attr) ||
        !core::zcl::attr_value_type(attr, vt) || !parse_value(r->argv[4], vt, v)) {
      std::snprintf(r->out, r->out_cap, "usage: inject set <entity> <attr> <value>\n");
      return;
    }
    inject_state(bus, static_cast<std::uint16_t>(ent), attr, v);
    std::snprintf(r->out, r->out_cap, "sent STATE to the Matter chip\n");
    return;
  }
  for (int i = 0; i < kInjectKinds; ++i) {
    if (std::strcmp(what, kInject[i].name) != 0) continue;
    const std::uint16_t id = static_cast<std::uint16_t>(kInjectBase + i);
    hubproto::AttrDecl attrs[8];
    const std::size_t n = core::zcl::attrs_for(kInject[i].caps, attrs, 8);
    std::uint8_t buf[4 + 3 * 8];
    const auto d = hubproto::encode_declare(id, kInject[i].type, attrs, static_cast<std::uint8_t>(n), buf, sizeof(buf));
    if (!d.ok) break;
    inject_send(bus, hubproto::MsgType::Declare, id, buf, d.len);
    g_injected[i] = true;
    // A plausible first reading for whatever the device has.
    const std::uint32_t c = kInject[i].caps;
    if (c & core::zcl::kCapOnOff) inject_state(bus, id, A::kOnOff, hubproto::Value::make_bool(true));
    if (c & core::zcl::kCapLevel) inject_state(bus, id, A::kBrightness, hubproto::Value::make_u8(127));
    if (c & core::zcl::kCapColorTemp) inject_state(bus, id, A::kColorTemp, hubproto::Value::make_u16(300));
    if (c & core::zcl::kCapColorXy) inject_state(bus, id, A::kColorXy, hubproto::Value::make_xy(0x4000, 0x3000));
    if (c & core::zcl::kCapTemperature) inject_state(bus, id, A::kTemperature, hubproto::Value::make_i16(215));
    if (c & core::zcl::kCapHumidity) inject_state(bus, id, A::kHumidity, hubproto::Value::make_u16(551));
    if (c & core::zcl::kCapIlluminance) inject_state(bus, id, A::kIlluminance, hubproto::Value::make_u16(320));
    if (c & core::zcl::kCapBattery) inject_state(bus, id, A::kBattery, hubproto::Value::make_u8(87));
    if (c & core::zcl::kCapOccupancy) inject_state(bus, id, A::kOccupied, hubproto::Value::make_bool(false));
    if (c & core::zcl::kCapContact) inject_state(bus, id, A::kContact, hubproto::Value::make_bool(true));
    if (c & core::zcl::kCapThermostat) {
      inject_state(bus, id, A::kSetpoint, hubproto::Value::make_i16(210));
      inject_state(bus, id, A::kMode, hubproto::Value::make_enum8(4));
    }
    std::snprintf(r->out, r->out_cap, "injected %s as entity 0x%04x (Matter chip only)\n", kInject[i].name,
                  static_cast<unsigned>(id));
    return;
  }
  std::snprintf(r->out, r->out_cap, "unknown kind '%s'\n", what);
}

int cmd_inject(int argc, char** argv) {
  static char buf[256];
  Req r{};
  r.kind = REQ_INJECT;
  r.argc = argc;
  r.argv = argv;
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

// loc                  show the location
// loc <lat> <lon>      degrees, north and east positive (e.g. loc 50.08 14.43)
// loc clear
int cmd_loc(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "clear") == 0) {
    clear_location();
    printf("location cleared (sunrise/sunset schedules will not fire)\n");
    return 0;
  }
  if (argc == 3) {
    char *e1 = nullptr, *e2 = nullptr;
    const double lat = std::strtod(argv[1], &e1);
    const double lon = std::strtod(argv[2], &e2);
    if (*argv[1] == '\0' || *e1 != '\0' || *argv[2] == '\0' || *e2 != '\0' || !(lat >= -90.0 && lat <= 90.0) ||
        !(lon >= -180.0 && lon <= 180.0)) {
      printf("usage: loc <lat -90..90> <lon -180..180>   (degrees; north and east positive)\n");
      return 1;
    }
    set_location(lat, lon);
    printf("location set: %.4f, %.4f\n", lat, lon);
    return 0;
  }
  if (argc != 1) {
    printf("usage: loc [<lat> <lon> | clear]\n");
    return 1;
  }
  if (g_loc_set) printf("location: %.4f, %.4f\n", g_loc_lat_e6 / 1e6, g_loc_lon_e6 / 1e6);
  else printf("no location set (loc <lat> <lon>)\n");
  return 0;
}

// sun: today's sunrise and sunset in local time.
int cmd_sun(int /*argc*/, char** /*argv*/) {
  if (!g_loc_set) { printf("no location set (loc <lat> <lon>)\n"); return 1; }
  if (!s_time_synced) { printf("clock not synced yet\n"); return 1; }
  core::SunTimes sun;
  if (!sun_today(sun)) { printf("the sun does not rise and set at this location today (polar day/night)\n"); return 0; }
  printf("sunrise %02d:%02d, sunset %02d:%02d (local time, see `tz`)\n", sun.rise / 60, sun.rise % 60, sun.set / 60,
         sun.set % 60);
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
  if (argc < 2 || argc > 3) {
    printf("usage: wifi_join <ssid> [<password>]   (no password = an open network)\n");
    return 1;
  }
  const char* why = nullptr;
  if (!core::wifi_credentials_valid(argv[1], argc == 3 ? argv[2] : "", &why)) {
    printf("%s\n", why != nullptr ? why : "bad credentials");
    return 1;
  }
  char buf[192];
  Req r{};
  r.kind = REQ_WIFI_JOIN;
  r.ssid = argv[1];
  r.pass = argc == 3 ? argv[2] : "";
  r.out = buf;
  r.out_cap = sizeof(buf);
  printf(submit(r) ? "%s" : "timeout\n", buf);
  return 0;
}

// wifi_forget: drop the saved network. The hub disconnects; after a reboot it uses the image's built-in
// default if there is one, else it waits for `wifi_join`.
void wifi_forget() {
  wifi_creds_erase();
  s_wifi_have_creds = false;
  s_wifi_up = false;
  esp_wifi_disconnect();
}

int cmd_wifi_forget(int /*argc*/, char** /*argv*/) {
  wifi_forget();
  printf("saved Wi-Fi credentials erased; disconnected\n");
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

#ifdef KEYSTONE_HAVE_MATTER
// IPv6 diagnostics for the Matter path.
//
// The commissioner lives on the brain, but the Thread network lives on the C6
// co-processor, which is a border router. A packet to a Matter device goes:
// brain -> Wi-Fi -> C6 -> 802.15.4. The brain learns the route to the mesh
// from the C6's router advertisement (a RIO), which the Matter stack's route
// hook keeps. Three things decide whether the operational session (CASE) can
// reach a device, and this command shows all of them: the addresses the hub
// owns (the source address of that traffic), the route lwIP would take, and
// whether a datagram actually leaves.
//
// From the Matter stack's route hook (platform/ESP32/route_hook/ESP32RouteTable.h):
// the RIOs from the Thread border router, and how lwIP asks it for the route
// and the next hop.
extern "C" struct netif* lwip_hook_ip6_route(const ip6_addr_t* src, const ip6_addr_t* dest);
extern "C" const ip6_addr_t* lwip_hook_nd6_get_gw(struct netif* netif, const ip6_addr_t* dest);

static void ip6_print_addrs() {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == nullptr) {
    printf("no Wi-Fi netif\n");
    return;
  }
  esp_ip6_addr_t addrs[6]{};
  const int n = esp_netif_get_all_ip6(netif, addrs);
  printf("Wi-Fi IPv6: %d address(es)\n", n);
  for (int i = 0; i < n && i < 6; i++) {
    ip6_addr_t a{};
    memcpy(a.addr, addrs[i].addr, 16);
    printf("  %s%s\n", ip6addr_ntoa(&a), ip6_addr_islinklocal(&a) ? " (link-local)" : "");
  }
  esp_ip6_addr_t global{};
  if (esp_netif_get_ip6_global(netif, &global) == ESP_OK) {
    ip6_addr_t a{};
    memcpy(a.addr, global.addr, 16);
    printf("  global: %s\n", ip6addr_ntoa(&a));
  }
}

static void ip6_print_route(const char* text) {
  ip6_addr_t dest{};
  if (!ip6addr_aton(text, &dest)) {
    printf("not an IPv6 address: %s\n", text);
    return;
  }
  struct netif* nif = nullptr;
  ip6_addr_t gw{};
  bool have_gw = false;
  // The route table is touched by the router-advertisement callback, which runs
  // in the TCP/IP thread; the core lock keeps the two apart.
  LOCK_TCPIP_CORE();
  nif = lwip_hook_ip6_route(nullptr, &dest);
  if (nif != nullptr) {
    const ip6_addr_t* g = lwip_hook_nd6_get_gw(nif, &dest);
    if (g != nullptr) {
      gw = *g;
      have_gw = true;
    }
  }
  UNLOCK_TCPIP_CORE();
  // ip6addr_ntoa() hands out one static buffer, so each address needs its own copy.
  char dest_text[INET6_ADDRSTRLEN], gw_text[INET6_ADDRSTRLEN];
  strncpy(dest_text, ip6addr_ntoa(&dest), sizeof(dest_text) - 1);
  dest_text[sizeof(dest_text) - 1] = '\0';
  if (nif == nullptr) {
    printf("route to %s: none\n", dest_text);
    return;
  }
  printf("route to %s: netif %c%c", dest_text, nif->name[0], nif->name[1]);
  if (have_gw) {
    strncpy(gw_text, ip6addr_ntoa(&gw), sizeof(gw_text) - 1);
    gw_text[sizeof(gw_text) - 1] = '\0';
    printf(" via %s", gw_text);
  }
  printf("\n");
}

// Send one datagram at the address and report what lwIP made of it. The reply
// is not expected (nothing answers there): what matters is whether the packet
// leaves the hub at all — the C6 counts what enters the mesh.
static int ip6_probe(const char* text, int port) {
  ip6_addr_t dest{};
  if (!ip6addr_aton(text, &dest)) {
    printf("not an IPv6 address: %s\n", text);
    return 1;
  }
  const int fd = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) {
    printf("socket failed: errno=%d\n", errno);
    return 1;
  }
  struct sockaddr_in6 sa{};
  sa.sin6_family = AF_INET6;
  sa.sin6_port = htons(static_cast<std::uint16_t>(port));
  // The raw bytes of an IPv6 address are the same in lwIP and in the socket API.
  memcpy(&sa.sin6_addr, dest.addr, 16);
  const char payload[] = "keystone";
  const int sent = lwip_sendto(fd, payload, sizeof(payload) - 1, 0,
                               reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));
  if (sent < 0) {
    printf("sendto %s failed: errno=%d (-4 no route, -6 no source address)\n", text, errno);
  } else {
    // lwIP binds the socket on the first send, so this is the source address
    // it chose for the traffic — the one the device would have to answer.
    struct sockaddr_in6 local{};
    socklen_t local_len = sizeof(local);
    if (lwip_getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &local_len) == 0) {
      ip6_addr_t from{};
      memcpy(from.addr, &local.sin6_addr, 16);
      printf("sent %d bytes to [%s]:%d from %s\n", sent, text, port, ip6addr_ntoa(&from));
    } else {
      printf("sent %d bytes to [%s]:%d\n", sent, text, port);
    }
  }
  lwip_close(fd);
  return sent < 0 ? 1 : 0;
}

int cmd_ip6(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
    if (argc < 3) {
      printf("usage: ip6 probe <address> [port]\n");
      return 1;
    }
    const int port = argc >= 4 ? atoi(argv[3]) : 5540;
    return ip6_probe(argv[2], port);
  }
  ip6_print_addrs();
  if (argc >= 2) {
    ip6_print_route(argv[1]);
  } else {
    printf("usage: ip6 [<address> | probe <address> [port]]\n");
  }
  return 0;
}
#endif  // KEYSTONE_HAVE_MATTER

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

// The firmware of a Zigbee *device*: the brain pushes the vendor .ota file to
// the coordinator, which serves it over the OTA Upgrade cluster (docs/ota.md).
fw::DeviceOtaJob* g_dev_ota = nullptr;

// The device session (waiting / transferring / done) is reported by the chip in
// the UART RX task; the main loop drains this queue and tells the app.
struct DevOtaEvent {
  std::uint16_t entity;
  hubproto::DevOtaPhase phase;
  hubproto::DevOtaStatus status;
  std::uint8_t percent;
};
QueueHandle_t g_dev_ota_q = nullptr;

void on_dev_ota_phase(void* /*ctx*/, std::uint16_t entity, hubproto::DevOtaPhase phase,
                      hubproto::DevOtaStatus status, std::uint8_t percent) {
  if (g_dev_ota_q == nullptr) return;
  const DevOtaEvent e{entity, phase, status, percent};
  xQueueSend(g_dev_ota_q, &e, 0);
}

const char* dev_ota_phase_name(hubproto::DevOtaPhase p) {
  switch (p) {
    case hubproto::DevOtaPhase::Receiving:    return "receiving";
    case hubproto::DevOtaPhase::Verifying:    return "verifying";
    case hubproto::DevOtaPhase::Notifying:    return "notifying";
    case hubproto::DevOtaPhase::Transferring: return "transferring";
    case hubproto::DevOtaPhase::Done:         return "done";
    case hubproto::DevOtaPhase::Failed:       return "failed";
    default:                                  return "idle";
  }
}

const char* dev_ota_status_name(hubproto::DevOtaStatus s) {
  switch (s) {
    case hubproto::DevOtaStatus::Ok:          return "ok";
    case hubproto::DevOtaStatus::Busy:        return "busy";
    case hubproto::DevOtaStatus::TooLarge:    return "too_large";
    case hubproto::DevOtaStatus::BadChecksum: return "bad_checksum";
    case hubproto::DevOtaStatus::BadFormat:   return "bad_format";
    case hubproto::DevOtaStatus::Unsupported: return "unsupported";
    case hubproto::DevOtaStatus::Timeout:     return "timeout";
    case hubproto::DevOtaStatus::Mismatch:    return "mismatch";
    case hubproto::DevOtaStatus::Aborted:     return "aborted";
    default:                                  return "ok";
  }
}

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

// Where the update stands, for the app protocol (`ota.status`). The job knows the transfer, the
// verifier knows whether the chip really came up on the new image.
void ota_app_status(core::app::OtaState& state, std::int64_t& pct, std::int64_t& chip, char* msg,
                     std::size_t msg_cap) {
  state = core::app::OtaState::Idle;
  pct = 0;
  chip = 0;
  msg[0] = '\0';
  if (g_ota == nullptr) {
    std::snprintf(msg, msg_cap, "the hub has no update job");
    return;
  }
  const fw::OtaJob::Status s = g_ota->status();
  chip = static_cast<std::int64_t>(s.chip);
  pct = static_cast<std::int64_t>(s.pct);
  std::snprintf(msg, msg_cap, "%s", s.message);

  switch (s.state) {
    case fw::OtaJob::State::Idle: state = core::app::OtaState::Idle; break;
    case fw::OtaJob::State::Downloading: state = core::app::OtaState::Downloading; break;
    case fw::OtaJob::State::Verifying: state = core::app::OtaState::Verifying; break;
    case fw::OtaJob::State::Done: state = core::app::OtaState::Done; break;
    case fw::OtaJob::State::Failed: state = core::app::OtaState::Failed; break;
  }

  // While the image is on its way the old verdict says nothing about it.
  if (g_ota->running() || g_verifier == nullptr) return;

  switch (g_verifier->verdict()) {
    case core::OtaVerifier::Verdict::Idle:
      break;
    case core::OtaVerifier::Verdict::Waiting:
      state = core::app::OtaState::Verifying;
      std::snprintf(msg, msg_cap, "the chip is rebooting into the new image");
      break;
    case core::OtaVerifier::Verdict::Verified:
      state = core::app::OtaState::Done;
      std::snprintf(msg, msg_cap, "verified");
      break;
    case core::OtaVerifier::Verdict::RolledBack:
      state = core::app::OtaState::Failed;
      std::snprintf(msg, msg_cap, "rolled back: the chip is not running the new image");
      break;
    case core::OtaVerifier::Verdict::NoRecord:
      state = core::app::OtaState::Done;
      std::snprintf(msg, msg_cap, "updated, but the chip has no OTA record to verify it");
      break;
    case core::OtaVerifier::Verdict::RunningUnconfirmed:
      state = core::app::OtaState::Done;
      std::snprintf(msg, msg_cap, "updated; the new image runs but is not confirmed yet");
      break;
    case core::OtaVerifier::Verdict::NoAnswer:
      state = core::app::OtaState::Failed;
      std::snprintf(msg, msg_cap, "the chip did not answer the verification request");
      break;
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
  if (argc >= 2 && std::strcmp(argv[1], "dev") == 0) {
    // The firmware of a Zigbee device (the coordinator serves it to the device).
    if (g_dev_ota == nullptr) { printf("ota dev not ready\n"); return 1; }
    if (argc == 3 && std::strcmp(argv[2], "status") == 0) {
      char dbuf[192];
      g_dev_ota->describe(dbuf, sizeof(dbuf));
      printf("%s", dbuf);
      return 0;
    }
    if (argc == 3 && std::strcmp(argv[2], "abort") == 0) {
      g_dev_ota->abort();
      printf("abort requested\n");
      return 0;
    }
    if (argc != 5) {
      printf("usage:\n"
             "  ota dev <id_or_name> <http(s)://url.ota> <sha256-hex>  - update a device's firmware\n"
             "  ota dev status                                          - progress / last result\n"
             "  ota dev abort                                           - stop the running update\n");
      return 1;
    }
    char dbuf[192];
    Req r{};
    r.kind = REQ_DEV_OTA;
    r.target = argv[2];
    r.url = argv[3];
    r.sha = argv[4];
    r.out = dbuf;
    r.out_cap = sizeof(dbuf);
    printf(submit(r) ? "%s" : "timeout\n", dbuf);
    return 0;
  }
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
int cmd_setup(int argc, char** argv);
int cmd_webplain(int argc, char** argv);
int cmd_relay(int argc, char** argv);

int cmd_led(int argc, char** argv) {
  if (argc == 1) {
    const auto s = status_led::current();
    const char* name = "?";
    switch (s) {
      case status_led::State::Booting:  name = "booting (blue)"; break;
      case status_led::State::Healthy:  name = "healthy (green)"; break;
      case status_led::State::Degraded: name = "degraded (amber)"; break;
      case status_led::State::Offline:  name = "offline (red)"; break;
      case status_led::State::Setup:    name = "setup (blue pulse)"; break;
      case status_led::State::Pairing:  name = "pairing (green pulse)"; break;
      case status_led::State::Off:      name = "off"; break;
    }
    printf("status LED: %s\n", name);
    return 0;
  }
  if (std::strcmp(argv[1], "off") == 0) {
    status_led::set(status_led::State::Off);
    printf("LED off (stays off until `led on` or reboot)\n");
    return 0;
  }
  if (std::strcmp(argv[1], "on") == 0) {
    status_led::set(status_led::State::Booting);
    printf("LED back to automatic\n");
    return 0;
  }
  printf("usage: led [on|off]\n");
  return 1;
}

#ifdef KEYSTONE_HAVE_MATTER
// experiment/commissioner-on-s3: the node id of the last commissioning
// attempt, so a stuck attempt can be aborted.
static chip::NodeId g_matter_pair_node = chip::kUndefinedNodeId;

// The commissioning entry points run a deep CHIP call chain — the PAKE setup,
// the BLE bring-up, the crypto — and all of it happens under the caller's
// stack. The console REPL task has 8 KB, and that was not enough: the brain
// died with "***ERROR*** A stack overflow in task console_repl has been
// detected ***" and panic-rebooted in the middle of an attempt. So the console
// command only checks the arguments and hands the request over; the CHIP call
// happens on this task, which has a stack of its own.
static constexpr std::uint32_t kMatterTaskStackBytes = 12288;

// The stack of the task that creates the CHIP controller. The controller's own
// init (client.init + setup_commissioner) runs the CHIP stack's bring-up,
// which is a deep call chain — the same reason the commissioning has a task
// of its own.
static constexpr std::uint32_t kMatterCtrlTaskStackBytes = 8192;

// How long the controller task waits for the Wi-Fi interface to have a usable
// IPv6 link-local address. The interface comes up with the Wi-Fi driver (the
// Matter stack starts it on its own event loop), and the address needs the
// duplicate address detection to finish — about a second in total.
static constexpr std::uint32_t kMatterIpv6WaitMs = 10000;

// How long the console waits for the co-processor's answer.
static constexpr std::uint32_t kThreadDatasetWaitMs = 15000;

struct MatterRequest {
  enum class Kind { kNone, kPairCode, kPairBle, kPairBleThread, kCancel };
  Kind kind = Kind::kNone;
  chip::NodeId node_id = chip::kUndefinedNodeId;
  std::uint32_t pincode = 0;
  std::uint16_t disc = 0;
  char payload[80] = {0};  // the manual code / QR text for kPairCode
};

static MatterRequest g_matter_req;
static SemaphoreHandle_t g_matter_req_sem = nullptr;

// CHIP keeps the pairing delegate registered when a discovery attempt ends
// without a device: RendezvousCleanup() only reports OnPairingComplete once a
// device has actually connected, so a failed scan leaves the delegate in
// place and the next attempt is refused with "There is already a pairing
// process" (esp_err 259). StopPairing() resets the pairer's state; clearing
// the delegate then lets the next attempt start. Note that the radio is NOT
// stopped by the cancel: CHIP's NimBLE layer implements CancelConnection() as
// CHIP_ERROR_NOT_IMPLEMENTED, so the GAP scan keeps running until its 60 s
// window expires and a scan started right after a cancel fails with
// CHIP_ERROR_INVALID_STATE ("Failed to start a BLE scan: 3").
// The CHIP stack lock must be held.
static void matter_pair_abort_locked() {
  auto& client = esp_matter::controller::matter_controller_client::get_instance();
  auto* comm = client.get_commissioner();
  if (comm == nullptr || comm->GetPairingDelegate() == nullptr) {
    return;
  }
  if (g_matter_pair_node != chip::kUndefinedNodeId) {
    const CHIP_ERROR err = comm->StopPairing(g_matter_pair_node);
    printf("matter: stopped the previous attempt (%s)\n", chip::ErrorStr(err));
  }
  comm->RegisterPairingDelegate(nullptr);
}

// The one task that talks to the CHIP commissioner. Everything that ends up
// in CHIP (PairDevice, StopPairing, the crypto behind them) runs here.
static void matter_worker_task(void* arg) {
  (void)arg;
  for (;;) {
    if (xSemaphoreTake(g_matter_req_sem, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    const MatterRequest req = g_matter_req;
    esp_err_t err = ESP_FAIL;
    {
      esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
      auto& client = esp_matter::controller::matter_controller_client::get_instance();
      auto* comm = client.get_commissioner();
      if (comm != nullptr) {
        switch (req.kind) {
          case MatterRequest::Kind::kCancel: {
            matter_pair_abort_locked();
            err = ESP_OK;
            break;
          }
          case MatterRequest::Kind::kPairBle: {
            // Abort the previous attempt first: CHIP refuses a second pairing
            // while the delegate of the first one is still registered.
            matter_pair_abort_locked();
            g_matter_pair_node = req.node_id;
            static std::uint8_t no_dataset[1] = {0};
            err = esp_matter::controller::pairing_ble_thread(req.node_id, req.pincode, req.disc,
                                                             no_dataset, 0);
            break;
          }
          case MatterRequest::Kind::kPairBleThread: {
            // The same, but with the dataset the co-processor lent us: the device
            // can then find a Thread network to join.
            matter_pair_abort_locked();
            g_matter_pair_node = req.node_id;
            if (!g_thread_dataset.valid()) {
              err = ESP_ERR_INVALID_STATE;
              break;
            }
            err = esp_matter::controller::pairing_ble_thread(
                req.node_id, req.pincode, req.disc, g_thread_dataset.mutable_data(),
                static_cast<std::uint8_t>(g_thread_dataset.size()));
            break;
          }
          case MatterRequest::Kind::kPairCode: {
            matter_pair_abort_locked();
            g_matter_pair_node = req.node_id;
            err = esp_matter::controller::pairing_code(req.node_id, req.payload);
            break;
          }
          default:
            break;
        }
      }
    }
    ESP_LOGI(kTag, "matter: worker done (%s), %u B of its stack left",
             esp_err_to_name(err),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
  }
}

// Hand the request to the worker and return at once — the console must not
// wait for the CHIP stack lock.
static bool matter_enqueue(const MatterRequest& req) {
  if (g_matter_req_sem == nullptr) {
    printf("matter: the commissioner is not running\n");
    return false;
  }
  if (!g_matter_controller_ready) {
    printf("matter: the controller is not up yet: %s\n", g_matter_status);
    return false;
  }
  g_matter_req = req;
  xSemaphoreGive(g_matter_req_sem);
  return true;
}

// experiment/commissioner-on-s3: minimal entry point to drive the Matter
// controller. `matter_pair <node_id> <setup-code>` hands the setup payload
// (manual pairing code or the MT: QR text) to esp_matter_controller. The
// controller starts BLE discovery + PASE; for a Thread device (TIMMERFLOTTE)
// there is no Thread transport on brain-s3 and no dataset to hand over, so the
// commissioning will fail AFTER PASE at the network step — that is the point
// of this test (docs/matter-backlog.md §3, option A). The log will show
// whether SPAKE2+ with a real Matter device worked at all.
int cmd_matter_pair(int argc, char** argv) {
  if (argc != 3) {
    printf("usage: matter_pair <node_id> <setup-code>\n"
           "  node_id:    decimal, the id you assign in our fabric (any >1)\n"
           "  setup-code: 11-digit manual code (\"12345678901\") or QR payload (\"MT:...\")\n");
    return 1;
  }
  const chip::NodeId node_id = static_cast<chip::NodeId>(std::strtoull(argv[1], nullptr, 10));
  if (node_id < 2) {
    printf("node_id must be >= 2 (1 is reserved for the commissioner itself)\n");
    return 1;
  }
  MatterRequest req;
  req.kind = MatterRequest::Kind::kPairCode;
  req.node_id = node_id;
  std::snprintf(req.payload, sizeof(req.payload), "%s", argv[2]);
  if (!matter_enqueue(req)) {
    return 1;
  }
  printf("matter: pairing node 0x%016llx with code '%s' queued\n",
         static_cast<unsigned long long>(node_id), argv[2]);
  printf("matter: watch the log for PASE/CASE progress\n");
  return 0;
}

// experiment/commissioner-on-s3, option A: the pure BLE path. The controller
// scans BLE for a device that advertises `discriminator`, connects to it and
// runs PASE (SPAKE2+) with `pincode` over the BLE GATT link. The Thread
// operational dataset is empty on purpose — brain-s3 has no Thread radio, so
// this test can only prove the BLE stack and the controller up to PASE; the
// device will not get a fabric afterwards (docs/matter-backlog.md §3).
int cmd_matter_pair_ble(int argc, char** argv) {
  if (argc != 4) {
    printf("usage: matter_pair_ble <node_id> <pincode> <discriminator>\n"
           "  node_id:      decimal, the id you assign in our fabric (any >1)\n"
           "  pincode:      the setup PIN of the device, decimal (1..99999998,\n"
           "                chip::kSetupPINCodeMaximumValue — a Matter manual code\n"
           "                packs it in bits, so it is not the digits you read)\n"
           "  discriminator: the 12-bit setup discriminator (0..4095)\n");
    return 1;
  }
  const chip::NodeId node_id = static_cast<chip::NodeId>(std::strtoull(argv[1], nullptr, 10));
  const std::uint32_t pincode = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
  const std::uint16_t disc = static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10));
  if (node_id < 2 || pincode == 0 || pincode > 99999998 || disc > 0xFFF) {
    printf("bad arguments: node_id >= 2, pincode 1..99999998, discriminator 0..4095\n");
    return 1;
  }
  printf("matter: BLE commissioning node 0x%016llx, pin %u, discriminator %u queued\n",
         static_cast<unsigned long long>(node_id), pincode, disc);
  MatterRequest req;
  req.kind = MatterRequest::Kind::kPairBle;
  req.node_id = node_id;
  req.pincode = pincode;
  req.disc = disc;
  if (!matter_enqueue(req)) {
    return 1;
  }
  printf("matter: watch the log for PASE/CASE progress\n");
  return 0;
}

// experiment/commissioner-on-s3: abort a commissioning attempt that is still
// running (the BLE scan waits ~60 s for the device).
int cmd_matter_pair_cancel(int argc, char** argv) {
  (void)argc;
  (void)argv;
  MatterRequest req;
  req.kind = MatterRequest::Kind::kCancel;
  if (!matter_enqueue(req)) {
    return 1;
  }
  printf("matter: cancelling the commissioning attempt\n");
  return 0;
}

// experiment/commissioner-on-s3: the same as matter_pair_ble, but with a Thread
// network to hand to the device. The brain has no 802.15.4 radio, so the
// dataset comes from the co-processor that has one: the console asks for it
// (hubproto ThreadDatasetReq), waits for the answer and only then starts the
// commissioning. Without a dataset the device refuses the network
// (networkingStatus=1) and the commissioning dies at the Thread step.
int cmd_matter_pair_thread(int argc, char** argv) {
  if (argc != 4) {
    printf("usage: matter_pair_thread <node_id> <pincode> <discriminator>\n"
           "  node_id:      decimal, the id you assign in our fabric (any >1)\n"
           "  pincode:      the setup PIN of the device, decimal (1..99999998)\n"
           "  discriminator: the 12-bit setup discriminator (0..4095)\n"
           "  the Thread dataset is borrowed from the co-processor (chip 0x02)\n");
    return 1;
  }
  const chip::NodeId node_id = static_cast<chip::NodeId>(std::strtoull(argv[1], nullptr, 10));
  const std::uint32_t pincode = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
  const std::uint16_t disc = static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10));
  if (node_id < 2 || pincode == 0 || pincode > 99999998 || disc > 0xFFF) {
    printf("bad arguments: node_id >= 2, pincode 1..99999998, discriminator 0..4095\n");
    return 1;
  }
  if (g_bus == nullptr || g_thread_dataset_sem == nullptr) {
    printf("matter: the commissioner is not running\n");
    return 1;
  }
  // Forget an answer from an earlier attempt, so we do not start with a stale
  // dataset.
  g_thread_dataset.clear();
  while (xSemaphoreTake(g_thread_dataset_sem, 0) == pdTRUE) {
  }
  hubproto::Header q{};
  q.version = hubproto::kProtocolVersion;
  q.type = hubproto::MsgType::ThreadDatasetReq;
  q.chip_id = kThreadChipId;
  if (!g_bus->publish(q, nullptr, 0)) {
    printf("matter: cannot reach the co-processor (chip 0x%02x)\n", kThreadChipId);
    return 1;
  }
  printf("matter: asked chip 0x%02x for its Thread dataset, waiting %u ms...\n",
         kThreadChipId, static_cast<unsigned>(kThreadDatasetWaitMs));
  if (xSemaphoreTake(g_thread_dataset_sem, pdMS_TO_TICKS(kThreadDatasetWaitMs)) != pdTRUE) {
    printf("matter: no dataset in %u ms (is the co-processor alive?)\n",
           static_cast<unsigned>(kThreadDatasetWaitMs));
    return 1;
  }
  if (!g_thread_dataset.valid()) {
    printf("matter: the co-processor sent a dataset we cannot use\n");
    return 1;
  }
  printf("matter: dataset %u bytes, channel %u, network '%s', ext pan id %s\n",
         static_cast<unsigned>(g_thread_dataset.size()),
         static_cast<unsigned>(g_thread_dataset.channel()),
         g_thread_dataset.network_name(),
         g_thread_dataset.ext_pan_id());
  MatterRequest req;
  req.kind = MatterRequest::Kind::kPairBleThread;
  req.node_id = node_id;
  req.pincode = pincode;
  req.disc = disc;
  if (!matter_enqueue(req)) {
    return 1;
  }
  printf("matter: BLE commissioning node 0x%016llx, pin %u, discriminator %u queued\n",
         static_cast<unsigned long long>(node_id), pincode, disc);
  printf("matter: watch the log for PASE/CASE progress\n");
  return 0;
}
#endif  // KEYSTONE_HAVE_MATTER

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
  reg_cmd("loc", "loc [<lat> <lon> | clear] - the hub's location, for sunrise/sunset", &cmd_loc);
  reg_cmd("sun", "today's sunrise and sunset", &cmd_sun);
  reg_cmd("scene", "scene [<name> | add <name> <id> <attr> <value> ... | del <name>] - named groups of actions", &cmd_scene);
  reg_cmd("cancel", "cancel <n> - cancel a pending timer", &cmd_cancel);
  reg_cmd("at", "at [<YYYY-MM-DD>] <HH:MM> <on|off|toggle> <id> [daily|weekdays|weekends|mon,tue,..] - local-time action, recurring or once on a date", &cmd_at);
  reg_cmd("schedules", "list recurring time-of-day schedules", &cmd_schedules);
  reg_cmd("inject", "dev: pretend a device of a type exists (Matter chip only); inject <kind>|set|clear", &cmd_inject);
  reg_cmd("events", "events [<n>] - the newest hub events (what the app's history shows), newest first", &cmd_events);
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
  reg_cmd("wifi_forget", "erase the saved Wi-Fi credentials", &cmd_wifi_forget);
  reg_cmd("wifi_scan", "scan for nearby Wi-Fi networks", &cmd_wifi_scan);
  reg_cmd("setup", "setup [on|off] - the first-time setup access point: status, or open/close it", &cmd_setup);
  reg_cmd("time", "show current wall-clock time and sync status", &cmd_time);
  reg_cmd("tz", "tz <posix_tz> - get or set timezone", &cmd_tz);
  reg_cmd("ota", "ota <zigbee|thread> <url> <sha256> | dev <id> <url> <sha256> | status | abort - update a C6 or a device", &cmd_ota);
  reg_cmd("desired", "desired [clear <id_or_name>|all] - what the hub last asked each attribute to be", &cmd_desired);
  reg_cmd("rule", "rule [add|del] ... - persistent if-then rules (see `rule help`)", &cmd_rule);
  reg_cmd("name", "name [<id> <name> | clear <id_or_name>] - manage entity names", &cmd_name);
  reg_cmd("relay", "relay [<secret> | <host> <secret> | clear] - remote access tunnel", &cmd_relay);
  reg_cmd("led", "led [on|off] - status LED (green=healthy, amber=degraded, red=offline)", &cmd_led);
#ifdef KEYSTONE_HAVE_MATTER
  reg_cmd("matter_pair",
          "matter_pair <node_id> <setup-code> - try to commission a Matter device from the hub",
          &cmd_matter_pair);
  reg_cmd("matter_pair_ble",
          "matter_pair_ble <node_id> <pincode> <discriminator> - commission a Matter device over BLE",
          &cmd_matter_pair_ble);
  reg_cmd("matter_pair_cancel", "matter_pair_cancel - abort a running Matter commissioning attempt", &cmd_matter_pair_cancel);
  reg_cmd("matter_pair_thread",
          "matter_pair_thread <node_id> <pincode> <discriminator> - commission a Matter device with the Thread dataset of the co-processor",
          &cmd_matter_pair_thread);
  reg_cmd("ip6", "ip6 [<address> | probe <address> [port]] - the hub's IPv6 addresses, the route to the Thread mesh, a probe datagram",
          &cmd_ip6);
#endif

  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

}  // namespace

namespace {

// ---- Hub send/broadcast dispatch (local + relay) ------------------------------------
// Replies and broadcasts go to local WebSocket clients (appsrv) AND relay-connected
// remote clients (relay). The fd sign distinguishes them: real socket fds are ≥ 0,
// relay virtual fds are ≤ relay::kFdBase (-100).
constexpr int kBroadcastFd = -1;  // sentinel for app_send_snapshot's "send to everyone" mode

void hub_send(int fd, const char* data, std::size_t len) {
  if (relay::is_relay_fd(fd)) relay::send(fd, data, len);
  else appsrv::send(fd, data, len);
}
void hub_broadcast(const char* data, std::size_t len) {
  appsrv::broadcast(data, len);
  relay::broadcast(data, len);
}
std::size_t hub_client_count() {
  return appsrv::client_count() + relay::client_count();
}

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

// How the last attempt to add a Matter device stands. The main loop owns it: the chip's answer
// arrives in the UART task (queued) and the app asks for it (`matter.status`) or hears about it
// (the `matter` push).
struct MatterStatus {
  hubproto::MatterState state = hubproto::MatterState::Idle;
  hubproto::MatterCode code = hubproto::MatterCode::Ok;
  std::uint16_t entity = 0;
  std::uint32_t deadline_ms = 0;  // when the attempt is given up on (0: nothing running)
};
MatterStatus g_matter;

// Has the Matter chip ever answered a MatterPair? A chip that is alive but silent
// has no commissioner — then a second attempt is refused right away instead of
// making the app wait another minute.
bool g_matter_cannot = false;

// How long the hub waits for the Matter chip to say something before it gives up.
constexpr std::uint32_t kMatterTimeoutMs = 60000;

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
  if (w.ok()) hub_send(fd, b, w.size());
}

void app_on_cmd_result(std::uint16_t seq, hubproto::CmdAckStatus status) {
  if (g_app_send.task != nullptr && g_app_send.task == xTaskGetCurrentTaskHandle()) {
    g_app_send.delivered = true;
    g_app_send.status = status;
    return;
  }
  if (g_app_pending_mu == nullptr) return;
  // A pending entry carries any client fd: a local WebSocket client (>= 0) or a remote one
  // through the relay (<= relay::kFdBase). "Found" is therefore a flag, not fd >= 0 —
  // testing the sign silently threw away every ack for a remote client.
  bool found = false;
  int fd = 0;
  std::int32_t id = 0;
  xSemaphoreTake(g_app_pending_mu, portMAX_DELAY);
  for (auto& p : g_app_pending) {
    if (p.used && p.seq == seq) {
      fd = p.fd;
      id = p.id;
      p.used = false;
      found = true;
      break;
    }
  }
  xSemaphoreGive(g_app_pending_mu);
  if (found) app_send_ack(fd, id, status);
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
  const Scenes& scenes;
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
  std::uint8_t scene_slots[8] = {};
  std::size_t n_scenes = 0;

  AppSource(const Registry& r, const Rules& ru, const Schedules& sc, const core::TimerTable<8>& tm,
            const Scenes& scn, const core::RadioCoordinator<Registry>& co, std::uint32_t t)
      : reg(r), rules(ru), sched(sc), timers(tm), scenes(scn), coord(co), now(t) {
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
    for (std::uint8_t i = 0; i < 8; ++i) if (scenes.get(i) != nullptr) scene_slots[n_scenes++] = i;
  }

  std::size_t count(core::app::Section s) const {
    switch (s) {
      case core::app::Section::Entities: return n_ents;
      case core::app::Section::Chips: return n_chips;
      case core::app::Section::Rules: return rules.rule_count();
      case core::app::Section::Schedules: return n_sched;
      case core::app::Section::Timers: return n_timers;
      case core::app::Section::Scenes: return n_scenes;
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
      case core::app::Section::Rules: {
        const auto* ru = rules.rule_at(i);
        core::app::write_rule(w, i, *ru, scene_name_for(ru->action_entity, ru->action_attr));
        break;
      }
      case core::app::Section::Schedules: {
        const auto* sc = sched.get(sched_slots[i]);
        core::app::write_schedule(w, sched_slots[i], *sc, scene_name_for(sc->entity_id, sc->attr_id));
        break;
      }
      case core::app::Section::Timers: {
        const auto* tm = timers.get(timer_slots[i]);
        core::app::write_timer(w, core::app::TimerView{timer_slots[i], tm->entity_id, tm->attr_id, tm->value,
                                                       app_timer_left_s(*tm, now),
                                                       scene_name_for(tm->entity_id, tm->attr_id)});
        break;
      }
      case core::app::Section::Scenes:
        core::app::write_scene(w, *scenes.get(scene_slots[i]));
        break;
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
      core::app::write_rule(w, i, *rules.rule_at(i), scene_name_for(rules.rule_at(i)->action_entity, rules.rule_at(i)->action_attr));
      mix(tmp, w.size());
    }
    for (std::size_t i = 0; i < n_sched; ++i) {
      core::JsonWriter w(tmp, sizeof(tmp));
      core::app::write_schedule(w, sched_slots[i], *sched.get(sched_slots[i]),
                                scene_name_for(sched.get(sched_slots[i])->entity_id, sched.get(sched_slots[i])->attr_id));
      mix(tmp, w.size());
    }
    char big[1024];
    for (std::size_t i = 0; i < n_scenes; ++i) {
      core::JsonWriter w(big, sizeof(big));
      core::app::write_scene(w, *scenes.get(scene_slots[i]));
      mix(big, w.size());
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
  // 4 KB snapshot frame: in PSRAM via EXT_RAM_BSS_ATTR so it does not sit in
  // the brain's internal-DRAM BSS (experiment/commissioner-on-s3: each KB
  // freed before the heap pool starts widens the pool and the BLE controller's
  // contiguous allocation gate with it).
  static EXT_RAM_BSS_ATTR char buf[core::app::kMaxFrame];
  core::app::SnapshotCursor cur;
  while (!cur.done()) {
    core::JsonWriter w(buf, sizeof(buf));
    // Items are at most ~1 KB (an entity with 8 attributes); 3000 leaves room for one.
    if (!core::app::encode_snapshot_part(cur, const_cast<AppSource&>(src), w, 3000)) {
      ESP_LOGE(kTag, "app: snapshot part %u did not fit a frame", static_cast<unsigned>(cur.part));
      return;
    }
    if (fd == kBroadcastFd) hub_broadcast(buf, w.size());
    else hub_send(fd, buf, w.size());
  }
}

void app_handle_cmd(const appsrv::Inbound& in, const Registry& reg, Dispatcher& disp, std::uint32_t now) {
  using namespace core::app;
  char b[160];
  core::JsonWriter w(b, sizeof(b));
  auto fail = [&](Err e, const char* msg = nullptr) {
    encode_error(w, in.id, e, msg);
    hub_send(in.fd, b, w.size());
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
  bool sent;
  {
    CmdSrcScope src(core::CmdSource::App);
    sent = disp.send(entity, attr, v, now, seq);
  }
  const AppSendCtx done = g_app_send;
  g_app_send = AppSendCtx{};
  if (!sent && !done.delivered) return fail(Err::Internal, "command queue full");

  encode_accepted(w, in.id, seq);
  hub_send(in.fd, b, w.size());
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
// Devices the hub was asked to unpair. The Zigbee chip answers a successful unpair with a fresh HELLO, so the
// brain's registry loses the entity; once the chip has declared again without it, the Matter chip is told to
// drop the endpoint that mirrored it (Undeclare), so Apple Home does not keep a ghost accessory. Removal is
// tied to this explicit request, not to "an entity is missing": one lost DECLARE frame must never delete an
// endpoint people have put in rooms and automations. Main loop only.
constexpr std::uint32_t kUnpairWatchMs = 120000;  // a device that never left is forgotten after this
struct PendingUnpair {
  bool used = false;
  std::uint16_t entity = 0;
  std::uint32_t since_ms = 0;
} g_unpair[4];

void note_unpair(std::uint16_t entity, std::uint32_t now) {
  PendingUnpair* free_slot = nullptr;
  for (auto& u : g_unpair) {
    if (u.used && u.entity == entity) { u.since_ms = now; return; }
    if (!u.used && free_slot == nullptr) free_slot = &u;
  }
  if (free_slot != nullptr) *free_slot = PendingUnpair{true, entity, now};
}

// A chip-meta COMMAND: entity = the chip's self entity, attr = the op (core::app::kMeta*), the
// u16 value its argument. Mirrored in zigbee-c6 handle_meta.
void send_chip_meta(core::Bus& bus, std::uint8_t chip, std::uint8_t attr, std::uint16_t arg) {
  if (attr == core::app::kMetaUnpair) note_unpair(arg, now_ms(nullptr));
  if (attr == core::app::kMetaPair) {
    ev_plain(core::EventKind::Pairing, static_cast<std::uint8_t>(core::PairingEv::Opened), arg);
  } else {
    ev_plain(core::EventKind::Pairing,
             static_cast<std::uint8_t>(attr == core::app::kMetaUnpair ? core::PairingEv::Unpair : core::PairingEv::Identify),
             0, arg);
  }
  const std::uint8_t payload[5] = {0, 0, attr, static_cast<std::uint8_t>(arg & 0xFF),
                                   static_cast<std::uint8_t>((arg >> 8) & 0xFF)};
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = hubproto::MsgType::Command;
  h.chip_id = chip;
  h.entity_id = hubproto::kSelfEntityId;
  bus.publish(h, payload, sizeof(payload));
}

void persist_webtoken(const char* token);
bool restore_webtoken(char* out, std::size_t cap);  // defined with the app glue, below
void app_hub_info(core::app::HubInfo& h);          // defined with the app glue, below
void setup_token_request(const char* token, std::uint32_t now);  // defined with the setup code, below

// Live history: what the ring got since the last push goes to every client at once, newest first, so the
// History tab does not wait for its next poll. At most a page per call; a burst continues on the next
// pass, and a client that missed one asks with `history` (the push says `latest`). With nobody connected
// the cursor just follows the ring, so a client that arrives later is not sent the backlog.
std::uint32_t g_ev_pushed = 0;
std::uint32_t g_ev_push_ms = 0;

void history_push(Scenes& scenes, std::uint32_t now_ms) {
  if (g_ev_mu == nullptr || now_ms - g_ev_push_ms < 250) return;
  g_ev_push_ms = now_ms;
  // 4 KB history frame + ~576 B event page in PSRAM (see app_send_snapshot).
  static EXT_RAM_BSS_ATTR char hb[core::app::kMaxFrame];
  static EXT_RAM_BSS_ATTR core::Event page[core::app::kHistoryPage];
  xSemaphoreTake(g_ev_mu, portMAX_DELAY);
  const std::uint32_t latest = g_ev_ring.latest();
  std::size_t n = 0;
  if (hub_client_count() == 0) {
    g_ev_pushed = latest;
  } else {
    n = g_ev_ring.read_after(g_ev_pushed, core::app::kHistoryPage, page);
  }
  xSemaphoreGive(g_ev_mu);
  if (n == 0) return;
  g_ev_pushed = page[n - 1].seq;
  for (std::size_t i = 0; i < n / 2; ++i) std::swap(page[i], page[n - 1 - i]);  // oldest-first -> newest-first
  core::JsonWriter w(hb, sizeof(hb));
  core::app::encode_history_push(w, s_time_synced ? static_cast<std::int64_t>(time(nullptr)) : 0,
                                 static_cast<std::uint32_t>(esp_timer_get_time() / 1000000), page, n,
                                 latest,
                                 [](void* sc, std::uint8_t id) -> const char* {
                                   const auto* found = static_cast<Scenes*>(sc)->find_by_id(id);
                                   return found != nullptr ? found->name : nullptr;
                                 },
                                 &scenes);
  if (w.ok()) hub_broadcast(hb, w.size());
}

// ---- delayed hub actions -------------------------------------------------------------------
//
// A few requests change the hub itself: a reboot, a factory reset, a new app token, forgetting
// the Wi-Fi network, a new relay. The reply has to reach the client first, so the work happens a
// moment later, in the main loop (the same pattern the setup code uses for the network and the
// token).

enum class HubAction : std::uint8_t { None, Reboot, FactoryReset, SetToken, ForgetWifi, RestartRelay, ClearRelay };

struct PendingHubAction {
  bool pending = false;
  HubAction kind = HubAction::None;
  std::uint32_t at = 0;  // the main loop's clock
  char token[core::app::kMaxToken + 1] = {};
};

PendingHubAction g_hub_action;

constexpr std::uint32_t kHubActionDelayMs = 300;      // a reboot: the reply is out by then
constexpr std::uint32_t kFactoryResetDelayMs = 500;  // a factory reset erases and reboots

void hub_action_request(HubAction kind, std::uint32_t now, std::uint32_t delay_ms,
                         const char* token = nullptr) {
  g_hub_action.kind = kind;
  g_hub_action.at = now + delay_ms;
  g_hub_action.pending = true;
  if (token != nullptr) {
    std::snprintf(g_hub_action.token, sizeof(g_hub_action.token), "%s", token);
  } else {
    g_hub_action.token[0] = '\0';
  }
}

void app_handle_req(const appsrv::Inbound& in, core::Bus& bus, Dispatcher& disp, Registry& reg, Rules& rules,
                    Schedules& sched, core::TimerTable<8>& timers, Scenes& scenes, std::uint32_t now) {
  using namespace core::app;
  core::JsonReader reader;
  const ClientMsg m = decode_client(reader, in.text, in.len);
  char b[192];
  core::JsonWriter w(b, sizeof(b));

  Outcome o = Outcome::fail(Err::Unsupported, m.op);
  const char* op = m.kind == ClientMsg::Kind::Req ? m.op : "";
  if (m.kind != ClientMsg::Kind::Req) {
    o = Outcome::fail(Err::BadArgs, "not a request");
  } else if (in.setup_only && !core::setup_op_allowed(op)) {
    o = Outcome::fail(Err::Unsupported, "setup only");  // the server already refuses these; this is the second lock
  } else if (!std::strcmp(op, "setup.apply")) {
    if (!in.setup_only) {
      o = Outcome::fail(Err::Unsupported, "setup only");
    } else {
      char ssid[core::kMaxSsidLen + 2], pass[core::kMaxPasswordLen + 4], token[kMaxToken + 2];
      o = setup_parse(reader, m.args, ssid, sizeof(ssid), pass, sizeof(pass), token, sizeof(token));
      if (o.ok) {
        // Both are applied ~0.7 s from now, after this reply has gone out: the token (the app server restarts
        // with it) and the network (the hub leaves the access point's channel for the home router's).
        setup_token_request(token, now);
        wifi_request(ssid, pass, now);
        ESP_LOGI(kTag, "setup: network %s and a new app token accepted", ssid);
      }
      std::memset(pass, 0, sizeof(pass));
      std::memset(token, 0, sizeof(token));
    }
  } else if (!std::strcmp(op, "name.set")) {
    o = name_set(reg, reader, m.args);
    if (o.ok) persist_names(reg.names());
  } else if (!std::strcmp(op, "name.clear")) {
    o = name_clear(reg, reader, m.args);
    if (o.ok) persist_names(reg.names());
  } else if (!std::strcmp(op, "rule.add")) {
    o = rule_add(reg, scenes, rules, reader, m.args);
    if (o.ok) persist_rules(rules);
  } else if (!std::strcmp(op, "rule.del")) {
    o = rule_del(rules, reader, m.args);
    if (o.ok) persist_rules(rules);
  } else if (!std::strcmp(op, "schedule.add")) {
    o = schedule_add(reg, scenes, sched, reader, m.args, local_today());
    if (o.ok) persist_schedules(sched);
  } else if (!std::strcmp(op, "schedule.del")) {
    o = schedule_del(sched, reader, m.args);
    if (o.ok) persist_schedules(sched);
  } else if (!std::strcmp(op, "timer.add")) {
    time_t epoch = 0;
    if (s_time_synced) time(&epoch);
    o = timer_add(reg, scenes, timers, reader, m.args, now, static_cast<std::uint64_t>(epoch));
    if (o.ok) persist_timers(timers, now);
  } else if (!std::strcmp(op, "timer.cancel")) {
    o = timer_cancel(timers, reader, m.args);
    if (o.ok) persist_timers(timers, now);
  } else if (!std::strcmp(op, "wifi.set")) {
    char ssid[core::kMaxSsidLen + 2], pass[core::kMaxPasswordLen + 4];
    o = wifi_parse(reader, m.args, ssid, sizeof(ssid), pass, sizeof(pass));
    if (o.ok) wifi_request(ssid, pass, now);  // applied in ~0.7 s, after this reply has gone out
    std::memset(pass, 0, sizeof(pass));
  } else if (!std::strcmp(op, "wifi.scan")) {
    // The scan blocks for a couple of seconds; it is the whole reply, so answer here.
    static EXT_RAM_BSS_ATTR char sb[1200];
    struct Net { char ssid[core::kMaxSsidLen + 1]; int rssi; bool secure; };
    static EXT_RAM_BSS_ATTR Net nets[16];
    std::size_t n = 0;
    wifi_scan_config_t sc = {};
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
      uint16_t found = 0;
      esp_wifi_scan_get_ap_num(&found);
      uint16_t take = found > 40 ? 40 : found;
      static EXT_RAM_BSS_ATTR wifi_ap_record_t recs[40];
      if (esp_wifi_scan_get_ap_records(&take, recs) == ESP_OK) {
        for (uint16_t i = 0; i < take; ++i) {
          const char* name = reinterpret_cast<const char*>(recs[i].ssid);
          if (name[0] == '\0') continue;  // hidden network
          std::size_t k = 0;
          while (k < n && std::strcmp(nets[k].ssid, name) != 0) ++k;  // one entry per name: the strongest
          if (k < n) {
            if (recs[i].rssi > nets[k].rssi) nets[k].rssi = recs[i].rssi;
            continue;
          }
          if (n == 16) continue;
          std::snprintf(nets[n].ssid, sizeof(nets[n].ssid), "%s", name);
          nets[n].rssi = recs[i].rssi;
          nets[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
          ++n;
        }
      }
      for (std::size_t i = 1; i < n; ++i) {  // strongest first (insertion sort, n <= 16)
        const Net cur = nets[i];
        std::size_t j = i;
        while (j > 0 && nets[j - 1].rssi < cur.rssi) { nets[j] = nets[j - 1]; --j; }
        nets[j] = cur;
      }
      core::JsonWriter sw(sb, sizeof(sb));
      encode_res_networks(sw, in.id, nets, n);
      if (sw.ok()) {
        hub_send(in.fd, sb, sw.size());
        ESP_LOGI(kTag, "app: req wifi.scan -> %u network(s)", static_cast<unsigned>(n));
        return;
      }
      o = Outcome::fail(Err::Internal, "scan result too large");
    } else {
      o = Outcome::fail(Err::Internal, "scan failed");
    }
  } else if (!std::strcmp(op, "history")) {
    // Newest-first page of the event ring (core/event_log.hpp). Main loop only, so the buffers can be static.
    core::app::HistoryQuery q;
    o = core::app::history_parse(reader, m.args, q);
    if (o.ok) {
      static EXT_RAM_BSS_ATTR char hb[core::app::kMaxFrame];
      static EXT_RAM_BSS_ATTR core::Event page[core::app::kHistoryPage + 1];
      std::size_t n = 0;
      std::uint32_t oldest = 0, latest = 0;
      xSemaphoreTake(g_ev_mu, portMAX_DELAY);
      n = g_ev_ring.read(q.before, q.limit + 1, page, q.entity_only, q.entity);  // one more than asked: is there more?
      oldest = g_ev_ring.oldest();
      latest = g_ev_ring.latest();
      xSemaphoreGive(g_ev_mu);
      const bool more = n > q.limit;
      if (more) n = q.limit;
      core::JsonWriter hw(hb, sizeof(hb));
      encode_res_history(hw, in.id, s_time_synced ? static_cast<std::int64_t>(time(nullptr)) : 0,
                         static_cast<std::uint32_t>(esp_timer_get_time() / 1000000), page, n, oldest, latest, more,
                         [](void* sc, std::uint8_t id) -> const char* {
                           const auto* found = static_cast<Scenes*>(sc)->find_by_id(id);
                           return found != nullptr ? found->name : nullptr;
                         },
                         &scenes);
      if (hw.ok()) {
        hub_send(in.fd, hb, hw.size());
        ESP_LOGI(kTag, "app: req history -> %u event(s)", static_cast<unsigned>(n));
        return;
      }
      o = Outcome::fail(Err::Internal, "history too large");
    }
  } else if (!std::strcmp(op, "location.set")) {
    double lat = 0, lon = 0;
    o = location_parse(reader, m.args, lat, lon);
    if (o.ok) set_location(lat, lon);
  } else if (!std::strcmp(op, "location.clear")) {
    clear_location();
    o = Outcome::done();
  } else if (!std::strcmp(op, "scene.put")) {
    o = scene_put(reg, scenes, reader, m.args);
    if (o.ok) persist_scenes(scenes);
  } else if (!std::strcmp(op, "scene.del")) {
    o = scene_del(scenes, reader, m.args, [&](std::uint8_t id) {
      for (std::size_t i = 0; i < 8; ++i) {
        const auto* s = sched.get(i);
        if (s != nullptr && s->entity_id == core::kSceneTarget && s->attr_id == id) return true;
        const auto* t = timers.get(i);
        if (t != nullptr && t->entity_id == core::kSceneTarget && t->attr_id == id) return true;
      }
      for (std::size_t i = 0; i < rules.rule_count(); ++i) {
        const auto* ru = rules.rule_at(i);
        if (ru->action_entity == core::kSceneTarget && ru->action_attr == id) return true;
      }
      return false;
    });
    if (o.ok) persist_scenes(scenes);
  } else if (!std::strcmp(op, "scene.apply")) {
    o = scene_apply(reg, scenes, reader, m.args, [&](std::uint16_t e, std::uint8_t a, const hubproto::Value& v) {
      CmdSrcScope src(core::CmdSource::Scene);
      std::uint16_t seq = 0;
      return disp.send(e, a, v, now, seq);
    });
    if (o.ok) {
      // The name was resolved inside scene_apply; read it again for the history.
      char sname[24] = {};
      if (reader.get_string(reader.find(m.args, "name"), sname, sizeof(sname))) {
        if (const auto* sc = scenes.find(sname)) ev_plain(core::EventKind::Scene, static_cast<std::uint8_t>(core::SceneVia::App), sc->id);
      }
    }
  } else if (!std::strcmp(op, "pair") || !std::strcmp(op, "unpair") || !std::strcmp(op, "identify")) {
    // Zigbee chip operations, fire-and-forget like the console: the result is seen as an entity
    // appearing (pair), going away (unpair) or a device blinking (identify).
    const core::app::ChipMeta cm =
        !std::strcmp(op, "pair")
            ? core::app::pair_meta(reader, m.args, kZigbeeChipId)
            : core::app::device_meta(reg, reader, m.args,
                                     !std::strcmp(op, "unpair") ? core::app::kMetaUnpair : core::app::kMetaIdentify,
                                     kZigbeeChipId);
    o = cm.out;
    if (o.ok) send_chip_meta(bus, cm.chip, cm.attr, cm.arg);
  } else if (!std::strcmp(op, "matter.commission")) {
    // The bytes the phone read from a Matter device over Bluetooth. The phone does not
    // interpret them any further — the Matter chip is the only one with a stack that can
    // commission anything — so they go there, and the chip says how far it got.
    const core::app::MatterRequest mreq = core::app::matter_commission_parse(reader, m.args);
    o = mreq.out;
    if (o.ok && !g_thread_seen) {
      o = Outcome::fail(Err::Unsupported, "the hub has no Matter chip");
    } else if (o.ok && g_matter_cannot) {
      // The chip is alive and never answered the last time — it has no commissioner.
      o = Outcome::fail(Err::Unsupported, "this hub's Matter chip cannot add Matter devices");
    }
    if (o.ok) {
      std::uint8_t payload[hubproto::kMatterPairHeader + hubproto::kMatterMaxPayload] = {};
      const auto enc =
          hubproto::encode_matter_pair(mreq.kind, mreq.bytes, mreq.len, payload, sizeof(payload));
      if (!enc.ok) {
        o = Outcome::fail(Err::Internal, "the payload does not fit");
      } else {
        hubproto::Header h{};
        h.version = hubproto::kProtocolVersion;
        h.type = hubproto::MsgType::MatterPair;
        h.chip_id = kThreadChipId;
        bus.publish(h, payload, enc.len);
        g_matter.state = hubproto::MatterState::Scanning;
        g_matter.code = hubproto::MatterCode::Ok;
        g_matter.entity = 0;
        g_matter.deadline_ms = now + kMatterTimeoutMs;
        ESP_LOGI(kTag, "app: matter.commission kind=%u %u byte(s) -> chip 0x%02x",
                 static_cast<unsigned>(mreq.kind), static_cast<unsigned>(mreq.len),
                 static_cast<unsigned>(kThreadChipId));
        // The answer says the bytes were handed over — how the attempt goes is reported
        // by the `matter` push, not here.
        static char mb[64];
        core::JsonWriter mw(mb, sizeof(mb));
        mw.begin_object().kv("type", "res").kv("id", static_cast<std::int64_t>(in.id)).kv("ok", true)
            .key("result").begin_object().kv("state", "started").end_object().end_object();
        if (mw.ok()) {
          hub_send(in.fd, mb, mw.size());
          return;
        }
        o = Outcome::fail(Err::Internal, "matter reply too large");
      }
    }
  } else if (!std::strcmp(op, "matter.status")) {
    // A result of its own shape: sent right away.
    static char mb[256];
    core::JsonWriter mw(mb, sizeof(mb));
    core::app::encode_res_ok_matter(mw, in.id, g_matter.state, g_matter.entity, g_matter.code,
                                     core::app::matter_message(g_matter.state, g_matter.code));
    if (mw.ok()) {
      hub_send(in.fd, mb, mw.size());
      return;
    }
    o = Outcome::fail(Err::Internal, "matter status too large");
  } else if (!std::strcmp(op, "ota.start")) {
    // Update a co-processor over the UART link, exactly as the `ota` console command does.
    core::app::OtaRequest ota_req;
    o = core::app::ota_start_parse(reader, m.args, ota_req);
    if (o.ok) {
      if (g_ota == nullptr) {
        o = Outcome::fail(Err::Internal, "the hub has no update job");
      } else if (g_ota->running()) {
        o = Outcome::fail(Err::Busy, "an update is already running");
      } else {
        char err[96];
        if (!g_ota->start(ota_req.chip, ota_req.url, ota_req.sha256, err, sizeof(err))) {
          o = Outcome::fail(Err::Internal, err);
        } else {
          ESP_LOGI(kTag, "app: update of chip 0x%02x started", static_cast<unsigned>(ota_req.chip));
        }
      }
    }
  } else if (!std::strcmp(op, "ota.status")) {
    // A result of its own shape: sent right away.
    static char ob[256];
    core::app::OtaState st = core::app::OtaState::Idle;
    std::int64_t pct = 0, chip = 0;
    char msg[96] = {};
    ota_app_status(st, pct, chip, msg, sizeof(msg));
    core::JsonWriter ow(ob, sizeof(ob));
    encode_res_ok_ota(ow, in.id, st, pct, chip, msg);
    if (ow.ok()) {
      hub_send(in.fd, ob, ow.size());
      ESP_LOGI(kTag, "app: req ota.status -> %s", core::app::ota_state_name(st));
      return;
    }
    o = Outcome::fail(Err::Internal, "ota status too large");
  } else if (!std::strcmp(op, "hub.reboot")) {
    hub_action_request(HubAction::Reboot, now, kHubActionDelayMs);
    o = Outcome::done();
  } else if (!std::strcmp(op, "hub.factory_reset")) {
    hub_action_request(HubAction::FactoryReset, now, kFactoryResetDelayMs);
    o = Outcome::done();
  } else if (!std::strcmp(op, "access.get")) {
    // What the app's access card shows: the token (the client already has it), the certificate
    // fingerprint it pins, and the relay. The token is never logged.
    char tok[core::app::kMaxToken + 1] = {};
    const bool have = restore_webtoken(tok, sizeof(tok));
    char fp[80] = {};
    if (appsrv::tls_available()) {
      const char* f = appsrv::cert_fingerprint();
      if (f != nullptr) std::snprintf(fp, sizeof(fp), "%s", f);
    }
    char rurl[256] = {};
    relay::configured_url(rurl, sizeof(rurl));
    static char ab[512];
    core::JsonWriter aw(ab, sizeof(ab));
    encode_res_ok_access(aw, in.id, have ? tok : "", fp, rurl[0] != '\0', rurl);
    std::memset(tok, 0, sizeof(tok));  // the copy in the reply is what matters
    if (aw.ok()) {
      hub_send(in.fd, ab, aw.size());
      ESP_LOGI(kTag, "app: req access.get -> token %s, relay %s", have ? "set" : "not set",
               rurl[0] != '\0' ? "set" : "not set");
      return;
    }
    o = Outcome::fail(Err::Internal, "access view too large");
  } else if (!std::strcmp(op, "access.token.set")) {
    // The new token is saved and the app server restarts with it: this connection drops and the
    // client has to come back with the new token.
    char tok[core::app::kMaxToken + 1] = {};
    o = core::app::access_token_parse(reader, m.args, tok, sizeof(tok));
    if (o.ok) hub_action_request(HubAction::SetToken, now, kHubActionDelayMs, tok);
    std::memset(tok, 0, sizeof(tok));
  } else if (!std::strcmp(op, "access.relay.set")) {
    core::app::RelayRequest relay_req;
    o = core::app::access_relay_parse(reader, m.args, relay_req);
    if (o.ok) {
      if (!relay::configure(relay_req.url, relay_req.secret)) {
        o = Outcome::fail(Err::Internal, "cannot store the relay credentials");
      } else {
        // Restarting the relay drops the clients that came through it, and closing the
        // tunnel blocks for a moment - so it happens after the reply is on its way.
        hub_action_request(HubAction::RestartRelay, now, kHubActionDelayMs);
      }
    }
    std::memset(relay_req.secret, 0, sizeof(relay_req.secret));
  } else if (!std::strcmp(op, "access.relay.clear")) {
    hub_action_request(HubAction::ClearRelay, now, kHubActionDelayMs);
    o = Outcome::done();
  } else if (!std::strcmp(op, "wifi.forget")) {
    // The reply goes out first; the hub drops the network right after it.
    hub_action_request(HubAction::ForgetWifi, now, kHubActionDelayMs);
    o = Outcome::done();
  } else if (!std::strcmp(op, "device.ota")) {
    // The firmware of a Zigbee device: the hub fetches the .ota file and pushes it
    // to the coordinator, which serves it to the device (docs/ota.md).
    const core::app::DeviceOta d = core::app::device_ota_meta(reg, reader, m.args, kZigbeeChipId);
    o = d.out;
    if (o.ok) {
      char err[96];
      if (g_dev_ota == nullptr) {
        o = core::app::Outcome::fail(core::app::Err::Internal, "not ready");
      } else if (!g_dev_ota->start(kZigbeeChipId, d.entity, d.url, d.sha256, err, sizeof(err))) {
        o = core::app::Outcome::fail(core::app::Err::Busy, "an update is already running");
      }
    }
  }

  if (o.ok) {
    if (o.has_counts) encode_res_ok_counts(w, in.id, o.sent, o.skipped);
    else if (o.has_index) encode_res_ok_index(w, in.id, o.index);
    else encode_res_ok(w, in.id);
    g_app_force_check = true;
  } else {
    encode_res_error(w, in.id, o.err, o.msg[0] != '\0' ? o.msg : nullptr);
  }
  hub_send(in.fd, b, w.size());
  ESP_LOGI(kTag, "app: req %s -> %s%s%s", op, o.ok ? "ok" : err_name(o.err), o.msg[0] ? " " : "", o.msg);
}

void app_hub_info(core::app::HubInfo& h) {
  const char* tz = std::getenv("TZ");
  h.name = "home";
  h.fw = esp_app_get_description()->version;
  h.time = s_time_synced ? static_cast<std::int64_t>(time(nullptr)) : 0;
  h.tz = tz != nullptr ? tz : "UTC";
  h.synced = s_time_synced;
  h.has_wifi = wifi_current(h.wifi_ssid, sizeof(h.wifi_ssid), h.wifi_rssi, h.wifi_ip, sizeof(h.wifi_ip));
  if (g_loc_set) {
    h.has_location = true;
    h.lat = g_loc_lat_e6 / 1e6;
    h.lon = g_loc_lon_e6 / 1e6;
    core::SunTimes sun;
    if (sun_today(sun)) {
      h.has_sun = true;
      h.sun_rise = sun.rise;
      h.sun_set = sun.set;
    }
  }
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

// ---- a factory reset ------------------------------------------------------------------------
//
// Everything the hub's own settings live in, in the "keystone" NVS namespace. The Wi-Fi
// credentials stay (otherwise the hub would come up with no network at all and only its setup
// access point), and the co-processors keep their own partitions (their networks and images).

constexpr const char* kFactoryResetKeys[] = {
    "rules", "ent_names", "schedules", "timers", "scenes", "desired",
    kWebTokenKey, kWebPlainKey, "relay_url", "relay_sec", "location",
};

void factory_reset() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(kTag, "factory reset: cannot open the NVS namespace");
    return;
  }
  for (const char* key : kFactoryResetKeys) nvs_erase_key(h, key);
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGW(kTag, "factory reset: the hub's settings are erased");
}

// Runs the action the app asked for, once the reply has had its time to go out.
void hub_action_service(std::uint32_t now) {
  if (!g_hub_action.pending) return;
  if (static_cast<std::int32_t>(now - g_hub_action.at) < 0) return;
  const HubAction kind = g_hub_action.kind;
  g_hub_action.pending = false;

  switch (kind) {
    case HubAction::Reboot:
      ESP_LOGI(kTag, "app: rebooting the hub");
      esp_restart();
      break;
    case HubAction::FactoryReset:
      ESP_LOGW(kTag, "app: factory reset - erasing the hub's settings and rebooting");
      factory_reset();
      esp_restart();
      break;
    case HubAction::SetToken: {
      const char* token = g_hub_action.token;
      persist_webtoken(token);
      if (!appsrv::start(token, &app_hub_info, restore_webplain())) {
        ESP_LOGE(kTag, "app: the token was saved, but the app server did not start");
      }
      relay::set_token(token);
      if (!relay::running()) relay::start(token, &app_hub_info);
      std::memset(g_hub_action.token, 0, sizeof(g_hub_action.token));
      ESP_LOGI(kTag, "app: the app token was changed; the clients have to reconnect");
      break;
    }
    case HubAction::ForgetWifi:
      ESP_LOGW(kTag, "app: forgetting the saved Wi-Fi network");
      wifi_forget();
      break;
    case HubAction::RestartRelay: {
      // The URL and the secret are in NVS by now; the client picks them up there.
      char tok[core::app::kMaxToken + 1] = {};
      const bool have = restore_webtoken(tok, sizeof(tok));
      if (relay::running()) relay::stop();
      if (!relay::start(have ? tok : nullptr, &app_hub_info)) {
        ESP_LOGW(kTag, "app: the relay is configured but did not come up (it retries on the next boot)");
      }
      std::memset(tok, 0, sizeof(tok));
      ESP_LOGI(kTag, "app: the relay was reconfigured");
      break;
    }
    case HubAction::ClearRelay:
      relay::stop();
      relay::clear_config();
      ESP_LOGI(kTag, "app: the relay was removed");
      break;
    case HubAction::None:
      break;
  }
}

const char* app_mode_text() {
  switch (appsrv::mode()) {
    case appsrv::Mode::Tls: return "wss (TLS)";
    case appsrv::Mode::Plain: return "ws (PLAIN, development only)";
    case appsrv::Mode::Stopped: break;
  }
  return "stopped";
}

int cmd_relay(int argc, char** argv) {
  if (argc == 1) {
    static char buf[384];
    relay::describe(buf, sizeof(buf));
    printf("%s", buf);
    return 0;
  }
  if (std::strcmp(argv[1], "clear") == 0) {
    relay::stop();
    relay::clear_config();
    printf("relay credentials erased; tunnel stopped\n");
    return 0;
  }
  // relay <secret>          — default host
  // relay <host> <secret>   — custom host
  if (argc > 3) {
    printf("usage:\n"
           "  relay                          show status\n"
           "  relay <secret>                 connect to %s\n"
           "  relay <host> <secret>          connect to a custom relay\n"
           "  relay clear                    disconnect and erase credentials\n",
           relay::default_host());
    return 1;
  }
  const char* host = argc == 3 ? argv[1] : nullptr;
  const char* sec = argc == 3 ? argv[2] : argv[1];
  char url[256];
  const int n = relay::build_url(url, sizeof(url), host, nullptr);
  if (n < 0 || static_cast<std::size_t>(n) >= sizeof(url)) {
    printf("relay host too long\n");
    return 1;
  }
  if (!relay::configure(url, sec)) {
    printf("bad relay credentials\n");
    return 1;
  }
  char tok[core::app::kMaxToken + 1] = {};
  const bool have = restore_webtoken(tok, sizeof(tok));
  if (relay::running()) relay::stop();
  if (relay::start(have ? tok : nullptr, &app_hub_info)) {
    printf("relay configured and connecting to %s\n", url);
  } else {
    printf("relay configured but could not start (will retry on next boot)\n");
  }
  return 0;
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
           have ? "set" : "NOT set", app_mode_text(), appsrv::port(), static_cast<unsigned>(hub_client_count()));
    if (appsrv::tls_available()) printf("certificate SHA-256 (what apps pin): %s\n", appsrv::cert_fingerprint());
    return 0;
  }
  if (std::strcmp(argv[1], "clear") == 0) {
    persist_webtoken(nullptr);
    appsrv::stop();
    relay::set_token(nullptr);
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
  relay::set_token(argv[1]);
  if (!relay::running()) relay::start(argv[1], &app_hub_info);
  printf("token set; app server (re)started: %s on port %d — existing connections were closed\n",
         app_mode_text(), appsrv::port());
  if (appsrv::mode() == appsrv::Mode::Tls) printf("certificate SHA-256: %s\n", appsrv::cert_fingerprint());
  return 0;
}

// ---- first-time setup access point (core/setup_mode.hpp) ----------------------------------------
// With no network credentials, or after ten minutes without a connection, the hub opens `Keystone-XXXX`,
// protected by the setup code from its TLS identity. A phone that joined it can reach the app server and
// authenticate with that code instead of the token, which lets it scan for networks and send `setup.apply`
// (the home network plus the app's long-term token) and nothing more. The policy lives in core; this is the
// radio and server side.
core::SetupPolicy g_setup_policy;
std::atomic<bool> g_ap_up{false};
std::atomic<int> g_setup_force{0};  // from the console: 1 = open the access point, 2 = close it

struct PendingToken {
  bool pending = false;
  char token[core::app::kMaxToken + 1] = {};
  std::uint32_t at = 0;
} g_new_token;

// The code this firmware was built with, or null (an identity from before setup mode existed has none).
const char* setup_code() {
#ifdef KEYSTONE_HAVE_TLS
  if (kHubSetupCode != nullptr && core::setup_code_valid(kHubSetupCode)) return kHubSetupCode;
#endif
  return nullptr;
}

void setup_token_request(const char* token, std::uint32_t now) {
  std::snprintf(g_new_token.token, sizeof(g_new_token.token), "%s", token);
  persist_webtoken(token);  // saved now; the server picks it up when the reply has gone out
  g_new_token.at = now + 700;
  g_new_token.pending = true;
}

void setup_ap_set(bool up) {
  const char* code = setup_code();
  if (up) {
    std::uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ssid[24];
    core::setup_ap_ssid(mac, ssid, sizeof(ssid));
    wifi_config_t ap = {};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = static_cast<std::uint8_t>(std::strlen(ssid));
    std::strncpy(reinterpret_cast<char*>(ap.ap.password), code, sizeof(ap.ap.password) - 1);
    ap.ap.channel = 6;  // follows the station's channel while it is connected
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.pmf_cfg.required = false;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    appsrv::set_setup_code(code);
    if (!appsrv::running()) {
      char tok[core::app::kMaxToken + 1] = {};
      const bool have = restore_webtoken(tok, sizeof(tok));
      if (!appsrv::start(have ? tok : nullptr, &app_hub_info, restore_webplain())) ESP_LOGE(kTag, "setup: the app server did not start");
    }
    ESP_LOGW(kTag, "setup: access point %s is open (the code is on the label; `setup` shows it)", ssid);
  } else {
    esp_wifi_set_mode(WIFI_MODE_STA);
    appsrv::set_setup_code(nullptr);
    char tok[core::app::kMaxToken + 1] = {};
    if (!restore_webtoken(tok, sizeof(tok))) appsrv::stop();  // nothing but setup clients could have used it
    ESP_LOGI(kTag, "setup: access point closed");
  }
  g_ap_up = up;
  g_hub_dirty = true;
}

void setup_service(std::uint32_t now) {
  if (g_new_token.pending && static_cast<std::int32_t>(now - g_new_token.at) >= 0) {
    g_new_token.pending = false;
    if (!appsrv::start(g_new_token.token, &app_hub_info, restore_webplain())) ESP_LOGE(kTag, "setup: the app server did not restart");
    relay::set_token(g_new_token.token);
    if (!relay::running()) relay::start(g_new_token.token, &app_hub_info);
    std::memset(g_new_token.token, 0, sizeof(g_new_token.token));
  }
  const int f = g_setup_force.exchange(0);
  if (f == 1) g_setup_policy.force(true);
  else if (f == 2) g_setup_policy.force(false);
  if (setup_code() == nullptr) return;
  g_setup_policy.update(now, s_wifi_up, s_wifi_have_creds);
  const bool want = g_setup_policy.ap_wanted();
  if (want != g_ap_up) setup_ap_set(want);
}

// setup [on|off]: the first-time setup access point. Shows the state and the code (the console is physical access).
int cmd_setup(int argc, char** argv) {
  const char* code = setup_code();
  if (code == nullptr) {
    printf("this firmware has no setup code: make one with scripts/gen-hub-cert.sh --setup-code, then rebuild\n");
    return 1;
  }
  if (argc >= 2) {
    if (std::strcmp(argv[1], "on") == 0) g_setup_force = 1;
    else if (std::strcmp(argv[1], "off") == 0) g_setup_force = 2;
    else {
      printf("usage: setup [on|off]\n");
      return 1;
    }
    vTaskDelay(pdMS_TO_TICKS(300));  // the main loop picks it up
  }
  std::uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ssid[24];
  core::setup_ap_ssid(mac, ssid, sizeof(ssid));
  printf("setup access point %s: %s\n", ssid, g_ap_up ? "OPEN" : "closed");
  printf("setup code: %.4s-%.4s-%.4s  (it is the access point's password and what the app asks for)\n", code, code + 4, code + 8);
  printf("it opens by itself with no saved network, or after %u min without a connection\n",
         static_cast<unsigned>(core::SetupPolicy::kDownMs / 60000u));
  return 0;
}

// Human-readable reset reason for the boot banner: tells a planned reset, a crash and
// a watchdog apart when all we have is the log after the reboot.
const char* reset_reason_text(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software reset (esp_restart)";
    case ESP_RST_PANIC: return "panic (crash)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}

#ifdef KEYSTONE_HAVE_MATTER
// Minimal event callback — same shape as the vendor controller example
// (ChipDeviceEvent + intptr_t). We don't act on events yet; the hook exists so
// esp_matter::start() has something to call.
static void matter_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event,
                            intptr_t /*arg*/) {
  (void)event;
}

// Bring up the Matter stack. Called once from app_main() before the Wi-Fi
// driver is started.
//
// Only the CHIP *stack* comes up here. The controller/commissioner is created
// later, when the Wi-Fi link is up — see matter_controller_start().
//
// Fabric 1 / node 112233 are placeholder admin credentials (test DAC); the
// production path will mint per-hub credentials later, see docs/matter-backlog.md §6.
static void matter_bringup() {
  const std::size_t heap_before =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const std::size_t largest_before =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGI("matter", "heap before bringup: %u B internal, largest block %u B",
           static_cast<unsigned>(heap_before),
           static_cast<unsigned>(largest_before));

  esp_err_t err = esp_matter::start(&matter_event_cb);
  if (err != ESP_OK) {
    ESP_LOGE("matter", "esp_matter::start failed: %d", err);
    return;
  }
  ESP_LOGI("matter",
           "after esp_matter::start: heap %u B internal, largest block %u B, psram free %u B",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  // The task that runs the commissioning calls. It is created here, while the
  // internal heap is still one big block — the CHIP stack needs a large
  // contiguous chunk later on.
  g_matter_req_sem = xSemaphoreCreateBinary();
  if (g_matter_req_sem == nullptr) {
    ESP_LOGE("matter", "no memory for the commissioning queue");
  } else if (xTaskCreate(matter_worker_task, "matter_cmd",
                         kMatterTaskStackBytes / sizeof(StackType_t), nullptr, 5,
                         nullptr) != pdPASS) {
    ESP_LOGE("matter", "could not create the commissioning task");
    vSemaphoreDelete(g_matter_req_sem);
    g_matter_req_sem = nullptr;
  } else {
    ESP_LOGI("matter", "commissioning task up (%u B of stack)",
             static_cast<unsigned>(kMatterTaskStackBytes));
  }

  // The task that creates the controller. It waits for the Wi-Fi interface and
  // for a usable IPv6 address on it — see matter_controller_task.
  g_matter_ctrl_done = xSemaphoreCreateBinary();
  if (g_matter_ctrl_done == nullptr) {
    ESP_LOGE("matter", "no memory for the controller queue");
  } else if (xTaskCreate(matter_controller_task, "matter_ctrl",
                         kMatterCtrlTaskStackBytes / sizeof(StackType_t), nullptr,
                         5, nullptr) != pdPASS) {
    ESP_LOGE("matter", "could not create the controller task");
    vSemaphoreDelete(g_matter_ctrl_done);
    g_matter_ctrl_done = nullptr;
  }

  // The queue the co-processor's Thread dataset arrives on. The brain has no
  // 802.15.4 radio, so the dataset has to come from the chip that has one.
  g_thread_dataset_sem = xSemaphoreCreateBinary();
  if (g_thread_dataset_sem == nullptr) {
    ESP_LOGE("matter", "no memory for the Thread dataset queue");
  }

  const std::size_t heap_after =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGI("matter",
           "stack up: heap %u B (-%d B), largest block %u B, psram free %u B",
           static_cast<unsigned>(heap_after),
           static_cast<int>(heap_before) - static_cast<int>(heap_after),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

// Create the CHIP controller/commissioner.
//
// This is deliberately NOT done at the very beginning of the boot: it happens
// when the Wi-Fi interface comes up and has a usable IPv6 link-local address
// (matter_controller_task takes care of that) — but it happens BEFORE the
// hub's own tables are allocated, because the controller wants one contiguous
// block of internal RAM.
//
// The reason for the address is CHIP's own mDNS (DNS-SD) server. It binds one
// UDP endpoint per interface that is up AND has an IPv6 address, and it binds
// them ONCE — when the controller is created. Created before Wi-Fi, it binds
// nothing at all, and the operational discovery of the accessory then fails
// with CHIP_ERROR_NO_ENDPOINT (0x46): "Device connection failed. Error 46".
//
// The commissioning itself (BLE, PASE, the crypto) does not care about the
// network, but the last step — finding the accessory's IPv6 address over
// DNS-SD — does, and that step is the one that has to work.
//
// UDP listen port 5580 is Matter's default secure-session port.
static void matter_controller_start() {
  if (g_matter_controller_ready) {
    return;
  }
  matter_set_status("creating the controller");
  const std::size_t heap_before =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &client = esp_matter::controller::matter_controller_client::get_instance();
    const esp_err_t err = client.init(/*node_id=*/112233, /*fabric_id=*/1, /*udp_port=*/5580);
    if (err != ESP_OK) {
      ESP_LOGE("matter", "client.init failed: %d", err);
      matter_set_status("client.init failed");
      return;
    }
    // The trust store for the accessory's attestation chain — esp-matter reads
    // it inside setup_commissioner(), so it has to be in place before that.
    matter_install_paa_trust_store();
    const esp_err_t comm_err = client.setup_commissioner();
    if (comm_err != ESP_OK) {
      ESP_LOGE("matter", "setup_commissioner failed: %d", comm_err);
      matter_set_status("setup_commissioner failed");
      return;
    }
  }
  g_matter_controller_ready = true;
  matter_set_status("up");
  ESP_LOGI("matter",
           "controller up (node 112233, fabric 1, UDP 5580): heap %u B (-%d B), "
           "largest block %u B",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<int>(heap_before) -
               static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

// The task that creates the controller.
//
// It waits for the Wi-Fi interface to be up and to have a *usable* IPv6
// link-local address on it: a fresh lwIP address is tentative until the
// duplicate address detection has finished, and CHIP's mDNS skips a tentative
// address — it would bind no endpoint again, which is exactly the failure
// this whole dance is about ("Device connection failed. Error 46").
//
// It runs on a task of its own (the CHIP init is a deep call chain, and
// app_main's stack is small) and it is started right after the Matter stack —
// the Wi-Fi driver, and with it the interface, comes up a moment later. The
// hub's own tables are allocated only after this task is done, because the
// controller wants one contiguous block of internal RAM and the tables would
// fragment it away.
static void matter_controller_task(void* arg) {
  (void)arg;
  matter_set_status("waiting for IPv6");
  if (!wait_wifi_ipv6_linklocal(kMatterIpv6WaitMs)) {
    ESP_LOGE("matter", "no usable IPv6 on the Wi-Fi interface; the commissioner stays down");
    matter_set_status("no usable IPv6");
  } else {
    matter_controller_start();
  }
  if (g_matter_ctrl_done != nullptr) xSemaphoreGive(g_matter_ctrl_done);
  vTaskDelete(nullptr);
}
#endif  // KEYSTONE_HAVE_MATTER

}  // namespace

extern "C" void app_main(void) {
#ifdef KEYSTONE_HAVE_MATTER
  ESP_LOGI("matter", "app_main entry: heap %u B internal, largest block %u B",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
  status_led::init();

  // NVS + netif + default event loop + default STA/AP netifs (cheap).
  wifi_bootstrap();
#ifdef KEYSTONE_HAVE_MATTER
  ESP_LOGI("matter", "after bootstrap: heap %u B internal, largest block %u B",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif

#ifdef KEYSTONE_HAVE_MATTER
  // experiment/commissioner-on-s3: bring up Matter + BLE BEFORE the Wi-Fi
  // driver init, so the BLE controller gets to take its ~30 KB contiguous
  // internal-RAM allocation from the fattest pool (the Wi-Fi driver's own
  // ~20 KB had been fragmenting it into chunks too small for BLE to fit).
  // Step 1a of docs/matter-backlog.md §3's R3 path; the hardware log from
  // the previous commit (BLE_INIT: Malloc failed) is what justifies this
  // reordering.
  matter_bringup();
#endif

  // Heavy Wi-Fi driver init + config + start.
  wifi_start_driver();

#ifdef KEYSTONE_HAVE_MATTER
  // The CHIP controller is created HERE — between the Matter stack and the
  // hub's own tables, and that is the whole point of the ordering:
  //
  //   * it wants one contiguous block of internal RAM (the CHIP stack's
  //     bring-up asks for tens of KB at once), and the hub's own tables —
  //     the UART links, the NVS buffers, the app server — would fragment the
  //     heap away before it runs. That is exactly how the controller died
  //     with "client.init failed" when it was created at the end of the boot;
  //   * and it binds CHIP's DNS-SD endpoints on the interfaces that have an
  //     IPv6 address, so the Wi-Fi interface has to be up with a link-local
  //     address first — the Wi-Fi driver above is what brings it up.
  //
  // The task does the work (its own stack: the CHIP init is a deep call
  // chain); app_main waits for it, so the heap is still one big block when it
  // runs.
  if (g_matter_ctrl_done != nullptr) {
    xSemaphoreTake(g_matter_ctrl_done, portMAX_DELAY);
  }
#endif

  // Event history first: everything below may emit.
  g_ev_mu = xSemaphoreCreateMutex();
  g_ev_out.add(&g_ev_ring);
  {
    core::app::ResetWhy why = core::app::ResetWhy::Other;
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON: why = core::app::ResetWhy::PowerOn; break;
      case ESP_RST_SW: case ESP_RST_EXT: why = core::app::ResetWhy::Software; break;
      case ESP_RST_PANIC: why = core::app::ResetWhy::Panic; break;
      case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: why = core::app::ResetWhy::Watchdog; break;
      case ESP_RST_BROWNOUT: why = core::app::ResetWhy::Brownout; break;
      case ESP_RST_UNKNOWN: why = core::app::ResetWhy::Unknown; break;
      default: break;
    }
    ev_plain(core::EventKind::Boot, static_cast<std::uint8_t>(why), 0);
  }

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

  // experiment/commissioner-on-s3: tagged EXT_RAM_BSS_ATTR so all the brain's
  // core tables (registry, bus router, radio coord, state machine, dispatcher,
  // chip monitor, rule engine, OTA jobs, reconciler) live in PSRAM instead of
  // internal-DRAM BSS. That frees ~15 KB of internal RAM for the Matter
  // controller to use at runtime. These are plain C++ aggregates with no
  // vtables; their constructors run on first entry to app_main (long after
  // PSRAM is initialised by the bootloader).
  static EXT_RAM_BSS_ATTR core::UartTransport bus_zb(link_zb);
  static EXT_RAM_BSS_ATTR core::UartTransport bus_th(link_th);

  static EXT_RAM_BSS_ATTR Registry reg;
  restore_names(reg.names());
  static EXT_RAM_BSS_ATTR ChipRouter bus;           // presents itself as one Bus
  // publish(chip=0x01) -> UART1, publish(chip=0x02) -> UART2. A refused bind
  // means the brain is deaf on that link — say so.
  if (!bus.bind(kZigbeeChipId, bus_zb)) {
    ESP_LOGE(kTag, "no room for the Zigbee link");
  }
  if (!bus.bind(kThreadChipId, bus_th)) {
    ESP_LOGE(kTag, "no room for the Thread link");
  }
  g_bus = &bus;

  static EXT_RAM_BSS_ATTR core::RadioCoordinator<Registry> coord(bus, reg);
  static EXT_RAM_BSS_ATTR core::StateMachine<Registry> sm(bus, reg);
  static EXT_RAM_BSS_ATTR Dispatcher disp(bus, reg);
  static EXT_RAM_BSS_ATTR core::ChipMonitor monitor(bus, &now_ms, nullptr);
  g_monitor = &monitor;
  monitor.on_event(&on_chip_event, nullptr);
  disp.set_alive_filter(&core::ChipMonitor::alive_thunk, &monitor);
  static EXT_RAM_BSS_ATTR Rules rules(disp, &now_ms, nullptr);

  static EXT_RAM_BSS_ATTR fw::OtaJob ota_job(bus);
  g_ota = &ota_job;
  static EXT_RAM_BSS_ATTR fw::DeviceOtaJob dev_ota_job(bus);
  g_dev_ota = &dev_ota_job;
  dev_ota_job.on_phase(&on_dev_ota_phase, nullptr);
  static EXT_RAM_BSS_ATTR core::OtaVerifier verifier(bus);
  g_verifier = &verifier;
  verifier.on_done(&on_verify_done, nullptr);
  ota_job.on_accepted(&on_ota_accepted, nullptr);

  disp.on_result(&on_cmd_result, nullptr);
  // Check the result: a refused subscription is a feature that silently does
  // nothing (see ChipRouter::subscribe).
  if (!bus.subscribe(&on_bus_traffic, nullptr)) {
    ESP_LOGE(kTag, "no room for the per-chip rx counter");
  }
  if (!bus.subscribe(&on_bus_forward, &bus)) {
    ESP_LOGE(kTag, "no room for the cross-chip forwarder — Matter <-> Zigbee and the "
                   "Thread dataset will not work");
  }
  ESP_LOGI(kTag, "bus fan-out: %d subscribers (room for %d)",
           bus.subscriber_count(), ChipRouter::kMaxSubs);

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
  static EXT_RAM_BSS_ATTR Reconciler reconciler(disp, &now_ms, nullptr);
  g_rec = &reconciler;
  restore_desired(reconciler);
  reconciler.on_event(&on_reconcile, nullptr);
  disp.on_send(&rec_note_command, nullptr);
  sm.on_report(&rec_on_report, nullptr);

  sm.on_change(&Rules::on_state_change, &rules);
  sm.on_change(&on_state_change, nullptr);

  link_zb.start();
  link_th.start();

  // Persistent timers table (survives reboots via NVS). PSRAM-backed storage
  // (EXT_RAM_BSS_ATTR) to keep internal-DRAM BSS small for Matter+BLE on S3.
  static EXT_RAM_BSS_ATTR core::TimerTable<8> timer_table;
  time_t boot_now = 0;
  if (s_time_synced) time(&boot_now);
  restore_timers(timer_table, now_ms(nullptr), static_cast<std::uint64_t>(boot_now));

  // Recurring time-of-day actions (`at HH:MM …`); they only run once the clock is
  // synced, in local time (see `tz`).
  static EXT_RAM_BSS_ATTR Schedules schedule_table;
  restore_schedules(schedule_table);

  // Scenes: named groups of actions (`scene`), applied from the console or the app.
  static EXT_RAM_BSS_ATTR Scenes scene_store;
  restore_scenes(scene_store);
  g_scenes = &scene_store;
  g_scene_q = xQueueCreate(8, sizeof(std::uint8_t));
  rules.set_action_hook(&rule_action_hook, nullptr);
  rules.set_fire_hook(&rule_fire_hook, nullptr);
  rules.set_sun(&sun_cb, nullptr);
  restore_location();

  // App protocol: live events from the UART RX task, CMD_ACK bookkeeping, and the
  // server itself (only when a token has been set with `webtoken`).
  g_app_ev_q = xQueueCreate(32, sizeof(AppEvent));
  g_dev_ota_q = xQueueCreate(16, sizeof(DevOtaEvent));
  g_matter_q = xQueueCreate(8, sizeof(MatterUpdate));
  g_app_pending_mu = xSemaphoreCreateMutex();
  {
    char tok[core::app::kMaxToken + 1] = {};
    bool have = restore_webtoken(tok, sizeof(tok));
    if (have) {
      if (!appsrv::start(tok, &app_hub_info, restore_webplain())) ESP_LOGE(kTag, "app server failed to start");
    } else {
      ESP_LOGI(kTag, "app server off: no token (set one with `webtoken <token>`)");
    }
    relay::start(have ? tok : nullptr, &app_hub_info);
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
  // Why did we come up? A watchdog or a panic shows up here even when the log around it
  // is lost (the USB console is the only place we can read).
  ESP_LOGI(kTag, "reset reason: %s", reset_reason_text(esp_reset_reason()));

  bool announced_zb = false;
  bool announced_th = false;

  // Self-heal QUERY: if a bound chip hasn't declared yet (e.g. brain rebooted
  // after the chip, so we missed its HELLO), poke it every so often. On a QUERY
  // the co-processor re-sends DECLARE + DECLARE_END; a ready chip is skipped.
  const std::uint32_t kSelfHealMs = 5000;
  std::uint32_t last_query_ms = 0;
  std::size_t s_bad_zb = bus_zb.bad_frames();
  std::size_t s_bad_th = bus_th.bad_frames();
  const std::uint8_t kBoundChips[] = {kZigbeeChipId, kThreadChipId};
  while (true) {
    const std::uint32_t t = now_ms(nullptr);
    // The frame layer drops a block without a word (bad COBS, bad CRC, a type we
    // do not know). That silence is why a lost frame is hard to see: count it.
    {
      const std::size_t bad_zb = bus_zb.bad_frames();
      const std::size_t bad_th = bus_th.bad_frames();
      if (bad_zb != s_bad_zb || bad_th != s_bad_th) {
        ESP_LOGW(kTag, "link dropped frames: zigbee %u, thread %u (bad COBS/CRC or unknown type)",
                 static_cast<unsigned>(bad_zb - s_bad_zb),
                 static_cast<unsigned>(bad_th - s_bad_th));
        s_bad_zb = bad_zb;
        s_bad_th = bad_th;
      }
    }
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

    // An unpaired device is gone from the registry and the Zigbee chip has finished declaring: drop its Matter
    // mirror and the hub's memory of what it wanted from it. A device still in the registry (the leave failed)
    // is left alone and forgotten after kUnpairWatchMs.
    for (auto& u : g_unpair) {
      if (!u.used) continue;
      if (reg.find(u.entity) != nullptr) {  // not gone (yet)
        if (static_cast<std::uint32_t>(t - u.since_ms) > kUnpairWatchMs) {
          ESP_LOGW(kTag, "unpair 0x%04x: the device is still declared after %u s, giving up on its cleanup",
                   static_cast<unsigned>(u.entity), static_cast<unsigned>(kUnpairWatchMs / 1000));
          u.used = false;
        }
        continue;
      }
      // Gone from the registry: wait for the Zigbee chip to finish declaring (so it is not just mid-resync) and
      // for the Matter chip to be there to hear it (it keeps waiting while that chip is down; lost on a brain reboot).
      if (!coord.chip_ready(kZigbeeChipId) || !coord.chip_ready(kThreadChipId)) continue;
      hubproto::Header g{};
      g.version = hubproto::kProtocolVersion;
      g.type = hubproto::MsgType::Undeclare;
      g.chip_id = kThreadChipId;
      g.entity_id = u.entity;
      bus.publish(g, nullptr, 0);
      xSemaphoreTakeRecursive(g_rec_mu, portMAX_DELAY);
      if (reconciler.desired().clear_entity(u.entity) > 0) reconciler.note_dirty();
      xSemaphoreGiveRecursive(g_rec_mu);
      ESP_LOGI(kTag, "unpair 0x%04x done: told the Matter chip to drop its bridged endpoint",
               static_cast<unsigned>(u.entity));
      u.used = false;
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
        if (w.ok()) hub_broadcast(b, w.size());
      }
      g_hub_dirty = true;  // today's sun becomes known with the clock
    }
    if (g_hub_dirty.exchange(false) && hub_client_count() > 0) {
      char b[320];
      core::JsonWriter w(b, sizeof(b));
      core::app::HubInfo info{};
      app_hub_info(info);
      core::app::encode_hub(w, info);
      if (w.ok()) hub_broadcast(b, w.size());
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
      core::SunTimes sun;
      const bool have_sun = sun_today(sun);
      const std::size_t n = schedule_table.poll(
          local,
          [](void* ctx, std::size_t slot, const Schedules::Schedule& s) {
            auto* c = static_cast<TimerFireContext*>(ctx);
            run_action(*c->disp, *c->reg, c->now_ms, "schedule", slot, s.entity_id,
                       s.attr_id, s.value);
          },
          &fc, core::ScheduleTable<8>::kDefaultMaxLateMin, have_sun ? &sun : nullptr);
      // Saved after a fire (keeps last_fired_day across reboots, drops a finished one-off) and after an expired one-off was dropped.
      if (schedule_table.take_pruned() || n > 0) persist_schedules(schedule_table);
    }

    // Scenes that rules asked for (queued by the rule hook in the UART task).
    {
      std::uint8_t scene_id = 0;
      for (int n = 0; n < 4 && xQueueReceive(g_scene_q, &scene_id, 0) == pdTRUE; ++n) {
        run_scene(disp, reg, scene_id, t, "rule");
      }
    }

    // ---- App protocol: live state, structural resync, chip health, inbound commands ----
    {
      AppEvent ev;
      for (int n = 0; n < 16 && xQueueReceive(g_app_ev_q, &ev, 0) == pdTRUE; ++n) {
        const auto* e = reg.find(ev.entity);
        if (e == nullptr || e->device_type == hubproto::DeviceType::Chip) continue;
        if (hub_client_count() == 0) continue;
        char b[160];
        core::JsonWriter w(b, sizeof(b));
        core::app::encode_state(w, ev.entity, ev.attr, ev.value);
        if (w.ok()) hub_broadcast(b, w.size());
      }
      // The firmware session of a Zigbee device (the chip reports the phase).
      DevOtaEvent de;
      while (xQueueReceive(g_dev_ota_q, &de, 0) == pdTRUE) {
        ESP_LOGI(kTag, "device 0x%04x firmware: %s (%s, %u%%)",
                 static_cast<unsigned>(de.entity), dev_ota_phase_name(de.phase),
                 dev_ota_status_name(de.status), static_cast<unsigned>(de.percent));
        if (hub_client_count() == 0) continue;
        char b[160];
        core::JsonWriter w(b, sizeof(b));
        core::app::encode_device_ota(w, de.entity, dev_ota_phase_name(de.phase),
                                     dev_ota_status_name(de.status), de.percent);
        if (w.ok()) hub_broadcast(b, w.size());
      }
      // What the Matter chip said about the attempt to add a device — and what the app hears.
      {
        MatterUpdate mu;
        bool moved = false;
        while (g_matter_q != nullptr && xQueueReceive(g_matter_q, &mu, 0) == pdTRUE) {
          g_matter.state = mu.state;
          g_matter.code = mu.code;
          g_matter.entity = mu.entity;
          if (mu.state == hubproto::MatterState::Done || mu.state == hubproto::MatterState::Failed) {
            g_matter.deadline_ms = 0;  // it is over, one way or another
          }
          moved = true;
        }
        // The chip went quiet: the attempt is given up on, so the app is not left waiting.
        if (!moved && g_matter.deadline_ms != 0 &&
            static_cast<std::int32_t>(t - g_matter.deadline_ms) >= 0) {
          const auto* ti = g_monitor != nullptr ? g_monitor->info(kThreadChipId) : nullptr;
          const bool chip_alive = ti != nullptr && !ti->silent;
          if (chip_alive) g_matter_cannot = true;
          g_matter.state = hubproto::MatterState::Failed;
          g_matter.code = chip_alive ? hubproto::MatterCode::Unsupported : hubproto::MatterCode::Timeout;
          g_matter.entity = 0;
          g_matter.deadline_ms = 0;
          moved = true;
          ESP_LOGW(kTag, "matter: the chip did not answer (chip alive: %s)",
                   chip_alive ? "yes" : "no");
        }
        if (moved) {
          ESP_LOGI(kTag, "matter: %s (%s), entity 0x%04x", core::app::matter_state_name(g_matter.state),
                   core::app::matter_code_name(g_matter.code), static_cast<unsigned>(g_matter.entity));
          if (hub_client_count() > 0) {
            char b[192];
            core::JsonWriter w(b, sizeof(b));
            core::app::encode_matter_event(w, g_matter.state, g_matter.entity, g_matter.code,
                                            core::app::matter_message(g_matter.state, g_matter.code));
            if (w.ok()) hub_broadcast(b, w.size());
          }
        }
      }
      history_push(scene_store, t);
      mdns_sync();
      wifi_service(t);
      setup_service(t);
      if (appsrv::running()) {
        appsrv::tick(t);
        if (hub_client_count() > 0 && (t - app_last_check_ms >= 1000 || g_app_force_check.exchange(false))) {
          app_last_check_ms = t;
          app_expire_pending(t);
          AppSource src(reg, rules, schedule_table, timer_table, scene_store, coord, t);
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
              if (w.ok()) hub_broadcast(b, w.size());
              std::snprintf(cs.status, sizeof(cs.status), "%s", cv.status);
              cs.boot_id = cv.boot_id;
              cs.reboots = cv.reboots;
              cs.t_ms = t;
              cs.any = true;
            }
          }
        }
        auto drain_inbound = [&](appsrv::Inbound& in) {
          if (in.kind == appsrv::Inbound::Kind::Connected && in.setup_only) {
            // A setup client gets the welcome and nothing about the home.
          } else if (in.kind == appsrv::Inbound::Kind::Connected) {
            AppSource src(reg, rules, schedule_table, timer_table, scene_store, coord, t);
            app_last_fp = src.fingerprint();
            app_send_snapshot(in.fd, src);
          } else if (in.kind == appsrv::Inbound::Kind::Req) {
            app_handle_req(in, bus, disp, reg, rules, schedule_table, timer_table, scene_store, t);
            std::free(in.text);
          } else {
            app_handle_cmd(in, reg, disp, t);
          }
        };
        appsrv::Inbound in;
        while (appsrv::next(in)) drain_inbound(in);
        while (relay::next(in)) drain_inbound(in);
        relay::tick(t);
      }
    }

    // A reboot, a factory reset, a new app token or a forgotten network: the app asked for it,
    // the reply has had its time to go out, now do it.
    hub_action_service(t);

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
            CmdSrcScope src(core::CmdSource::Console);
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
          std::uint8_t tattr = kOnOffAttr;
          if (r->idx == 2) {  // a scene
            const auto* sc = scene_store.find(r->target);
            if (sc == nullptr) {
              std::snprintf(r->out, r->out_cap, "%s: no such scene\n", r->target);
              break;
            }
            id = core::kSceneTarget;
            tattr = sc->id;
          } else if (!reg.resolve(r->target, id) || reg.find(id) == nullptr) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          time_t cur_epoch = 0;
          if (s_time_synced) time(&cur_epoch);
          const hubproto::Value action =
              r->idx == 2 ? hubproto::Value::make_bool(true)
              : r->idx == 1 ? hubproto::Value::make_u8(2) : hubproto::Value::make_bool(r->val);
          int slot = timer_table.schedule(id, tattr, action,
                                          r->delay_ms, t,
                                          static_cast<std::uint64_t>(cur_epoch));
          if (slot < 0) {
            std::snprintf(r->out, r->out_cap, "no free timer slot\n");
          } else {
            persist_timers(timer_table, t);
            const char* name = reg.name_of(id);
            if (id == core::kSceneTarget) {
              std::snprintf(r->out, r->out_cap, "timer #%d: scene %s in %us\n", slot, r->target,
                            static_cast<unsigned>(r->delay_ms / 1000u));
            } else if (name != nullptr) {
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
            if (tm->entity_id == core::kSceneTarget) {
              const char* sn = scene_name_for(tm->entity_id, tm->attr_id);
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "#%u: scene %s in %us\n", static_cast<unsigned>(i),
                  sn != nullptr ? sn : "?", static_cast<unsigned>(left)));
            } else if (name != nullptr) {
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
          std::uint8_t sattr = kOnOffAttr;
          if (r->idx == 2) {  // a scene
            const auto* sc = scene_store.find(r->target);
            if (sc == nullptr) {
              std::snprintf(r->out, r->out_cap, "%s: no such scene\n", r->target);
              break;
            }
            id = core::kSceneTarget;
            sattr = sc->id;
          } else if (!reg.resolve(r->target, id) || reg.find(id) == nullptr) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          const hubproto::Value action =
              r->idx == 2 ? hubproto::Value::make_bool(true)
              : r->idx == 1 ? hubproto::Value::make_u8(2) : hubproto::Value::make_bool(r->val);
          const auto point = static_cast<std::uint16_t>(r->delay_ms);
          const int slot = schedule_table.add_point(id, sattr, action, point, static_cast<std::uint8_t>(r->id), r->argc);
          if (slot < 0) {
            std::snprintf(r->out, r->out_cap, "no free schedule slot\n");
            break;
          }
          persist_schedules(schedule_table);
          char dbuf[40], tbuf[24];
          core::format_time_point(point, tbuf, sizeof(tbuf));
          if (r->argc != 0) {
            char d[16];
            core::format_date(r->argc, d, sizeof(d));
            std::snprintf(dbuf, sizeof(dbuf), "on %s", d);
          } else {
            days_text(static_cast<std::uint8_t>(r->id), dbuf, sizeof(dbuf));
          }
          const char* hint = s_time_synced ? "" : " (clock not synced yet — paused until it is)";
          if (core::is_solar_point(point) && !g_loc_set) hint = " (no location yet — set one with `loc <lat> <lon>`; it will not fire until then)";
          if (id == core::kSceneTarget) {
            std::snprintf(r->out, r->out_cap, "schedule #%d: %s %s scene %s%s\n", slot, tbuf, dbuf, r->target, hint);
            break;
          }
          std::snprintf(r->out, r->out_cap, "schedule #%d: %s %s 0x%04x -> %s%s\n", slot, tbuf, dbuf,
                        static_cast<unsigned>(id), timer_action_text(action), hint);
          break;
        }
        case REQ_SCHEDULES: {
          std::size_t off = 0;
          bool any = false;
          for (std::size_t i = 0; i < 8 && off < r->out_cap; ++i) {
            const auto* s = schedule_table.get(i);
            if (s == nullptr) continue;
            any = true;
            char dbuf[40], tbuf[24];
            core::format_time_point(s->minute_of_day, tbuf, sizeof(tbuf));
            if (s->entity_id == core::kSceneTarget) {
              const char* sn = scene_name_for(s->entity_id, s->attr_id);
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "#%u: %s %s  scene %s\n", static_cast<unsigned>(i), tbuf,
                  schedule_when(*s, dbuf, sizeof(dbuf)), sn != nullptr ? sn : "?"));
              continue;
            }
            const char* name = reg.name_of(s->entity_id);
            off += static_cast<std::size_t>(std::snprintf(
                r->out + off, r->out_cap - off, "#%u: %s %s  0x%04x%s%s%s -> %s\n",
                static_cast<unsigned>(i), tbuf,
                schedule_when(*s, dbuf, sizeof(dbuf)),
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
            // What THIS side dropped: the frame layer is silent about it, so it is
            // counted here. A frame the chip sent and we never saw is here.
            const std::size_t dropped =
                id == kThreadChipId ? bus_th.bad_frames() : bus_zb.bad_frames();
            if (reg.get_value(id, hubproto::chip_attr::kCrcErrors, v)) {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "crc_errors %u, dropped %u\n",
                  static_cast<unsigned>(v.as_u16()), static_cast<unsigned>(dropped)));
            } else {
              off += static_cast<std::size_t>(std::snprintf(
                  r->out + off, r->out_cap - off, "dropped %u\n",
                  static_cast<unsigned>(dropped)));
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
          send_chip_meta(bus, kZigbeeChipId, core::app::kMetaPair, sec);
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
          send_chip_meta(bus, e->chip_id, r->kind == REQ_UNPAIR ? core::app::kMetaUnpair : core::app::kMetaIdentify, id);
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
        case REQ_DEV_OTA: {
          // The firmware of a Zigbee device: the hub fetches the .ota file and
          // pushes it to the coordinator, which serves it to the device.
          std::uint16_t id = 0;
          if (!reg.resolve(r->target, id)) {
            std::snprintf(r->out, r->out_cap, "%s: not found\n", r->target);
            break;
          }
          const auto* e = reg.find(id);
          if (e == nullptr) {
            std::snprintf(r->out, r->out_cap, "0x%04x: not found\n", static_cast<unsigned>(id));
            break;
          }
          if (e->chip_id != kZigbeeChipId) {
            std::snprintf(r->out, r->out_cap,
                          "0x%04x is not a Zigbee device (only Zigbee devices can be updated)\n",
                          static_cast<unsigned>(id));
            break;
          }
          char err[96];
          if (g_dev_ota == nullptr ||
              !g_dev_ota->start(e->chip_id, id, r->url, r->sha, err, sizeof(err))) {
            std::snprintf(r->out, r->out_cap, "ota dev: %s\n",
                          g_dev_ota == nullptr ? "not ready" : err);
            break;
          }
          const char* name = reg.name_of(id);
          if (name != nullptr) {
            std::snprintf(r->out, r->out_cap,
                          "ota dev: %s 0x%04x started; watch `ota dev status`\n", name,
                          static_cast<unsigned>(id));
          } else {
            std::snprintf(r->out, r->out_cap,
                          "ota dev: 0x%04x started; watch `ota dev status`\n",
                          static_cast<unsigned>(id));
          }
          break;
        }
        case REQ_WIFI_JOIN: {
          wifi_request(r->ssid, r->pass, t);
          std::snprintf(r->out, r->out_cap,
                        "trying %s: kept only if the hub gets an address within %u s, otherwise it goes back to the old network\n",
                        r->ssid, static_cast<unsigned>(kWifiTrialMs / 1000));
          break;
        }
        case REQ_WIFI_STATUS: {
          char ssid[core::kMaxSsidLen + 1], ip[16];
          int rssi = 0;
          if (wifi_current(ssid, sizeof(ssid), rssi, ip, sizeof(ip))) {
            std::snprintf(r->out, r->out_cap, "connected to %s, rssi=%d, ip=%s%s\n", ssid, rssi, ip,
                          g_wifi_trial.active() ? " (trial: not saved yet)" : "");
          } else {
            std::snprintf(r->out, r->out_cap, "not connected%s%s\n", s_wifi_have_creds ? " (retrying with back-off), configured: " : " (no credentials)",
                          s_wifi_have_creds ? s_wifi_active.ssid : "");
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
        case REQ_INJECT:
          handle_inject(bus, r);
          break;
        case REQ_SCENE: {
          const int argc = r->argc;
          char** argv = r->argv;
          auto say = [&](const char* fmt, auto... a) { std::snprintf(r->out, r->out_cap, fmt, a...); };
          if (argc == 1) {  // list
            std::size_t off = 0;
            for (std::size_t i = 0; i < 8 && off < r->out_cap; ++i) {
              const auto* sc = scene_store.get(i);
              if (sc == nullptr) continue;
              off += static_cast<std::size_t>(std::snprintf(r->out + off, r->out_cap - off, "%s:", sc->name));
              for (std::size_t k = 0; k < sc->count && off < r->out_cap; ++k) {
                const auto& a = sc->actions[k];
                const char* en = reg.name_of(a.entity_id);
                char vbuf[16];
                fmt_value(vbuf, sizeof(vbuf), a.value.type, a.value);
                if (en != nullptr) {
                  off += static_cast<std::size_t>(std::snprintf(r->out + off, r->out_cap - off, " %s", en));
                } else {
                  off += static_cast<std::size_t>(std::snprintf(r->out + off, r->out_cap - off, " 0x%04x",
                                                                static_cast<unsigned>(a.entity_id)));
                }
                off += static_cast<std::size_t>(std::snprintf(r->out + off, r->out_cap - off, ".0x%02x=%s",
                                                              static_cast<unsigned>(a.attr_id), vbuf));
              }
              if (off < r->out_cap) off += static_cast<std::size_t>(std::snprintf(r->out + off, r->out_cap - off, "\n"));
            }
            if (off == 0) say("no scenes\n");
          } else if (std::strcmp(argv[1], "del") == 0) {
            if (argc != 3) say("usage: scene del <name>\n");
            else {
              const auto* sc = scene_store.find(argv[2]);
              bool used = false;
              if (sc != nullptr) {
                for (std::size_t i = 0; i < 8; ++i) {
                  const auto* sd = schedule_table.get(i);
                  const auto* tm = timer_table.get(i);
                  used = used || (sd != nullptr && sd->entity_id == core::kSceneTarget && sd->attr_id == sc->id) ||
                         (tm != nullptr && tm->entity_id == core::kSceneTarget && tm->attr_id == sc->id);
                }
                for (std::size_t i = 0; i < rules.rule_count(); ++i) {
                  const auto* ru = rules.rule_at(i);
                  used = used || (ru->action_entity == core::kSceneTarget && ru->action_attr == sc->id);
                }
              }
              if (sc == nullptr) say("%s: no such scene\n", argv[2]);
              else if (used) say("%s is used by a schedule, timer or rule; remove that first\n", argv[2]);
              else { scene_store.remove(argv[2]); persist_scenes(scene_store); say("deleted scene %s\n", argv[2]); }
            }
          } else if (std::strcmp(argv[1], "add") == 0) {
            const int triples = (argc - 3) / 3;
            if (argc < 6 || (argc - 3) % 3 != 0 || triples > 8) {
              say("usage: scene add <name> <id_or_name> <attr> <value> [<id_or_name> <attr> <value> ...]\n");
              break;
            }
            Scenes::Action acts[8];
            bool good = true;
            for (int k = 0; k < triples && good; ++k) {
              const char* ent = argv[3 + 3 * k];
              std::uint16_t id = 0;
              std::uint8_t at = 0;
              const core::AttrState* st = nullptr;
              if (!reg.resolve(ent, id) || reg.find(id) == nullptr ||
                  reg.find(id)->device_type == hubproto::DeviceType::Chip) {
                say("%s: not a device\n", ent);
                good = false;
              } else if (!parse_attr(argv[4 + 3 * k], at)) {
                say("%s: unknown attribute\n", argv[4 + 3 * k]);
                good = false;
              } else {
                const auto* e = reg.find(id);
                for (std::uint8_t i = 0; i < e->attr_count; ++i) if (e->attrs[i].attr_id == at) st = &e->attrs[i];
                if (st == nullptr) { say("%s has no attribute %s\n", ent, argv[4 + 3 * k]); good = false; }
                else if ((st->flags & hubproto::kFlagWritable) == 0) { say("%s.%s is read-only\n", ent, argv[4 + 3 * k]); good = false; }
                else if (!parse_value(argv[5 + 3 * k], st->value_type, acts[k].value)) { say("%s: bad value for %s\n", argv[5 + 3 * k], argv[4 + 3 * k]); good = false; }
                else {
                  acts[k].entity_id = id;
                  acts[k].attr_id = at;
                  for (int j = 0; j < k; ++j) {
                    if (acts[j].entity_id == id && acts[j].attr_id == at) { say("%s.%s given twice\n", ent, argv[4 + 3 * k]); good = false; }
                  }
                }
              }
            }
            if (good) {
              if (scene_store.put(argv[2], acts, static_cast<std::size_t>(triples)) < 0) {
                say("cannot store scene (bad name %s, or the table is full)\n", argv[2]);
              } else {
                persist_scenes(scene_store);
                say("scene %s stored (%d action%s)\n", argv[2], triples, triples == 1 ? "" : "s");
              }
            }
          } else if (argc == 2) {  // apply
            const auto res = core::app::apply_scene(
                reg, scene_store, argv[1], [&](std::uint16_t e, std::uint8_t a, const hubproto::Value& v) {
                  CmdSrcScope src(core::CmdSource::Scene);
                  std::uint16_t seq = 0;
                  return disp.send(e, a, v, now_ms(nullptr), seq);
                });
            if (res.found) {
              if (const auto* sc = scene_store.find(argv[1])) ev_plain(core::EventKind::Scene, static_cast<std::uint8_t>(core::SceneVia::Console), sc->id);
            }
            if (!res.found) say("%s: no such scene\n", argv[1]);
            else say("scene %s: %u sent, %u skipped\n", argv[1], static_cast<unsigned>(res.sent), static_cast<unsigned>(res.skipped));
          } else {
            say("usage: scene [<name> | add ... | del <name>]\n");
          }
          break;
        }
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

    // ---- Status LED: derive state from Wi-Fi + chip health ----
    if (status_led::current() != status_led::State::Off) {
      status_led::State ls;
      if (g_ap_up && !s_wifi_up) {
        ls = status_led::State::Setup;
      } else {
        const bool wifi = s_wifi_up.load();
        const bool zb_ok = coord.chip_ready(kZigbeeChipId);
        const bool th_ok = coord.chip_ready(kThreadChipId);
        const auto* zi = g_monitor->info(kZigbeeChipId);
        const auto* ti = g_monitor->info(kThreadChipId);
        const bool zb_silent = zi != nullptr && zi->silent;
        const bool th_silent = ti != nullptr && ti->silent;
        if (wifi && zb_ok && !zb_silent && th_ok && !th_silent)
          ls = status_led::State::Healthy;
        else if (!wifi && (zb_silent || !zb_ok) && (th_silent || !th_ok))
          ls = status_led::State::Offline;
        else
          ls = status_led::State::Degraded;
      }
      status_led::set(ls);
    }
    status_led::tick(t);

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
