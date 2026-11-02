#include "device_ota_job.hpp"

#include <cstdio>
#include <cstring>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#if __has_include("esp_crt_bundle.h")
#include "esp_crt_bundle.h"
#endif

namespace fw {

namespace {

const char* kTag = "dev-ota";

// The device may take a while to answer the Image Notify (it polls every few
// seconds), so the session gets a generous window.
constexpr std::uint32_t kSessionTimeoutMs = 15 * 60 * 1000;

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

bool parse_hex_digest(const char* hex, std::uint8_t out[32]) {
  if (hex == nullptr || std::strlen(hex) != 64) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (int i = 0; i < 32; ++i) {
    const int hi = nib(hex[2 * i]);
    const int lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

const char* phase_text(hubproto::DevOtaPhase p) {
  switch (p) {
    case hubproto::DevOtaPhase::Receiving:    return "the chip is taking the file";
    case hubproto::DevOtaPhase::Verifying:    return "the chip is checking the file";
    case hubproto::DevOtaPhase::Notifying:    return "the device was told about the image";
    case hubproto::DevOtaPhase::Transferring: return "the device is pulling the image";
    case hubproto::DevOtaPhase::Done:         return "the device finished the update";
    case hubproto::DevOtaPhase::Failed:       return "the device failed the update";
    default:                                  return "starting";
  }
}

}  // namespace

DeviceOtaJob::DeviceOtaJob(core::Bus& bus) noexcept
    : mtx_(xSemaphoreCreateMutex()),
      wake_(xSemaphoreCreateBinary()),
      bus_(bus, mtx_),
      sender_(bus_) {
  sender_.on_wake(&DeviceOtaJob::wake_cb, this);
  sender_.on_phase(&DeviceOtaJob::phase_cb, this);
}

void DeviceOtaJob::wake_cb(void* ctx) { xSemaphoreGive(static_cast<DeviceOtaJob*>(ctx)->wake_); }

void DeviceOtaJob::phase_cb(void* ctx, std::uint16_t entity_id, hubproto::DevOtaPhase phase,
                            hubproto::DevOtaStatus status, std::uint8_t percent) {
  auto* j = static_cast<DeviceOtaJob*>(ctx);
  // Called with the job mutex held (the bus callback takes it).
  j->phase_ = phase;
  j->phase_status_ = status;
  j->percent_ = percent;
  if (phase == hubproto::DevOtaPhase::Done || phase == hubproto::DevOtaPhase::Failed) {
    j->session_done_ = true;
  }
  if (j->phase_fn_ != nullptr) {
    j->phase_fn_(j->phase_ctx_, entity_id, phase, status, percent);
  }
}

std::size_t DeviceOtaJob::read_cb(void* ctx, std::uint8_t* buf, std::size_t cap) {
  auto* j = static_cast<DeviceOtaJob*>(ctx);
  const std::size_t avail = j->pre_len_ - j->pre_pos_;
  const std::size_t n = avail < cap ? avail : cap;
  std::memcpy(buf, j->pre_ + j->pre_pos_, n);
  j->pre_pos_ += n;
  return n;
}

bool DeviceOtaJob::start(std::uint8_t chip_id, std::uint16_t entity_id, const char* url,
                          const char* sha256_hex, char* err, std::size_t err_cap) noexcept {
  auto fail = [&](const char* m) {
    std::snprintf(err, err_cap, "%s", m);
    return false;
  };
  if (running_) return fail("a device update is already running (see `ota dev status`)");
  if (url == nullptr || std::strlen(url) == 0 || std::strlen(url) >= sizeof(url_)) {
    return fail("bad url");
  }
  if (!parse_hex_digest(sha256_hex, sha256_)) return fail("sha256 must be 64 hex digits");

  chip_id_ = chip_id;
  entity_id_ = entity_id;
  std::strncpy(url_, url, sizeof(url_) - 1);
  url_[sizeof(url_) - 1] = '\0';
  abort_ = false;
  running_ = true;
  phase_ = hubproto::DevOtaPhase::Idle;
  phase_status_ = hubproto::DevOtaStatus::Ok;
  percent_ = 0;
  session_done_ = false;
  std::snprintf(result_, sizeof(result_), "starting");
  if (xTaskCreate(&DeviceOtaJob::task_entry, "devota", 8192, this, 5, nullptr) != pdPASS) {
    running_ = false;
    return fail("cannot create the update task (out of memory)");
  }
  return true;
}

void DeviceOtaJob::abort() noexcept {
  abort_ = true;
  xSemaphoreGive(wake_);
}

void DeviceOtaJob::task_entry(void* arg) {
  static_cast<DeviceOtaJob*>(arg)->run();
  vTaskDelete(nullptr);
}

void DeviceOtaJob::finish(bool ok, const char* msg) noexcept {
  std::snprintf(result_, sizeof(result_), "%s: %s", ok ? "OK" : "FAILED", msg);
  if (ok) {
    ESP_LOGI(kTag, "%s", result_);
  } else {
    ESP_LOGE(kTag, "%s", result_);
  }
  running_ = false;
}

void DeviceOtaJob::run() noexcept {
  esp_http_client_config_t cfg = {};
  cfg.url = url_;
  cfg.timeout_ms = 10000;
  cfg.buffer_size = 1024;
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) { finish(false, "cannot init the HTTP client"); return; }

  ESP_LOGI(kTag, "fetching %s", url_);
  if (esp_http_client_open(client, 0) != ESP_OK) {
    esp_http_client_cleanup(client);
    finish(false, "cannot connect (Wi-Fi up? url reachable?)");
    return;
  }
  const int64_t len = esp_http_client_fetch_headers(client);
  const int http_status = esp_http_client_get_status_code(client);
  if (http_status != 200) {
    char m[48];
    std::snprintf(m, sizeof(m), "server answered HTTP %d", http_status);
    esp_http_client_cleanup(client);
    finish(false, m);
    return;
  }
  if (len <= 0 || len > 0x7FFFFFFF) {
    esp_http_client_cleanup(client);
    finish(false, "server sent no Content-Length (chunked transfer is not supported)");
    return;
  }
  total_ = static_cast<std::uint32_t>(len);
  std::uint32_t http_remaining = total_;
  pre_len_ = pre_pos_ = 0;
  started_ms_ = now_ms();
  ESP_LOGI(kTag, "image is %u B for entity 0x%04x", static_cast<unsigned>(total_),
           static_cast<unsigned>(entity_id_));

  xSemaphoreTake(mtx_, portMAX_DELAY);
  const bool started = sender_.start(chip_id_, entity_id_, total_, sha256_, &DeviceOtaJob::read_cb,
                                     this, now_ms());
  xSemaphoreGive(mtx_);
  if (!started) {
    esp_http_client_cleanup(client);
    finish(false, "the sender refused to start");
    return;
  }

  // Read the next chunk from HTTP while NOT holding the sender mutex.
  auto prefetch = [&]() {
    if (pre_pos_ != pre_len_ || http_remaining == 0) return;
    const std::size_t want = http_remaining < sizeof(pre_) ? http_remaining : sizeof(pre_);
    std::size_t got = 0;
    int stalls = 0;
    while (got < want && !abort_) {
      const int r = esp_http_client_read(client, reinterpret_cast<char*>(pre_) + got,
                                         static_cast<int>(want - got));
      if (r > 0) { got += static_cast<std::size_t>(r); stalls = 0; continue; }
      if (r < 0 || ++stalls > 50) break;  // error, or ~5 s with no data
    }
    pre_len_ = got;
    pre_pos_ = 0;
    http_remaining -= static_cast<std::uint32_t>(got);
    // got < want leaves the sender with a short/empty prefetch: it fails with
    // SourceError, which is the right outcome for a truncated download.
  };

  unsigned last_pct = 101;
  for (;;) {
    prefetch();

    xSemaphoreTake(mtx_, portMAX_DELAY);
    if (abort_) sender_.abort();
    sender_.poll(now_ms());
    const bool done = sender_.finished();
    const std::uint32_t acked = sender_.acked();
    xSemaphoreGive(mtx_);

    // The chunk we just sent is on the wire: read the next one from HTTP now,
    // while the chip is writing and acking, instead of after the ack arrives.
    if (!done) prefetch();

    const unsigned pct =
        static_cast<unsigned>((static_cast<std::uint64_t>(acked) * 100) / (total_ ? total_ : 1));
    if (pct / 10 != last_pct / 10 && !done) {
      last_pct = pct;
      ESP_LOGI(kTag, "sending the image: %u%% (%u/%u B)", pct, static_cast<unsigned>(acked),
               static_cast<unsigned>(total_));
    }
    if (done) break;
    xSemaphoreTake(wake_, pdMS_TO_TICKS(20));
  }
  esp_http_client_cleanup(client);

  xSemaphoreTake(mtx_, portMAX_DELAY);
  const auto state = sender_.state();
  const auto failure = sender_.failure();
  const auto status = sender_.status();
  xSemaphoreGive(mtx_);

  if (state != core::DeviceOtaSender::State::Done) {
    char m[96];
    const char* why = failure == core::DeviceOtaSender::Failure::Timeout ? "timeout"
                     : failure == core::DeviceOtaSender::Failure::SourceError ? "the download failed"
                     : failure == core::DeviceOtaSender::Failure::Aborted ? "aborted"
                     : status == hubproto::DevOtaStatus::TooLarge ? "the image is too large"
                     : status == hubproto::DevOtaStatus::BadChecksum ? "checksum mismatch"
                     : status == hubproto::DevOtaStatus::Busy ? "the chip is busy"
                     : status == hubproto::DevOtaStatus::BadFormat ? "not a Zigbee OTA file"
                                                                  : "the chip refused the image";
    std::snprintf(m, sizeof(m), "the chip refused the image (%s)", why);
    finish(false, m);
    return;
  }

  ESP_LOGI(kTag, "the chip has the image; waiting for the device");
  // The chip now talks to the device: the session ends when the device reports
  // Done (or Failed), or when it stops answering.
  const std::uint32_t deadline = started_ms_ + kSessionTimeoutMs;
  while (!session_done_ && !abort_ && now_ms() < deadline) {
    xSemaphoreTake(mtx_, portMAX_DELAY);
    sender_.poll(now_ms());
    xSemaphoreGive(mtx_);
    xSemaphoreTake(wake_, pdMS_TO_TICKS(200));
  }

  if (abort_) {
    finish(false, "aborted");
    return;
  }
  if (phase_ == hubproto::DevOtaPhase::Done) {
    char m[96];
    std::snprintf(m, sizeof(m), "entity 0x%04x updated in %u s",
                  static_cast<unsigned>(entity_id_),
                  static_cast<unsigned>((now_ms() - started_ms_) / 1000));
    finish(true, m);
    return;
  }
  if (phase_ == hubproto::DevOtaPhase::Failed) {
    char m[96];
    std::snprintf(m, sizeof(m), "the device did not take the image (%s)",
                  phase_status_ == hubproto::DevOtaStatus::Mismatch
                      ? "it asked for another firmware"
                      : "the device aborted");
    finish(false, m);
    return;
  }
  finish(false, "the device did not answer in time");
}

void DeviceOtaJob::describe(char* out, std::size_t cap) noexcept {
  if (!running_) {
    std::snprintf(out, cap, "%s\n", result_);
    return;
  }
  xSemaphoreTake(mtx_, portMAX_DELAY);
  const std::uint32_t acked = sender_.acked();
  const std::uint32_t total = total_;
  const auto st = sender_.state();
  const auto phase = phase_;
  const auto percent = percent_;
  xSemaphoreGive(mtx_);
  const char* what = (st == core::DeviceOtaSender::State::Beginning)
                         ? "the chip is preparing its image partition"
                     : (st == core::DeviceOtaSender::State::Sending)
                         ? "sending the image to the chip"
                     : (st == core::DeviceOtaSender::State::Ending)
                         ? "the chip is checking the image"
                         : phase_text(phase);
  const unsigned pct = (phase == hubproto::DevOtaPhase::Transferring)
                           ? percent
                           : static_cast<unsigned>((static_cast<std::uint64_t>(acked) * 100) /
                                                  (total ? total : 1));
  std::snprintf(out, cap, "entity 0x%04x: %s - %u%% (%u/%u B)\n", static_cast<unsigned>(entity_id_),
                what, pct, static_cast<unsigned>(acked), static_cast<unsigned>(total));
}

}  // namespace fw
