#include "relay_client.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "core/app_protocol.hpp"
#include "core/json.hpp"

namespace relay {
namespace {

const char* kTag = "relay";
constexpr int kMaxVClients = 3;
constexpr std::uint32_t kHelloTimeoutMs = 5000;
// Same token-guessing brake the local server has (app_server.cpp): after kAuthFailLimit
// wrong tokens inside kLockoutMs, every remote client is refused until the window passes.
// One lockout for the whole tunnel — the relay is a single trust boundary, we cannot see
// the client's address.
constexpr int kAuthFailLimit = 5;
constexpr std::uint32_t kLockoutMs = 60000;
constexpr const char* kNvs = "keystone";
constexpr std::size_t kOutQueue = 16;   // frames waiting to go out
constexpr int kMaxFlushPerTick = 8;
constexpr std::uint8_t kMaxSendTries = 3;  // retries per frame before it is given up on

struct VClient {
  int cid = 0;
  bool authed = false;
  std::uint32_t deadline_ms = 0;
};

esp_websocket_client_handle_t g_ws = nullptr;
SemaphoreHandle_t g_mu = nullptr;
QueueHandle_t g_in_q = nullptr;
QueueHandle_t g_out_q = nullptr;   // heap-allocated, NUL-terminated frames
HubInfoFn g_info = nullptr;
char g_token[core::app::kMaxToken + 1] = {};
char g_secret[64] = {};
char g_url[256] = {};
VClient g_vclients[kMaxVClients];
std::atomic<bool> g_tunnel_authed{false};
bool g_started = false;
int g_fails = 0;
std::uint32_t g_fail_until_ms = 0;
std::atomic<std::uint32_t> g_dropped{0};  // outbound frames lost (full queue or a dead socket)

// Frame accumulation (esp_websocket_client may fragment large frames).
char g_rx_buf[5120];
std::size_t g_rx_len = 0;
std::size_t g_rx_total = 0;
bool g_rx_over = false;   // the frame is bigger than g_rx_buf: drop its sender

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

struct Guard {
  Guard() { xSemaphoreTake(g_mu, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(g_mu); }
};

int cid_to_fd(int cid) { return kFdBase - cid; }
int fd_to_cid(int fd) { return kFdBase - fd; }

VClient* find_cid(int cid) {
  for (auto& c : g_vclients) if (c.cid == cid) return &c;
  return nullptr;
}

VClient* find_free() {
  for (auto& c : g_vclients) if (c.cid == 0) return &c;
  return nullptr;
}

std::size_t authed_count_locked() {
  std::size_t n = 0;
  for (auto& c : g_vclients) if (c.cid != 0 && c.authed) ++n;
  return n;
}

void clear_all() {
  Guard g;
  for (auto& c : g_vclients) c = VClient{};
}

// Hand a frame to the outbound queue. Never sends from the WebSocket event handler:
// the main loop drains the queue in tick(), outside the component's internal lock.
void tunnel_send(const char* msg, std::size_t len) {
  if (g_out_q == nullptr) return;
  char* copy = static_cast<char*>(std::malloc(len + 1));
  if (copy == nullptr) return;
  std::memcpy(copy, msg, len);
  copy[len] = '\0';
  if (xQueueSend(g_out_q, &copy, 0) != pdTRUE) {
    std::free(copy);
    ++g_dropped;
    ESP_LOGW(kTag, "outbound queue full, dropped a %u B frame", static_cast<unsigned>(len));
  }
}

// A frame that could not go out (socket down, or the component refused it) is put back for
// the next pass instead of vanishing: a dropped frame is a command or a snapshot part the
// remote client never sees. Retries are bounded, the counter is one slot in the queue item.
struct OutFrame {
  char* text;
  std::uint8_t tries;
};

void flush_out() {
  if (g_out_q == nullptr) return;
  for (int i = 0; i < kMaxFlushPerTick; ++i) {
    OutFrame f{nullptr, 0};
    if (xQueueReceive(g_out_q, &f, 0) != pdTRUE) return;
    const bool conn = g_ws != nullptr && esp_websocket_client_is_connected(g_ws);
    const int sent = conn ? esp_websocket_client_send_text(g_ws, f.text,
                                                           static_cast<int>(std::strlen(f.text)),
                                                           pdMS_TO_TICKS(1000))
                          : -1;
    if (sent < 0 && f.tries < kMaxSendTries) {
      f.tries++;
      if (xQueueSend(g_out_q, &f, 0) == pdTRUE) continue;
      ++g_dropped;
    } else if (sent < 0) {
      ++g_dropped;
    }
    std::free(f.text);
  }
}

void drop_out() {
  if (g_out_q == nullptr) return;
  OutFrame f{nullptr, 0};
  while (xQueueReceive(g_out_q, &f, 0) == pdTRUE) std::free(f.text);
}

// Close a virtual client for good: tell the relay (or its socket leaks and keeps counting
// against the client budget), then free the slot. Without this the relay hangs on to
// sockets the hub has already rejected.
void drop_cid(int cid, const char* why) {
  char msg[40];
  const int n = std::snprintf(msg, sizeof(msg), "!close:%d", cid);
  if (n > 0) tunnel_send(msg, static_cast<std::size_t>(n));
  Guard g;
  VClient* vc = find_cid(cid);
  if (vc != nullptr) {
    ESP_LOGW(kTag, "cid=%d dropped (%s)", cid, why);
    vc->cid = 0;
  }
}

bool locked_out() {
  Guard g;
  return g_fails >= kAuthFailLimit && static_cast<std::int32_t>(now_ms() - g_fail_until_ms) < 0;
}

void note_failure() {
  Guard g;
  ++g_fails;
  g_fail_until_ms = now_ms() + kLockoutMs;
}

void clear_failures() {
  Guard g;
  g_fails = 0;
  g_fail_until_ms = 0;
}

// "<cid>:<payload>" — parse the cid without atoi(): the frame in g_rx_buf is not
// NUL-terminated, and a stale digit after the frame would silently change the id.
int parse_cid(const char* p, const char* end) {
  int cid = 0;
  for (const char* q = p; q < end; ++q) {
    if (*q < '0' || *q > '9') return 0;
    cid = cid * 10 + (*q - '0');
    if (cid > 9999) return 0;
  }
  return cid;
}

// Send a reply to a remote client through the tunnel: "<cid>:<json>".
void client_reply(int cid, const char* json, std::size_t len) {
  char prefix[12];
  const int pn = std::snprintf(prefix, sizeof(prefix), "%d:", cid);
  if (pn <= 0) return;
  const std::size_t total = static_cast<std::size_t>(pn) + len;
  char* frame = static_cast<char*>(std::malloc(total));
  if (frame == nullptr) return;
  std::memcpy(frame, prefix, static_cast<std::size_t>(pn));
  std::memcpy(frame + pn, json, len);
  tunnel_send(frame, total);
  std::free(frame);
}

bool ct_equal(const char* a, const char* b) {
  const std::size_t la = std::strlen(a), lb = std::strlen(b);
  const std::size_t n = la > lb ? la : lb;
  unsigned diff = static_cast<unsigned>(la ^ lb);
  for (std::size_t i = 0; i < n; ++i) {
    diff |= static_cast<unsigned>((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
  }
  return diff == 0;
}

// A remote client sent a JSON frame through the tunnel. Run the same auth and decode
// the app_server does, then push Inbound to the relay queue.
void on_client_data(int cid, const char* text, std::size_t len) {
  using namespace core::app;

  bool is_authed = false;
  {
    Guard g;
    VClient* vc = find_cid(cid);
    if (vc == nullptr) return;   // already dropped: the relay is behind, nothing to answer
    is_authed = vc->authed;
  }

  core::JsonReader reader;
  const ClientMsg m = decode_client(reader, text, len);

  if (m.kind == ClientMsg::Kind::Invalid) {
    char out[128];
    core::JsonWriter w(out, sizeof(out));
    encode_error(w, -1, Err::BadJson);
    client_reply(cid, w.c_str(), w.size());
    if (!is_authed) drop_cid(cid, "bad json before hello");
    return;
  }

  if (!is_authed) {
    char out[512];
    core::JsonWriter w(out, sizeof(out));

    if (m.kind != ClientMsg::Kind::Hello) {
      encode_error(w, -1, Err::Unauthorized);
      client_reply(cid, w.c_str(), w.size());
      drop_cid(cid, "command before hello");
      return;
    }
    if (m.v != kProtocolVersion) {
      encode_error(w, -1, Err::Version);
      client_reply(cid, w.c_str(), w.size());
      drop_cid(cid, "wrong protocol version");
      return;
    }
    if (locked_out()) {
      encode_error(w, -1, Err::Unauthorized, "locked out");
      client_reply(cid, w.c_str(), w.size());
      drop_cid(cid, "locked out");
      return;
    }
    bool good = false;
    {
      Guard g;
      if (m.token_ok && g_token[0] != '\0') good = ct_equal(m.token, g_token);
    }
    if (!good) {
      note_failure();
      ESP_LOGW(kTag, "cid=%d bad token (%d failure(s) in the window)", cid, g_fails);
      encode_error(w, -1, Err::Unauthorized);
      client_reply(cid, w.c_str(), w.size());
      drop_cid(cid, "bad token");
      return;
    }
    clear_failures();
    {
      Guard g;
      VClient* v = find_cid(cid);
      if (v == nullptr) return;
      v->authed = true;
    }

    HubInfo info{"home", "?", 0, "UTC", false};
    if (g_info != nullptr) g_info(info);
    encode_welcome(w, info, false);
    client_reply(cid, w.c_str(), w.size());

    Inbound in{};
    in.kind = Inbound::Kind::Connected;
    in.fd = cid_to_fd(cid);
    in.setup_only = false;
    xQueueSend(g_in_q, &in, 0);
    ESP_LOGI(kTag, "cid=%d authenticated (%u remote)", cid,
             static_cast<unsigned>(authed_count_locked()));
    return;
  }

  // Already authenticated.
  switch (m.kind) {
    case ClientMsg::Kind::Cmd: {
      if (!m.has_id || !m.has_entity || !m.has_attr ||
          m.entity < 0 || m.entity > 0xFFFF || m.attr < 0 || m.attr > 0xFF) {
        char b[128];
        core::JsonWriter w(b, sizeof(b));
        encode_error(w, m.has_id ? m.id : -1, Err::BadArgs, m.has_id ? "entity/attr" : "missing id");
        client_reply(cid, w.c_str(), w.size());
        return;
      }
      Inbound in{};
      in.kind = Inbound::Kind::Cmd;
      in.fd = cid_to_fd(cid);
      in.id = m.id;
      in.entity = m.entity;
      in.attr = m.attr;
      in.value = m.value;
      if (xQueueSend(g_in_q, &in, 0) != pdTRUE) {
        char b[128];
        core::JsonWriter w(b, sizeof(b));
        encode_error(w, m.id, Err::Internal, "busy, retry");
        client_reply(cid, w.c_str(), w.size());
      }
      break;
    }
    case ClientMsg::Kind::Req: {
      if (!m.has_id) {
        char b[96];
        core::JsonWriter w(b, sizeof(b));
        encode_error(w, -1, Err::BadArgs, "missing id");
        client_reply(cid, w.c_str(), w.size());
        return;
      }
      Inbound in{};
      in.kind = Inbound::Kind::Req;
      in.fd = cid_to_fd(cid);
      in.setup_only = false;
      in.id = m.id;
      in.text = static_cast<char*>(std::malloc(len + 1));
      in.len = len;
      if (in.text != nullptr) {
        std::memcpy(in.text, text, len);
        in.text[len] = '\0';
      }
      if (in.text == nullptr || xQueueSend(g_in_q, &in, 0) != pdTRUE) {
        std::free(in.text);
        char b[128];
        core::JsonWriter w(b, sizeof(b));
        encode_res_error(w, m.id, Err::Internal, "busy, retry");
        client_reply(cid, w.c_str(), w.size());
      }
      break;
    }
    case ClientMsg::Kind::Hello:
      break;
    default: {
      char b[96];
      core::JsonWriter w(b, sizeof(b));
      encode_error(w, m.has_id ? m.id : -1, Err::Unsupported);
      client_reply(cid, w.c_str(), w.size());
      break;
    }
  }
}

// Handle a complete relay-protocol frame from the tunnel WebSocket.
void on_tunnel_message(const char* data, std::size_t len) {
  if (len == 0) return;

  if (len == 3 && std::strncmp(data, "!ok", 3) == 0) {
    g_tunnel_authed = true;
    ESP_LOGI(kTag, "tunnel authenticated — waiting for remote clients");
    return;
  }
  if (len > 7 && std::strncmp(data, "!error:", 7) == 0) {
    ESP_LOGW(kTag, "relay error: %.*s", static_cast<int>(len - 7), data + 7);
    return;
  }
  if (len > 6 && std::strncmp(data, "!open:", 6) == 0) {
    const int cid = parse_cid(data + 6, data + len);
    if (cid <= 0) return;
    {
      Guard g;
      if (find_cid(cid) != nullptr) return;
      VClient* vc = find_free();
      if (vc == nullptr) {
        // No room. Say why (the local server answers "busy" at this point) and hang up, or
        // the client sits there with a half-open socket and no explanation.
        ESP_LOGW(kTag, "no slot for cid=%d, refusing", cid);
        char err[96];
        core::JsonWriter w(err, sizeof(err));
        core::app::encode_error(w, -1, core::app::Err::Busy);
        client_reply(cid, w.c_str(), w.size());
        char msg[32];
        const int n = std::snprintf(msg, sizeof(msg), "!close:%d", cid);
        tunnel_send(msg, static_cast<std::size_t>(n));
        return;
      }
      *vc = VClient{cid, false, now_ms() + kHelloTimeoutMs};
    }
    char ready[32];
    const int n = std::snprintf(ready, sizeof(ready), "!ready:%d", cid);
    tunnel_send(ready, static_cast<std::size_t>(n));
    ESP_LOGI(kTag, "remote client cid=%d connected", cid);
    return;
  }
  if (len > 7 && std::strncmp(data, "!close:", 7) == 0) {
    const int cid = parse_cid(data + 7, data + len);
    if (cid <= 0) return;
    Guard g;
    if (VClient* vc = find_cid(cid)) {
      ESP_LOGI(kTag, "remote client cid=%d disconnected", cid);
      vc->cid = 0;
    }
    return;
  }

  // Data: <cid>:<payload>
  const char* colon = static_cast<const char*>(std::memchr(data, ':', len));
  if (colon == nullptr || colon == data) return;
  const int cid = parse_cid(data, colon);
  if (cid <= 0) return;
  const char* payload = colon + 1;
  const std::size_t plen = len - static_cast<std::size_t>(payload - data);
  if (plen > 0 && plen <= 64) {
    ESP_LOGI(kTag, "<- cid=%d %zu B: %.*s", cid, plen, static_cast<int>(plen), payload);
  } else {
    ESP_LOGI(kTag, "<- cid=%d %zu B", cid, plen);
  }
  on_client_data(cid, payload, plen);
}

void ws_event_handler(void*, esp_event_base_t, std::int32_t event_id, void* event_data) {
  auto* ev = static_cast<esp_websocket_event_data_t*>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
      ESP_LOGI(kTag, "tunnel connected, authenticating…");
      g_tunnel_authed = false;
      char auth[80];
      const int n = std::snprintf(auth, sizeof(auth), "!auth:%s", g_secret);
      if (n > 0) tunnel_send(auth, static_cast<std::size_t>(n));
      break;
    }
    case WEBSOCKET_EVENT_DATA:
      if (ev->op_code == 0x01 || ev->op_code == 0x00) {
        if (ev->payload_offset == 0) {
          g_rx_len = 0;
          g_rx_total = static_cast<std::size_t>(ev->payload_len);
          // The protocol allows 4 KB per frame (core::app::kMaxFrame); the "<cid>:" prefix
          // is the relay's, not the client's. Over that, the local server would close the
          // connection — do the same instead of parsing whatever arrived.
          g_rx_over = g_rx_total > core::app::kMaxFrame + 16;
        }
        if (g_rx_over) {
          // Too big to hold (the hub's frames are at most 4 KB). Read just enough to learn
          // who sent it and close that client, the same way the local server does.
          if (g_rx_len == 0) {
            const std::size_t n = ev->data_len < 12 ? static_cast<std::size_t>(ev->data_len) : 12;
            std::memcpy(g_rx_buf, ev->data_ptr, n);
            g_rx_len = n;
            const char* colon = static_cast<const char*>(std::memchr(g_rx_buf, ':', n));
            const int cid = colon != nullptr ? parse_cid(g_rx_buf, colon) : 0;
            ESP_LOGW(kTag, "cid=%d sent a frame over %u B, dropping", cid,
                     static_cast<unsigned>(core::app::kMaxFrame));
            if (cid > 0) drop_cid(cid, "frame over the limit");
          }
          break;
        }
        const std::size_t space = sizeof(g_rx_buf) - g_rx_len;
        const std::size_t copy = static_cast<std::size_t>(ev->data_len) < space
                                     ? static_cast<std::size_t>(ev->data_len)
                                     : space;
        std::memcpy(g_rx_buf + g_rx_len, ev->data_ptr, copy);
        g_rx_len += copy;
        if (g_rx_len >= g_rx_total && g_rx_total > 0) {
          on_tunnel_message(g_rx_buf, g_rx_total);
          g_rx_len = 0;
          g_rx_total = 0;
        }
      }
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(kTag, "tunnel disconnected");
      g_tunnel_authed = false;
      clear_all();
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(kTag, "tunnel connection error");
      break;
    default:
      break;
  }
}

}  // namespace

// ---- public API ------------------------------------------------------------------

bool configure(const char* url, const char* secret) {
  if (url == nullptr || secret == nullptr || url[0] == '\0' || secret[0] == '\0') return false;
  if (std::strlen(url) >= sizeof(g_url) || std::strlen(secret) >= sizeof(g_secret)) return false;
  nvs_handle_t h;
  if (nvs_open(kNvs, NVS_READWRITE, &h) != ESP_OK) return false;
  nvs_set_str(h, "relay_url", url);
  nvs_set_str(h, "relay_sec", secret);
  nvs_commit(h);
  nvs_close(h);
  std::strncpy(g_url, url, sizeof(g_url) - 1);
  std::strncpy(g_secret, secret, sizeof(g_secret) - 1);
  return true;
}

void clear_config() {
  nvs_handle_t h;
  if (nvs_open(kNvs, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "relay_url");
    nvs_erase_key(h, "relay_sec");
    nvs_commit(h);
    nvs_close(h);
  }
  g_url[0] = '\0';
  g_secret[0] = '\0';
}

void describe(char* buf, std::size_t cap) {
  if (g_url[0] == '\0') {
    std::snprintf(buf, cap, "relay: not configured (relay <url> <secret>)\n");
    return;
  }
  const bool conn = g_ws != nullptr && esp_websocket_client_is_connected(g_ws);
  std::size_t n_authed = 0;
  int fails = 0;
  if (g_mu != nullptr) {
    Guard g;
    n_authed = authed_count_locked();
    fails = g_fails;
  }
  std::snprintf(buf, cap, "relay: %s, tunnel %s%s, %u remote client(s)\n  url: %s\n",
                g_started ? "running" : "stopped",
                conn ? "connected" : "disconnected",
                (conn && g_tunnel_authed) ? " (authenticated)" : "",
                static_cast<unsigned>(n_authed), g_url);
  std::snprintf(buf + std::strlen(buf), cap - std::strlen(buf),
                "  auth failures in the window: %d (lockout at %d, 60 s), dropped frames: %u\n",
                fails, kAuthFailLimit, static_cast<unsigned>(g_dropped));
}

bool configured_url(char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return false;
  std::snprintf(out, cap, "%s", g_url);
  return g_url[0] != '\0';
}

bool start(const char* token, HubInfoFn info) {
  if (g_url[0] == '\0') {
    // Try loading from NVS.
    nvs_handle_t h;
    if (nvs_open(kNvs, NVS_READONLY, &h) != ESP_OK) return false;
    std::size_t ul = sizeof(g_url), sl = sizeof(g_secret);
    const bool ok = nvs_get_str(h, "relay_url", g_url, &ul) == ESP_OK &&
                    nvs_get_str(h, "relay_sec", g_secret, &sl) == ESP_OK &&
                    g_url[0] != '\0';
    nvs_close(h);
    if (!ok) { g_url[0] = '\0'; g_secret[0] = '\0'; return false; }
    ESP_LOGI(kTag, "loaded relay config from NVS: %s", g_url);
  }

  if (g_mu == nullptr) g_mu = xSemaphoreCreateMutex();
  if (g_in_q == nullptr) g_in_q = xQueueCreate(8, sizeof(Inbound));
  if (g_out_q == nullptr) g_out_q = xQueueCreate(kOutQueue, sizeof(OutFrame));
  if (g_mu == nullptr || g_in_q == nullptr || g_out_q == nullptr) return false;
  drop_out();

  {
    Guard g;
    if (token != nullptr) std::strncpy(g_token, token, sizeof(g_token) - 1);
    else g_token[0] = '\0';
  }
  g_info = info;

  if (g_ws != nullptr) stop();

  esp_websocket_client_config_t cfg = {};
  cfg.uri = g_url;
  cfg.buffer_size = 5120;
  cfg.reconnect_timeout_ms = 10000;
  cfg.network_timeout_ms = 10000;
  cfg.task_stack = 6144;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  g_ws = esp_websocket_client_init(&cfg);
  if (g_ws == nullptr) {
    ESP_LOGE(kTag, "esp_websocket_client_init failed");
    return false;
  }

  esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);

  if (esp_websocket_client_start(g_ws) != ESP_OK) {
    ESP_LOGE(kTag, "esp_websocket_client_start failed");
    esp_websocket_client_destroy(g_ws);
    g_ws = nullptr;
    return false;
  }

  g_started = true;
  ESP_LOGI(kTag, "connecting to %s", g_url);
  return true;
}

void stop() {
  drop_out();
  if (g_ws != nullptr) {
    esp_websocket_client_close(g_ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(g_ws);
    g_ws = nullptr;
  }
  g_tunnel_authed = false;
  g_started = false;
  clear_all();
  clear_failures();
  ESP_LOGI(kTag, "stopped");
}

void set_token(const char* token) {
  if (g_mu == nullptr) return;
  Guard g;
  if (token != nullptr) std::strncpy(g_token, token, sizeof(g_token) - 1);
  else g_token[0] = '\0';
}

bool running() { return g_started; }

bool connected() {
  return g_ws != nullptr && esp_websocket_client_is_connected(g_ws) && g_tunnel_authed;
}

void tick(std::uint32_t now) {
  // Everything the WebSocket task wanted to say goes out here, outside the component's lock.
  flush_out();
  if (g_mu == nullptr || !g_started) return;
  int expired[kMaxVClients];
  std::size_t n = 0;
  {
    Guard g;
    for (auto& c : g_vclients) {
      if (c.cid != 0 && !c.authed && static_cast<std::int32_t>(now - c.deadline_ms) > 0) {
        expired[n++] = c.cid;
      }
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    ESP_LOGW(kTag, "cid=%d no hello in %u ms, dropping", expired[i],
             static_cast<unsigned>(kHelloTimeoutMs));
    drop_cid(expired[i], "no hello");
  }
}

bool next(Inbound& out) {
  return g_in_q != nullptr && xQueueReceive(g_in_q, &out, 0) == pdTRUE;
}

void send(int fd, const char* data, std::size_t len) {
  if (!is_relay_fd(fd) || g_mu == nullptr || len == 0) return;
  const int cid = fd_to_cid(fd);
  bool ok = false;
  {
    Guard g;
    VClient* vc = find_cid(cid);
    ok = vc != nullptr && vc->authed;
  }
  if (ok) client_reply(cid, data, len);
}

void broadcast(const char* data, std::size_t len) {
  if (g_mu == nullptr || len == 0) return;
  int cids[kMaxVClients];
  std::size_t n = 0;
  {
    Guard g;
    for (auto& c : g_vclients)
      if (c.cid != 0 && c.authed) cids[n++] = c.cid;
  }
  for (std::size_t i = 0; i < n; ++i) client_reply(cids[i], data, len);
}

std::size_t client_count() {
  if (g_mu == nullptr) return 0;
  Guard g;
  return authed_count_locked();
}

}  // namespace relay
