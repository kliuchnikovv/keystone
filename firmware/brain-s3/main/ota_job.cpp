#include "ota_job.hpp"

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

const char* kTag = "ota";

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

const char* failure_text(core::OtaSender::Failure f, hubproto::OtaStatus s) {
  using F = core::OtaSender::Failure;
  switch (f) {
    case F::Timeout: return "chip stopped answering (timeout)";
    case F::SourceError: return "image source ended early";
    case F::Aborted: return "aborted";
    case F::Refused:
      switch (s) {
        case hubproto::OtaStatus::Busy: return "chip refused: busy";
        case hubproto::OtaStatus::TooLarge: return "chip refused: image too large for the slot";
        case hubproto::OtaStatus::BadState: return "chip refused: bad state";
        case hubproto::OtaStatus::BadOffset: return "chip lost sync (bad offset)";
        case hubproto::OtaStatus::WriteFail: return "chip flash write failed";
        case hubproto::OtaStatus::VerifyFail: return "chip rejected the image (sha256/validation)";
        case hubproto::OtaStatus::Unsupported: return "chip does not support OTA";
        default: return "chip refused";
      }
    default: return "failed";
  }
}

}  // namespace

OtaJob::OtaJob(core::Bus& bus) noexcept
    : mtx_(xSemaphoreCreateMutex()),
      wake_(xSemaphoreCreateBinary()),
      bus_(bus, mtx_),
      sender_(bus_) {
  sender_.on_wake(&OtaJob::wake_cb, this);
}

void OtaJob::wake_cb(void* ctx) { xSemaphoreGive(static_cast<OtaJob*>(ctx)->wake_); }

std::size_t OtaJob::read_cb(void* ctx, std::uint8_t* buf, std::size_t cap) {
  auto* j = static_cast<OtaJob*>(ctx);
  const std::size_t avail = j->pre_len_ - j->pre_pos_;
  const std::size_t n = avail < cap ? avail : cap;
  std::memcpy(buf, j->pre_ + j->pre_pos_, n);
  j->pre_pos_ += n;
  return n;
}

bool OtaJob::start(std::uint8_t chip_id, const char* url, const char* sha256_hex,
                   char* err, std::size_t err_cap) noexcept {
  auto fail = [&](const char* m) {
    std::snprintf(err, err_cap, "%s", m);
    return false;
  };
  if (running_) return fail("an update is already running (see `ota status`)");
  if (url == nullptr || std::strlen(url) == 0 || std::strlen(url) >= sizeof(url_)) {
    return fail("bad url");
  }
  if (!parse_hex_digest(sha256_hex, sha256_)) return fail("sha256 must be 64 hex digits");

  chip_id_ = chip_id;
  std::strncpy(url_, url, sizeof(url_) - 1);
  url_[sizeof(url_) - 1] = '\0';
  abort_ = false;
  running_ = true;
  std::snprintf(result_, sizeof(result_), "starting");
  if (xTaskCreate(&OtaJob::task_entry, "ota", 8192, this, 5, nullptr) != pdPASS) {
    running_ = false;
    return fail("cannot create the OTA task (out of memory)");
  }
  return true;
}

void OtaJob::abort() noexcept {
  abort_ = true;
  xSemaphoreGive(wake_);
}

void OtaJob::task_entry(void* arg) {
  static_cast<OtaJob*>(arg)->run();
  vTaskDelete(nullptr);
}

void OtaJob::finish(bool ok, const char* msg) noexcept {
  std::snprintf(result_, sizeof(result_), "%s: %s", ok ? "OK" : "FAILED", msg);
  if (ok) {
    ESP_LOGI(kTag, "%s", result_);
  } else {
    ESP_LOGE(kTag, "%s", result_);
  }
  running_ = false;
}

void OtaJob::run() noexcept {
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
  ESP_LOGI(kTag, "image is %u bytes; updating chip 0x%02x", static_cast<unsigned>(total_),
           static_cast<unsigned>(chip_id_));

  xSemaphoreTake(mtx_, portMAX_DELAY);
  const bool started = sender_.start(chip_id_, total_, sha256_, &OtaJob::read_cb, this, now_ms());
  xSemaphoreGive(mtx_);
  if (!started) {
    esp_http_client_cleanup(client);
    finish(false, "sender refused to start");
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

    const unsigned pct = static_cast<unsigned>((static_cast<std::uint64_t>(acked) * 100) / total_);
    if (pct / 10 != last_pct / 10 && !done) {
      last_pct = pct;
      ESP_LOGI(kTag, "%u%% (%u/%u B)", pct, static_cast<unsigned>(acked),
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

  if (state == core::OtaSender::State::Done) {
    char m[96];
    std::snprintf(m, sizeof(m), "chip 0x%02x accepted %u B in %u s, rebooting into the new image",
                  static_cast<unsigned>(chip_id_), static_cast<unsigned>(total_),
                  static_cast<unsigned>((now_ms() - started_ms_) / 1000));
    finish(true, m);
  } else {
    finish(false, failure_text(failure, status));
  }
}

void OtaJob::describe(char* out, std::size_t cap) noexcept {
  if (!running_) {
    std::snprintf(out, cap, "%s\n", result_);
    return;
  }
  xSemaphoreTake(mtx_, portMAX_DELAY);
  const std::uint32_t acked = sender_.acked();
  const auto st = sender_.state();
  xSemaphoreGive(mtx_);
  const char* phase = st == core::OtaSender::State::Beginning ? "waiting for the chip to erase its slot"
                    : st == core::OtaSender::State::Sending   ? "sending"
                    : st == core::OtaSender::State::Ending    ? "chip is verifying the image"
                                                              : "starting";
  const unsigned pct = total_ ? static_cast<unsigned>((static_cast<std::uint64_t>(acked) * 100) / total_) : 0;
  std::snprintf(out, cap, "chip 0x%02x: %s - %u%% (%u/%u B)\n", static_cast<unsigned>(chip_id_),
                phase, pct, static_cast<unsigned>(acked), static_cast<unsigned>(total_));
}

}  // namespace fw
