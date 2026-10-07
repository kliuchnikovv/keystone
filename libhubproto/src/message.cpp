#include "hubproto/message.hpp"

namespace hubproto {

bool is_known_type(std::uint8_t t) noexcept {
  switch (static_cast<MsgType>(t)) {
    case MsgType::Hello:
    case MsgType::Declare:
    case MsgType::DeclareEnd:
    case MsgType::State:
    case MsgType::Command:
    case MsgType::CmdAck:
    case MsgType::Query:
    case MsgType::Heartbeat:
    case MsgType::OtaBegin:
    case MsgType::OtaBeginAck:
    case MsgType::OtaChunk:
    case MsgType::OtaChunkAck:
    case MsgType::OtaEnd:
    case MsgType::OtaEndAck:
    case MsgType::OtaAbort:
    case MsgType::OtaInfoReq:
    case MsgType::OtaInfo:
      return true;
  }
  return false;
}

void encode_header(const Header& h, std::uint8_t out[kHeaderSize]) noexcept {
  out[0] = h.version;
  out[1] = static_cast<std::uint8_t>(h.type);
  out[2] = h.chip_id;
  out[3] = h.boot_id;
  out[4] = static_cast<std::uint8_t>(h.seq & 0xFF);
  out[5] = static_cast<std::uint8_t>((h.seq >> 8) & 0xFF);
  out[6] = static_cast<std::uint8_t>(h.entity_id & 0xFF);
  out[7] = static_cast<std::uint8_t>((h.entity_id >> 8) & 0xFF);
}

bool decode_header(const std::uint8_t in[kHeaderSize], Header& out) noexcept {
  if (!is_known_type(in[1])) return false;
  out.version = in[0];
  out.type = static_cast<MsgType>(in[1]);
  out.chip_id = in[2];
  out.boot_id = in[3];
  out.seq = static_cast<std::uint16_t>(in[4] | (in[5] << 8));
  out.entity_id = static_cast<std::uint16_t>(in[6] | (in[7] << 8));
  return true;
}

}  // namespace hubproto
