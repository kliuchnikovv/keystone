#include "core/uart_transport.hpp"

namespace core {

UartTransport::UartTransport(ByteLink& link) noexcept : link_(link) {
  link_.set_receiver(&UartTransport::on_bytes, this);
}

bool UartTransport::publish(const hubproto::Header& header,
                            const std::uint8_t* payload,
                            std::size_t payload_len) noexcept {
  auto r = hubproto::encode_frame(header, payload, payload_len, out_, sizeof(out_));
  if (!r.ok) return false;
  const bool ok = link_.write(out_, r.len);
  if (ok) ++tx_frames_;
  return ok;
}

bool UartTransport::subscribe(MessageFn fn, void* ctx) noexcept {
  if (fn == nullptr || sub_count_ >= kMaxSubscribers) return false;
  subs_[sub_count_].fn = fn;
  subs_[sub_count_].ctx = ctx;
  ++sub_count_;
  return true;
}

void UartTransport::on_bytes(void* ctx, const std::uint8_t* data,
                             std::size_t len) noexcept {
  static_cast<UartTransport*>(ctx)->handle_bytes(data, len);
}

void UartTransport::handle_bytes(const std::uint8_t* data,
                                 std::size_t len) noexcept {
  for (std::size_t i = 0; i < len; ++i) {
    if (reader_.push(data[i])) {
      ++rx_frames_;
      const hubproto::Header& h = reader_.header();
      const std::uint8_t* payload = reader_.payload();
      const std::size_t plen = reader_.payload_len();
      for (int s = 0; s < sub_count_; ++s) {
        subs_[s].fn(subs_[s].ctx, h, payload, plen);
      }
    }
  }
}

}  // namespace core
