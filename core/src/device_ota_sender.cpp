#include "core/device_ota_sender.hpp"

#include "hubproto/version.hpp"

namespace core {

using hubproto::DevOtaPhase;
using hubproto::DevOtaStatus;
using hubproto::MsgType;

DeviceOtaSender::DeviceOtaSender(Bus& bus, Config cfg) noexcept : bus_(bus), cfg_(cfg) {
  bus_.subscribe(&DeviceOtaSender::on_msg, this);
}

std::uint16_t DeviceOtaSender::next_seq() noexcept {
  const std::uint16_t s = seq_counter_;
  seq_counter_ = (seq_counter_ == 0xFFFF) ? 1 : static_cast<std::uint16_t>(seq_counter_ + 1);
  return s;
}

void DeviceOtaSender::send(MsgType type, std::uint16_t seq, const std::uint8_t* payload,
                            std::size_t len) noexcept {
  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = type;
  h.chip_id = chip_id_;
  h.boot_id = 0;
  h.seq = seq;
  h.entity_id = entity_id_;
  bus_.publish(h, payload, len);
}

void DeviceOtaSender::fail(Failure f, DevOtaStatus s) noexcept {
  state_ = State::Failed;
  failure_ = f;
  status_ = s;
  outstanding_ = false;
}

std::uint32_t DeviceOtaSender::timeout_for_state() const noexcept {
  switch (state_) {
    case State::Beginning: return cfg_.begin_timeout_ms;
    case State::Ending:    return cfg_.end_timeout_ms;
    default:               return cfg_.ack_timeout_ms;
  }
}

bool DeviceOtaSender::start(std::uint8_t chip_id, std::uint16_t entity_id,
                             std::uint32_t total_size,
                             const std::uint8_t sha256[hubproto::kDevOtaSha256Size],
                             ReadFn read, void* read_ctx, std::uint32_t now_ms) noexcept {
  if (total_size == 0 || read == nullptr) return false;
  if (state_ == State::Beginning || state_ == State::Sending || state_ == State::Ending) {
    return false;
  }

  chip_id_ = chip_id;
  entity_id_ = entity_id;
  total_ = total_size;
  for (std::size_t i = 0; i < hubproto::kDevOtaSha256Size; ++i) sha256_[i] = sha256[i];
  read_ = read;
  read_ctx_ = read_ctx;
  chunk_size_ = hubproto::kDevOtaMaxChunk;
  acked_ = 0;
  failure_ = Failure::None;
  status_ = DevOtaStatus::Ok;
  phase_ = DevOtaPhase::Idle;
  percent_ = 0;

  hubproto::DevOtaBegin begin{};
  begin.total_size = total_;
  for (std::size_t i = 0; i < hubproto::kDevOtaSha256Size; ++i) begin.sha256[i] = sha256_[i];
  const auto r = hubproto::encode_dev_ota_begin(begin, out_buf_, sizeof(out_buf_));
  if (!r.ok) return false;
  out_len_ = r.len;
  out_type_ = MsgType::DevOtaBegin;
  out_seq_ = next_seq();
  state_ = State::Beginning;
  outstanding_ = true;
  attempts_ = 0;
  transmit(now_ms);
  return true;
}

void DeviceOtaSender::transmit(std::uint32_t now_ms) noexcept {
  sent_ms_ = now_ms;
  ++attempts_;
  // An answer may arrive synchronously inside send() and clear outstanding_, so all
  // bookkeeping above is done before the publish.
  send(out_type_, out_seq_, out_buf_, out_len_);
}

void DeviceOtaSender::abort() noexcept {
  if (state_ == State::Idle || finished()) return;
  fail(Failure::Aborted, DevOtaStatus::Aborted);
}

void DeviceOtaSender::poll(std::uint32_t now_ms) noexcept {
  if (finished() || state_ == State::Idle) return;

  if (outstanding_) {
    if ((now_ms - sent_ms_) < timeout_for_state()) return;
    if (attempts_ > cfg_.max_retries) {
      fail(Failure::Timeout, DevOtaStatus::Timeout);
      return;
    }
    transmit(now_ms);  // resend the same request with the same seq
    return;
  }

  if (state_ != State::Sending) return;

  if (acked_ >= total_) {  // every byte confirmed: ask the chip to verify the file
    out_len_ = 0;
    out_type_ = MsgType::DevOtaEnd;
    out_seq_ = next_seq();
    state_ = State::Ending;
    outstanding_ = true;
    attempts_ = 0;
    transmit(now_ms);
    return;
  }

  const std::uint32_t remaining = total_ - acked_;
  const std::size_t want = (remaining < chunk_size_) ? remaining : chunk_size_;
  std::uint8_t data[hubproto::kDevOtaMaxChunk];
  const std::size_t got = read_(read_ctx_, data, want);
  if (got == 0 || got > want) {
    fail(Failure::SourceError, DevOtaStatus::Ok);
    return;
  }
  const auto r = hubproto::encode_dev_ota_chunk(acked_, data, got, out_buf_, sizeof(out_buf_));
  if (!r.ok) {
    fail(Failure::SourceError, DevOtaStatus::Ok);
    return;
  }
  out_len_ = r.len;
  out_type_ = MsgType::DevOtaChunk;
  out_offset_ = acked_;
  out_data_len_ = got;
  out_seq_ = next_seq();
  outstanding_ = true;
  attempts_ = 0;
  transmit(now_ms);
}

void DeviceOtaSender::handle(const hubproto::Header& h, const std::uint8_t* payload,
                              std::size_t len) noexcept {
  if (h.type != MsgType::DevOtaStatus || h.chip_id != chip_id_ || h.entity_id != entity_id_) {
    return;
  }
  hubproto::DevOtaStatusMsg m{};
  if (!hubproto::decode_dev_ota_status(payload, len, m)) return;

  const bool is_ack = outstanding_ && h.seq == out_seq_;
  if (!is_ack) {
    // A device-session report (the chip is talking to the device now).
    phase_ = m.phase;
    percent_ = m.percent;
    if (m.status != DevOtaStatus::Ok && m.phase == DevOtaPhase::Failed) {
      status_ = m.status;
    }
    if (phase_fn_ != nullptr) {
      phase_fn_(phase_ctx_, entity_id_, m.phase, m.status, m.percent);
    }
    if (wake_fn_ != nullptr) wake_fn_(wake_ctx_);
    return;
  }

  outstanding_ = false;
  if (m.status != DevOtaStatus::Ok) {
    fail(Failure::Refused, m.status);
    if (wake_fn_ != nullptr) wake_fn_(wake_ctx_);
    return;
  }

  switch (out_type_) {
    case MsgType::DevOtaBegin:
      state_ = State::Sending;
      break;
    case MsgType::DevOtaChunk:
      if (m.next_offset != out_offset_ + static_cast<std::uint32_t>(out_data_len_)) {
        fail(Failure::Refused, DevOtaStatus::BadFormat);
        break;
      }
      acked_ = m.next_offset;
      break;
    case MsgType::DevOtaEnd:
      // The chip took the file; from here on it reports the device session.
      state_ = State::Done;
      phase_ = m.phase;
      percent_ = m.percent;
      break;
    default:
      break;
  }
  if (wake_fn_ != nullptr) wake_fn_(wake_ctx_);
}

}  // namespace core
