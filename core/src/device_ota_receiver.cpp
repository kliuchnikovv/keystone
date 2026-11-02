#include "core/device_ota_receiver.hpp"

#include "hubproto/version.hpp"

namespace core {

using hubproto::DevOtaPhase;
using hubproto::DevOtaStatus;
using hubproto::MsgType;

namespace {

std::uint8_t percent_of(std::uint32_t done, std::uint32_t total) noexcept {
  if (total == 0) return 0;
  const std::uint64_t pct = (static_cast<std::uint64_t>(done) * 100) / total;
  return static_cast<std::uint8_t>(pct > 100 ? 100 : pct);
}

}  // namespace

DeviceOtaReceiver::DeviceOtaReceiver(Bus& bus, DeviceOtaSink& sink, Config cfg) noexcept
    : bus_(bus), sink_(sink), cfg_(cfg) {
  if (cfg_.chunk_size == 0 || cfg_.chunk_size > hubproto::kDevOtaMaxChunk) {
    cfg_.chunk_size = hubproto::kDevOtaMaxChunk;
  }
  bus_.subscribe(&DeviceOtaReceiver::on_msg, this);
}

void DeviceOtaReceiver::poll(std::uint32_t now_ms) noexcept {
  now_ms_ = now_ms;
  if (state_ == State::Receiving &&
      (now_ms - last_activity_ms_) >= cfg_.idle_timeout_ms) {
    drop();
  }
}

void DeviceOtaReceiver::drop() noexcept {
  if (state_ == State::Receiving) sink_.abort();
  state_ = State::Idle;
  total_ = 0;
  expected_ = 0;
}

void DeviceOtaReceiver::reply(const hubproto::Header& req, DevOtaPhase phase,
                               DevOtaStatus status, std::uint8_t percent,
                               std::uint32_t next_offset) noexcept {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = MsgType::DevOtaStatus;
  h.chip_id = cfg_.own_chip_id;
  h.boot_id = cfg_.boot_id;
  h.seq = req.seq;
  h.entity_id = entity_id_;

  hubproto::DevOtaStatusMsg m{};
  m.phase = phase;
  m.status = status;
  m.percent = percent;
  m.next_offset = next_offset;

  std::uint8_t buf[8];
  const auto r = hubproto::encode_dev_ota_status(m, buf, sizeof(buf));
  if (r.ok) bus_.publish(h, buf, r.len);
}

void DeviceOtaReceiver::report_phase(DevOtaPhase phase, DevOtaStatus status,
                                      std::uint8_t percent) noexcept {
  // A phase report is not an answer to a request, so it gets its own sequence
  // number (0) and the entity of the running update.
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = MsgType::DevOtaStatus;
  h.chip_id = cfg_.own_chip_id;
  h.boot_id = cfg_.boot_id;
  h.seq = 0;
  h.entity_id = entity_id_;

  hubproto::DevOtaStatusMsg m{};
  m.phase = phase;
  m.status = status;
  m.percent = percent;
  m.next_offset = expected_;

  std::uint8_t buf[8];
  const auto r = hubproto::encode_dev_ota_status(m, buf, sizeof(buf));
  if (r.ok) bus_.publish(h, buf, r.len);
}

void DeviceOtaReceiver::handle(const hubproto::Header& h, const std::uint8_t* payload,
                               std::size_t len) noexcept {
  switch (h.type) {
    case MsgType::DevOtaBegin: on_begin(h, payload, len); break;
    case MsgType::DevOtaChunk: on_chunk(h, payload, len); break;
    case MsgType::DevOtaEnd:   on_end(h); break;
    default: break;
  }
}

void DeviceOtaReceiver::on_begin(const hubproto::Header& h, const std::uint8_t* p,
                                  std::size_t n) noexcept {
  hubproto::DevOtaBegin b{};
  if (!hubproto::decode_dev_ota_begin(p, n, b)) return;

  if (state_ == State::Finished) {
    reply(h, DevOtaPhase::Failed, DevOtaStatus::Busy, 0, 0);
    return;
  }
  // A BEGIN while receiving means the brain restarted the transfer: it is the
  // authority, so drop the old one and start over.
  drop();
  entity_id_ = h.entity_id;
  if (b.total_size == 0) {
    reply(h, DevOtaPhase::Failed, DevOtaStatus::TooLarge, 0, 0);
    return;
  }

  const DevOtaStatus s = sink_.begin(h.entity_id, b.total_size);
  if (s != DevOtaStatus::Ok) {
    reply(h, DevOtaPhase::Failed, s, 0, 0);
    return;
  }

  total_ = b.total_size;
  expected_ = 0;
  for (std::size_t i = 0; i < hubproto::kDevOtaSha256Size; ++i) sha_expected_[i] = b.sha256[i];
  sha_.reset();
  state_ = State::Receiving;
  last_activity_ms_ = now_ms_;
  reply(h, DevOtaPhase::Receiving, DevOtaStatus::Ok, 0, 0);
}

void DeviceOtaReceiver::on_chunk(const hubproto::Header& h, const std::uint8_t* p,
                                  std::size_t n) noexcept {
  hubproto::DevOtaChunkView c{};
  if (!hubproto::decode_dev_ota_chunk(p, n, c)) return;
  if (state_ != State::Receiving) {
    reply(h, DevOtaPhase::Receiving, DevOtaStatus::Busy, 0, expected_);
    return;
  }
  last_activity_ms_ = now_ms_;

  // A resend after a lost ack: already written, so confirm without touching flash.
  if (static_cast<std::uint64_t>(c.offset) + c.len <= expected_) {
    reply(h, DevOtaPhase::Receiving, DevOtaStatus::Ok, percent_of(expected_, total_),
          expected_);
    return;
  }
  if (c.offset != expected_) {
    reply(h, DevOtaPhase::Receiving, DevOtaStatus::BadFormat, percent_of(expected_, total_),
          expected_);
    return;
  }
  if (static_cast<std::uint64_t>(expected_) + c.len > total_) {
    reply(h, DevOtaPhase::Receiving, DevOtaStatus::TooLarge, percent_of(expected_, total_),
          expected_);
    return;
  }
  if (!sink_.write(c.data, c.len)) {
    drop();
    reply(h, DevOtaPhase::Failed, DevOtaStatus::BadChecksum, 0, 0);
    return;
  }
  sha_.update(c.data, c.len);
  expected_ += static_cast<std::uint32_t>(c.len);
  reply(h, DevOtaPhase::Receiving, DevOtaStatus::Ok, percent_of(expected_, total_), expected_);
}

void DeviceOtaReceiver::on_end(const hubproto::Header& h) noexcept {
  // A resent END means our Ok answer was lost: the file is already accepted, so
  // confirm again rather than reporting a failure for a successful update.
  if (state_ == State::Finished) {
    reply(h, DevOtaPhase::Verifying, DevOtaStatus::Ok, 100, expected_);
    return;
  }
  if (state_ != State::Receiving) {
    reply(h, DevOtaPhase::Failed, DevOtaStatus::Busy, 0, 0);
    return;
  }

  if (expected_ != total_) {
    drop();
    reply(h, DevOtaPhase::Failed, DevOtaStatus::BadChecksum, 0, 0);
    return;
  }
  std::uint8_t digest[Sha256::kDigestSize];
  sha_.finish(digest);
  for (std::size_t i = 0; i < Sha256::kDigestSize; ++i) {
    if (digest[i] != sha_expected_[i]) {
      drop();
      reply(h, DevOtaPhase::Failed, DevOtaStatus::BadChecksum, 0, 0);
      return;
    }
  }
  const DevOtaStatus s = sink_.finish();
  if (s != DevOtaStatus::Ok) {
    drop();
    reply(h, DevOtaPhase::Failed, s, 0, 0);
    return;
  }
  state_ = State::Finished;
  reply(h, DevOtaPhase::Verifying, DevOtaStatus::Ok, 100, expected_);
}

}  // namespace core
