#include "core/ota_receiver.hpp"

#include "hubproto/version.hpp"

namespace core {

using hubproto::MsgType;
using hubproto::OtaStatus;

OtaReceiver::OtaReceiver(Bus& bus, OtaSink& sink, Config cfg) noexcept
    : bus_(bus), sink_(sink), cfg_(cfg) {
  if (cfg_.chunk_size == 0 || cfg_.chunk_size > hubproto::kOtaMaxChunk) {
    cfg_.chunk_size = hubproto::kOtaMaxChunk;
  }
  bus_.subscribe(&OtaReceiver::on_msg, this);
}

void OtaReceiver::poll(std::uint32_t now_ms) noexcept {
  now_ms_ = now_ms;
  if (state_ == State::Receiving &&
      (now_ms - last_activity_ms_) >= cfg_.idle_timeout_ms) {
    drop();
  }
}

void OtaReceiver::drop() noexcept {
  if (state_ == State::Receiving) sink_.abort();
  state_ = State::Idle;
  total_ = 0;
  expected_ = 0;
}

void OtaReceiver::reply(MsgType type, const hubproto::Header& req,
                        const std::uint8_t* payload, std::size_t len) noexcept {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = cfg_.own_chip_id;
  h.boot_id = cfg_.boot_id;
  h.seq = req.seq;
  h.entity_id = hubproto::kSelfEntityId;
  bus_.publish(h, payload, len);
}

void OtaReceiver::reply_chunk_ack(const hubproto::Header& req, OtaStatus s) noexcept {
  std::uint8_t buf[5];
  const auto r = hubproto::encode_ota_chunk_ack({s, expected_}, buf, sizeof(buf));
  if (r.ok) reply(MsgType::OtaChunkAck, req, buf, r.len);
}

void OtaReceiver::reply_end_ack(const hubproto::Header& req, OtaStatus s) noexcept {
  std::uint8_t buf[1];
  const auto r = hubproto::encode_ota_end_ack(s, buf, sizeof(buf));
  if (r.ok) reply(MsgType::OtaEndAck, req, buf, r.len);
}

void OtaReceiver::handle(const hubproto::Header& h, const std::uint8_t* payload,
                         std::size_t len) noexcept {
  switch (h.type) {
    case MsgType::OtaBegin: on_begin(h, payload, len); break;
    case MsgType::OtaChunk: on_chunk(h, payload, len); break;
    case MsgType::OtaEnd:   on_end(h); break;
    case MsgType::OtaAbort: drop(); break;
    case MsgType::OtaInfoReq: on_info_req(h); break;
    default: break;
  }
}

void OtaReceiver::on_begin(const hubproto::Header& h, const std::uint8_t* p,
                           std::size_t n) noexcept {
  hubproto::OtaBegin b{};
  if (!hubproto::decode_ota_begin(p, n, b)) return;

  std::uint8_t buf[3];
  auto answer = [&](OtaStatus s) {
    const auto r = hubproto::encode_ota_begin_ack({s, cfg_.chunk_size}, buf, sizeof(buf));
    if (r.ok) reply(MsgType::OtaBeginAck, h, buf, r.len);
  };

  if (state_ == State::Finished) { answer(OtaStatus::Busy); return; }
  // A BEGIN while receiving means the brain restarted the transfer: it is the
  // authority, so drop the old one and start over.
  drop();
  if (b.total_size == 0) { answer(OtaStatus::TooLarge); return; }

  const OtaStatus s = sink_.begin(b.total_size);
  if (s != OtaStatus::Ok) { answer(s); return; }

  total_ = b.total_size;
  expected_ = 0;
  for (std::size_t i = 0; i < hubproto::kOtaSha256Size; ++i) sha_expected_[i] = b.sha256[i];
  sha_.reset();
  state_ = State::Receiving;
  last_activity_ms_ = now_ms_;
  answer(OtaStatus::Ok);
}

void OtaReceiver::on_chunk(const hubproto::Header& h, const std::uint8_t* p,
                           std::size_t n) noexcept {
  hubproto::OtaChunkView c{};
  if (!hubproto::decode_ota_chunk(p, n, c)) return;
  if (state_ != State::Receiving) { reply_chunk_ack(h, OtaStatus::BadState); return; }
  last_activity_ms_ = now_ms_;

  // A resend after a lost ack: already written, so confirm without touching flash.
  if (static_cast<std::uint64_t>(c.offset) + c.len <= expected_) {
    reply_chunk_ack(h, OtaStatus::Ok);
    return;
  }
  if (c.offset != expected_) { reply_chunk_ack(h, OtaStatus::BadOffset); return; }
  if (static_cast<std::uint64_t>(expected_) + c.len > total_) {
    reply_chunk_ack(h, OtaStatus::TooLarge);
    return;
  }
  if (!sink_.write(c.data, c.len)) {
    drop();
    reply_chunk_ack(h, OtaStatus::WriteFail);
    return;
  }
  sha_.update(c.data, c.len);
  expected_ += static_cast<std::uint32_t>(c.len);
  reply_chunk_ack(h, OtaStatus::Ok);
}

void OtaReceiver::on_end(const hubproto::Header& h) noexcept {
  // A resent END means our Ok ack was lost: the image is already accepted, so
  // confirm again rather than reporting a failure for a successful update.
  if (state_ == State::Finished) { reply_end_ack(h, OtaStatus::Ok); return; }
  if (state_ != State::Receiving) { reply_end_ack(h, OtaStatus::BadState); return; }

  if (expected_ != total_) {
    drop();
    reply_end_ack(h, OtaStatus::VerifyFail);
    return;
  }
  std::uint8_t digest[Sha256::kDigestSize];
  sha_.finish(digest);
  for (std::size_t i = 0; i < Sha256::kDigestSize; ++i) {
    if (digest[i] != sha_expected_[i]) {
      drop();
      reply_end_ack(h, OtaStatus::VerifyFail);
      return;
    }
  }
  const OtaStatus s = sink_.finish();
  if (s != OtaStatus::Ok) {
    drop();
    reply_end_ack(h, s);
    return;
  }
  sink_.record_image(sha_expected_);
  state_ = State::Finished;
  reply_end_ack(h, OtaStatus::Ok);
}

void OtaReceiver::on_info_req(const hubproto::Header& h) noexcept {
  hubproto::OtaInfo info{};
  if (!sink_.info(info)) return;
  std::uint8_t buf[hubproto::kOtaInfoSize];
  const auto r = hubproto::encode_ota_info(info, buf, sizeof(buf));
  if (r.ok) reply(MsgType::OtaInfo, h, buf, r.len);
}

}  // namespace core
