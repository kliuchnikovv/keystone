#include "core/ota_sender.hpp"

#include "hubproto/version.hpp"

namespace core {

using hubproto::MsgType;
using hubproto::OtaStatus;

OtaSender::OtaSender(Bus& bus, Config cfg) noexcept : bus_(bus), cfg_(cfg) {
  bus_.subscribe(&OtaSender::on_msg, this);
}

std::uint16_t OtaSender::next_seq() noexcept {
  const std::uint16_t s = seq_counter_;
  seq_counter_ = (seq_counter_ == 0xFFFF) ? 1 : static_cast<std::uint16_t>(seq_counter_ + 1);
  return s;
}

void OtaSender::send(MsgType type, std::uint16_t seq, const std::uint8_t* payload,
                     std::size_t len) noexcept {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = chip_id_;
  h.boot_id = 0;
  h.seq = seq;
  h.entity_id = hubproto::kSelfEntityId;
  bus_.publish(h, payload, len);
}

void OtaSender::fail(Failure f, OtaStatus s) noexcept {
  state_ = State::Failed;
  failure_ = f;
  status_ = s;
  outstanding_ = false;
}

std::uint32_t OtaSender::timeout_for_state() const noexcept {
  switch (state_) {
    case State::Beginning: return cfg_.begin_timeout_ms;
    case State::Ending:    return cfg_.end_timeout_ms;
    default:               return cfg_.ack_timeout_ms;
  }
}

bool OtaSender::start(std::uint8_t chip_id, std::uint32_t total_size,
                      const std::uint8_t sha256[hubproto::kOtaSha256Size], ReadFn read,
                      void* read_ctx, std::uint32_t now_ms) noexcept {
  if (total_size == 0 || read == nullptr) return false;
  if (state_ == State::Beginning || state_ == State::Sending || state_ == State::Ending) {
    return false;
  }

  chip_id_ = chip_id;
  total_ = total_size;
  for (std::size_t i = 0; i < hubproto::kOtaSha256Size; ++i) sha256_[i] = sha256[i];
  read_ = read;
  read_ctx_ = read_ctx;
  chunk_size_ = hubproto::kOtaMaxChunk;
  acked_ = 0;
  failure_ = Failure::None;
  status_ = OtaStatus::Ok;

  const auto r = hubproto::encode_ota_begin({total_, {}}, out_buf_, sizeof(out_buf_));
  if (!r.ok) return false;
  for (std::size_t i = 0; i < hubproto::kOtaSha256Size; ++i) out_buf_[4 + i] = sha256_[i];
  out_len_ = r.len;
  out_type_ = MsgType::OtaBegin;
  out_seq_ = next_seq();
  state_ = State::Beginning;
  outstanding_ = true;
  attempts_ = 0;
  transmit(now_ms);
  return true;
}

void OtaSender::transmit(std::uint32_t now_ms) noexcept {
  sent_ms_ = now_ms;
  ++attempts_;
  // An ack may arrive synchronously inside send() and clear outstanding_, so all
  // bookkeeping above is done before the publish.
  send(out_type_, out_seq_, out_buf_, out_len_);
}

void OtaSender::abort() noexcept {
  if (state_ == State::Idle || finished()) return;
  send(MsgType::OtaAbort, next_seq(), nullptr, 0);
  fail(Failure::Aborted, OtaStatus::Ok);
}

void OtaSender::poll(std::uint32_t now_ms) noexcept {
  if (finished() || state_ == State::Idle) return;

  if (outstanding_) {
    if ((now_ms - sent_ms_) < timeout_for_state()) return;
    if (attempts_ > cfg_.max_retries) {
      fail(Failure::Timeout, OtaStatus::Ok);
      return;
    }
    transmit(now_ms);  // resend the same request with the same seq
    return;
  }

  if (state_ != State::Sending) return;

  if (acked_ >= total_) {  // every byte confirmed: ask the chip to verify + activate
    out_len_ = 0;
    out_type_ = MsgType::OtaEnd;
    out_seq_ = next_seq();
    state_ = State::Ending;
    outstanding_ = true;
    attempts_ = 0;
    transmit(now_ms);
    return;
  }

  const std::uint32_t remaining = total_ - acked_;
  const std::size_t want = (remaining < chunk_size_) ? remaining : chunk_size_;
  std::uint8_t data[hubproto::kOtaMaxChunk];
  const std::size_t got = read_(read_ctx_, data, want);
  if (got == 0 || got > want) {
    fail(Failure::SourceError, OtaStatus::Ok);
    return;
  }
  const auto r = hubproto::encode_ota_chunk(acked_, data, got, out_buf_, sizeof(out_buf_));
  if (!r.ok) {
    fail(Failure::SourceError, OtaStatus::Ok);
    return;
  }
  out_len_ = r.len;
  out_type_ = MsgType::OtaChunk;
  out_offset_ = acked_;
  out_data_len_ = got;
  out_seq_ = next_seq();
  outstanding_ = true;
  attempts_ = 0;
  transmit(now_ms);
}

void OtaSender::handle(const hubproto::Header& h, const std::uint8_t* payload,
                       std::size_t len) noexcept {
  if (!outstanding_ || h.chip_id != chip_id_ || h.seq != out_seq_) return;

  switch (h.type) {
    case MsgType::OtaBeginAck: {
      hubproto::OtaBeginAck a{};
      if (out_type_ != MsgType::OtaBegin || !hubproto::decode_ota_begin_ack(payload, len, a)) return;
      outstanding_ = false;
      if (a.status != OtaStatus::Ok) { fail(Failure::Refused, a.status); break; }
      if (a.chunk_size == 0) { fail(Failure::Refused, OtaStatus::BadState); break; }
      chunk_size_ = (a.chunk_size < hubproto::kOtaMaxChunk) ? a.chunk_size
                                                           : static_cast<std::uint16_t>(hubproto::kOtaMaxChunk);
      state_ = State::Sending;
      break;
    }
    case MsgType::OtaChunkAck: {
      hubproto::OtaChunkAck a{};
      if (out_type_ != MsgType::OtaChunk || !hubproto::decode_ota_chunk_ack(payload, len, a)) return;
      outstanding_ = false;
      if (a.status != OtaStatus::Ok ||
          a.next_offset != out_offset_ + static_cast<std::uint32_t>(out_data_len_)) {
        fail(Failure::Refused, a.status == OtaStatus::Ok ? OtaStatus::BadOffset : a.status);
        break;
      }
      acked_ = a.next_offset;
      break;
    }
    case MsgType::OtaEndAck: {
      OtaStatus s{};
      if (out_type_ != MsgType::OtaEnd || !hubproto::decode_ota_end_ack(payload, len, s)) return;
      outstanding_ = false;
      if (s == OtaStatus::Ok) state_ = State::Done;
      else fail(Failure::Refused, s);
      break;
    }
    default:
      return;
  }
  if (wake_fn_ != nullptr) wake_fn_(wake_ctx_);
}

}  // namespace core
