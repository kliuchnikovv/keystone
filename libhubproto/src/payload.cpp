#include "hubproto/payload.hpp"

#include "hubproto/message.hpp"  // kSelfEntityId

namespace hubproto {

std::size_t value_size(ValueType vt) noexcept {
  switch (vt) {
    case ValueType::Bool:    return 1;
    case ValueType::U8:      return 1;
    case ValueType::I16:     return 2;
    case ValueType::U16:     return 2;
    case ValueType::F32:     return 4;
    case ValueType::XyColor: return 4;
    case ValueType::Enum8:   return 1;
    case ValueType::U32:     return 4;
  }
  return 0;
}

namespace {

bool is_known_value_type(std::uint8_t vt) noexcept {
  return value_size(static_cast<ValueType>(vt)) != 0;
}

constexpr std::size_t kDeclareFixed = 4;  // entity_id + device_type + attr_count
constexpr std::size_t kAttrBytes = 3;     // attr_id + value_type + flags

}  // namespace

EncodeResult encode_declare(std::uint16_t entity_id, DeviceType device_type,
                            const AttrDecl* attrs, std::uint8_t count,
                            std::uint8_t* out, std::size_t cap) noexcept {
  const std::size_t need = kDeclareFixed + kAttrBytes * count;
  if (cap < need) return {false, 0};

  std::size_t w = 0;
  out[w++] = static_cast<std::uint8_t>(entity_id & 0xFF);
  out[w++] = static_cast<std::uint8_t>((entity_id >> 8) & 0xFF);
  out[w++] = static_cast<std::uint8_t>(device_type);
  out[w++] = count;
  for (std::uint8_t i = 0; i < count; ++i) {
    out[w++] = attrs[i].attr_id;
    out[w++] = static_cast<std::uint8_t>(attrs[i].value_type);
    out[w++] = attrs[i].flags;
  }
  return {true, w};
}

bool decode_declare(const std::uint8_t* in, std::size_t len,
                    DeclareView& out) noexcept {
  if (len < kDeclareFixed) return false;
  const std::uint8_t count = in[3];
  if (len < kDeclareFixed + kAttrBytes * count) return false;

  for (std::uint8_t i = 0; i < count; ++i) {
    const std::uint8_t vt = in[kDeclareFixed + kAttrBytes * i + 1];
    if (!is_known_value_type(vt)) return false;
  }

  out.entity_id = static_cast<std::uint16_t>(in[0] | (in[1] << 8));
  out.device_type = static_cast<DeviceType>(in[2]);
  out.attr_count = count;
  out.attrs = in + kDeclareFixed;
  return true;
}

AttrDecl declare_attr_at(const DeclareView& view, std::uint8_t index) noexcept {
  const std::uint8_t* p = view.attrs + kAttrBytes * index;
  return AttrDecl{p[0], static_cast<ValueType>(p[1]), p[2]};
}

// ---- Value ----------------------------------------------------------------

namespace {

void put_u16_le(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

std::uint16_t get_u16_le(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void put_u32_le(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

std::uint32_t get_u32_le(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

Value typed(ValueType t) noexcept {
  Value v{};
  v.type = t;
  v.len = static_cast<std::uint8_t>(value_size(t));
  return v;
}

}  // namespace

Value Value::make_bool(bool v) noexcept {
  Value out = typed(ValueType::Bool);
  out.raw[0] = v ? 1 : 0;
  return out;
}
Value Value::make_u8(std::uint8_t v) noexcept {
  Value out = typed(ValueType::U8);
  out.raw[0] = v;
  return out;
}
Value Value::make_i16(std::int16_t v) noexcept {
  Value out = typed(ValueType::I16);
  put_u16_le(out.raw, static_cast<std::uint16_t>(v));
  return out;
}
Value Value::make_u16(std::uint16_t v) noexcept {
  Value out = typed(ValueType::U16);
  put_u16_le(out.raw, v);
  return out;
}
Value Value::make_u32(std::uint32_t v) noexcept {
  Value out = typed(ValueType::U32);
  put_u32_le(out.raw, v);
  return out;
}
Value Value::make_f32(float v) noexcept {
  Value out = typed(ValueType::F32);
  std::uint8_t tmp[4];
  __builtin_memcpy(tmp, &v, 4);  // host and ESP32 are little-endian
  for (int i = 0; i < 4; ++i) out.raw[i] = tmp[i];
  return out;
}
Value Value::make_xy(std::uint16_t x, std::uint16_t y) noexcept {
  Value out = typed(ValueType::XyColor);
  put_u16_le(out.raw, x);
  put_u16_le(out.raw + 2, y);
  return out;
}
Value Value::make_enum8(std::uint8_t v) noexcept {
  Value out = typed(ValueType::Enum8);
  out.raw[0] = v;
  return out;
}

bool Value::as_bool() const noexcept { return raw[0] != 0; }
std::uint8_t Value::as_u8() const noexcept { return raw[0]; }
std::int16_t Value::as_i16() const noexcept {
  return static_cast<std::int16_t>(get_u16_le(raw));
}
std::uint16_t Value::as_u16() const noexcept { return get_u16_le(raw); }
std::uint32_t Value::as_u32() const noexcept { return get_u32_le(raw); }
float Value::as_f32() const noexcept {
  float v;
  std::uint8_t tmp[4] = {raw[0], raw[1], raw[2], raw[3]};
  __builtin_memcpy(&v, tmp, 4);
  return v;
}
void Value::as_xy(std::uint16_t& x, std::uint16_t& y) const noexcept {
  x = get_u16_le(raw);
  y = get_u16_le(raw + 2);
}
std::uint8_t Value::as_enum8() const noexcept { return raw[0]; }

bool operator==(const Value& a, const Value& b) noexcept {
  if (a.type != b.type || a.len != b.len) return false;
  for (std::size_t i = 0; i < a.len; ++i) {
    if (a.raw[i] != b.raw[i]) return false;
  }
  return true;
}

bool read_value(const std::uint8_t* in, std::size_t len, ValueType vt,
                Value& out) noexcept {
  const std::size_t sz = value_size(vt);
  if (sz == 0 || len < sz) return false;
  out.type = vt;
  out.len = static_cast<std::uint8_t>(sz);
  for (std::size_t i = 0; i < sz; ++i) out.raw[i] = in[i];
  return true;
}

// ---- COMMAND --------------------------------------------------------------

EncodeResult encode_command(const Command& cmd, std::uint8_t* out,
                            std::size_t cap) noexcept {
  const std::size_t need = 3 + cmd.value.len;
  if (cap < need) return {false, 0};
  put_u16_le(out, cmd.entity_id);
  out[2] = cmd.attr_id;
  for (std::size_t i = 0; i < cmd.value.len; ++i) out[3 + i] = cmd.value.raw[i];
  return {true, need};
}

bool decode_command(const std::uint8_t* in, std::size_t len, ValueType vt,
                    Command& out) noexcept {
  if (len < 3) return false;
  out.entity_id = get_u16_le(in);
  out.attr_id = in[2];
  return read_value(in + 3, len - 3, vt, out.value);
}

// ---- CMD_ACK --------------------------------------------------------------

EncodeResult encode_cmd_ack(CmdAckStatus status, std::uint8_t* out,
                            std::size_t cap) noexcept {
  if (cap < 1) return {false, 0};
  out[0] = static_cast<std::uint8_t>(status);
  return {true, 1};
}

bool decode_cmd_ack(const std::uint8_t* in, std::size_t len,
                    CmdAckStatus& out) noexcept {
  if (len < 1 || in[0] > static_cast<std::uint8_t>(CmdAckStatus::Unsupported)) {
    return false;
  }
  out = static_cast<CmdAckStatus>(in[0]);
  return true;
}

// ---- STATE ----------------------------------------------------------------

EncodeResult encode_state(std::uint16_t entity_id, const StateEntry* entries,
                          std::uint8_t count, std::uint8_t* out,
                          std::size_t cap) noexcept {
  std::size_t need = 3;
  for (std::uint8_t i = 0; i < count; ++i) need += 1 + entries[i].value.len;
  if (cap < need) return {false, 0};

  std::size_t w = 0;
  put_u16_le(out, entity_id);
  w = 2;
  out[w++] = count;
  for (std::uint8_t i = 0; i < count; ++i) {
    out[w++] = entries[i].attr_id;
    for (std::size_t b = 0; b < entries[i].value.len; ++b) {
      out[w++] = entries[i].value.raw[b];
    }
  }
  return {true, w};
}

StateReader state_reader(const std::uint8_t* in, std::size_t len) noexcept {
  StateReader r{};
  if (len < 3) {
    r.valid = false;
    return r;
  }
  r.entity_id = get_u16_le(in);
  r.remaining = in[2];
  r.p = in + 3;
  r.end = in + len;
  r.valid = true;
  return r;
}

bool state_next_attr(StateReader& reader, std::uint8_t& attr_id) noexcept {
  if (!reader.valid || reader.remaining == 0) return false;
  if (reader.p >= reader.end) {
    reader.valid = false;
    return false;
  }
  attr_id = reader.p[0];
  return true;
}

bool state_consume(StateReader& reader, ValueType vt, Value& out) noexcept {
  if (!reader.valid || reader.remaining == 0) return false;
  const std::size_t sz = value_size(vt);
  const std::size_t avail = static_cast<std::size_t>(reader.end - reader.p);
  if (sz == 0 || avail < 1 + sz) {
    reader.valid = false;
    return false;
  }
  out.type = vt;
  out.len = static_cast<std::uint8_t>(sz);
  for (std::size_t i = 0; i < sz; ++i) out.raw[i] = reader.p[1 + i];
  reader.p += 1 + sz;
  --reader.remaining;
  return true;
}

// ---- Chip self entity -----------------------------------------------------

EncodeResult encode_chip_self_declare(std::uint16_t self_entity_id, std::uint8_t* out,
                                      std::size_t cap) noexcept {
  const std::uint8_t ro =
      kFlagReadable | kFlagReportable;  // telemetry is read-only
  const AttrDecl attrs[6] = {
      {chip_attr::kUptimeS,   ValueType::U32, ro},
      {chip_attr::kHeapFree,  ValueType::U32, ro},
      {chip_attr::kBootId,    ValueType::U8,  ro},
      {chip_attr::kRxFrames,  ValueType::U32, ro},
      {chip_attr::kTxFrames,  ValueType::U32, ro},
      {chip_attr::kCrcErrors, ValueType::U16, ro},
  };
  return encode_declare(self_entity_id, DeviceType::Chip, attrs, 6, out, cap);
}

EncodeResult encode_chip_self_state(std::uint16_t self_entity_id, const ChipSelf& s,
                                    std::uint8_t* out, std::size_t cap) noexcept {
  const StateEntry e[6] = {
      {chip_attr::kUptimeS,   Value::make_u32(s.uptime_s)},
      {chip_attr::kHeapFree,  Value::make_u32(s.heap_free)},
      {chip_attr::kBootId,    Value::make_u8(s.boot_id)},
      {chip_attr::kRxFrames,  Value::make_u32(s.rx_frames)},
      {chip_attr::kTxFrames,  Value::make_u32(s.tx_frames)},
      {chip_attr::kCrcErrors, Value::make_u16(s.crc_errors)},
  };
  return encode_state(self_entity_id, e, 6, out, cap);
}

// ---- OTA ------------------------------------------------------------------

namespace {

void put32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

std::uint32_t get32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool is_known_ota_status(std::uint8_t s) noexcept {
  return s <= static_cast<std::uint8_t>(OtaStatus::Unsupported);
}

}  // namespace

EncodeResult encode_ota_begin(const OtaBegin& in, std::uint8_t* out,
                              std::size_t cap) noexcept {
  constexpr std::size_t need = 4 + kOtaSha256Size;
  if (cap < need) return {false, 0};
  put32(out, in.total_size);
  for (std::size_t i = 0; i < kOtaSha256Size; ++i) out[4 + i] = in.sha256[i];
  return {true, need};
}

bool decode_ota_begin(const std::uint8_t* in, std::size_t len, OtaBegin& out) noexcept {
  if (len != 4 + kOtaSha256Size) return false;
  out.total_size = get32(in);
  for (std::size_t i = 0; i < kOtaSha256Size; ++i) out.sha256[i] = in[4 + i];
  return true;
}

EncodeResult encode_ota_begin_ack(const OtaBeginAck& in, std::uint8_t* out,
                                  std::size_t cap) noexcept {
  if (cap < 3) return {false, 0};
  out[0] = static_cast<std::uint8_t>(in.status);
  out[1] = static_cast<std::uint8_t>(in.chunk_size & 0xFF);
  out[2] = static_cast<std::uint8_t>((in.chunk_size >> 8) & 0xFF);
  return {true, 3};
}

bool decode_ota_begin_ack(const std::uint8_t* in, std::size_t len,
                          OtaBeginAck& out) noexcept {
  if (len != 3 || !is_known_ota_status(in[0])) return false;
  out.status = static_cast<OtaStatus>(in[0]);
  out.chunk_size = static_cast<std::uint16_t>(in[1] | (in[2] << 8));
  return true;
}

EncodeResult encode_ota_chunk(std::uint32_t offset, const std::uint8_t* data,
                              std::size_t data_len, std::uint8_t* out,
                              std::size_t cap) noexcept {
  if (data_len == 0 || data_len > kOtaMaxChunk) return {false, 0};
  const std::size_t need = kOtaChunkHeader + data_len;
  if (cap < need) return {false, 0};
  put32(out, offset);
  for (std::size_t i = 0; i < data_len; ++i) out[kOtaChunkHeader + i] = data[i];
  return {true, need};
}

bool decode_ota_chunk(const std::uint8_t* in, std::size_t len,
                      OtaChunkView& out) noexcept {
  if (len <= kOtaChunkHeader || len - kOtaChunkHeader > kOtaMaxChunk) return false;
  out.offset = get32(in);
  out.data = in + kOtaChunkHeader;
  out.len = len - kOtaChunkHeader;
  return true;
}

EncodeResult encode_ota_chunk_ack(const OtaChunkAck& in, std::uint8_t* out,
                                  std::size_t cap) noexcept {
  if (cap < 5) return {false, 0};
  out[0] = static_cast<std::uint8_t>(in.status);
  put32(out + 1, in.next_offset);
  return {true, 5};
}

bool decode_ota_chunk_ack(const std::uint8_t* in, std::size_t len,
                          OtaChunkAck& out) noexcept {
  if (len != 5 || !is_known_ota_status(in[0])) return false;
  out.status = static_cast<OtaStatus>(in[0]);
  out.next_offset = get32(in + 1);
  return true;
}

EncodeResult encode_ota_end_ack(OtaStatus status, std::uint8_t* out,
                                std::size_t cap) noexcept {
  if (cap < 1) return {false, 0};
  out[0] = static_cast<std::uint8_t>(status);
  return {true, 1};
}

bool decode_ota_end_ack(const std::uint8_t* in, std::size_t len,
                        OtaStatus& out) noexcept {
  if (len != 1 || !is_known_ota_status(in[0])) return false;
  out = static_cast<OtaStatus>(in[0]);
  return true;
}

EncodeResult encode_ota_info(const OtaInfo& in, std::uint8_t* out,
                             std::size_t cap) noexcept {
  if (cap < kOtaInfoSize) return {false, 0};
  out[0] = in.running_slot;
  out[1] = in.confirmed ? kOtaInfoFlagConfirmed : 0;
  out[2] = in.last_ota_slot;
  for (std::size_t i = 0; i < kOtaSha256Size; ++i) out[3 + i] = in.last_ota_sha256[i];
  return {true, kOtaInfoSize};
}

bool decode_ota_info(const std::uint8_t* in, std::size_t len, OtaInfo& out) noexcept {
  if (len != kOtaInfoSize) return false;
  out.running_slot = in[0];
  out.confirmed = (in[1] & kOtaInfoFlagConfirmed) != 0;
  out.last_ota_slot = in[2];
  for (std::size_t i = 0; i < kOtaSha256Size; ++i) out.last_ota_sha256[i] = in[3 + i];
  return true;
}

// ---- device OTA ---------------------------------------------------------------

bool is_known_dev_ota_phase(std::uint8_t p) noexcept {
  return p <= static_cast<std::uint8_t>(DevOtaPhase::Failed);
}

bool is_known_dev_ota_status(std::uint8_t s) noexcept {
  return s <= static_cast<std::uint8_t>(DevOtaStatus::Aborted);
}

EncodeResult encode_dev_ota_begin(const DevOtaBegin& in, std::uint8_t* out,
                                   std::size_t cap) noexcept {
  constexpr std::size_t need = 4 + kDevOtaSha256Size;
  if (cap < need) return {false, 0};
  put32(out, in.total_size);
  for (std::size_t i = 0; i < kDevOtaSha256Size; ++i) out[4 + i] = in.sha256[i];
  return {true, need};
}

bool decode_dev_ota_begin(const std::uint8_t* in, std::size_t len, DevOtaBegin& out) noexcept {
  if (len != 4 + kDevOtaSha256Size) return false;
  out.total_size = get32(in);
  for (std::size_t i = 0; i < kDevOtaSha256Size; ++i) out.sha256[i] = in[4 + i];
  return true;
}

EncodeResult encode_dev_ota_chunk(std::uint32_t offset, const std::uint8_t* data,
                                   std::size_t data_len, std::uint8_t* out,
                                   std::size_t cap) noexcept {
  if (data_len == 0 || data_len > kDevOtaMaxChunk) return {false, 0};
  const std::size_t need = kDevOtaChunkHeader + data_len;
  if (cap < need) return {false, 0};
  put32(out, offset);
  for (std::size_t i = 0; i < data_len; ++i) out[kDevOtaChunkHeader + i] = data[i];
  return {true, need};
}

bool decode_dev_ota_chunk(const std::uint8_t* in, std::size_t len,
                           DevOtaChunkView& out) noexcept {
  if (len <= kDevOtaChunkHeader || len - kDevOtaChunkHeader > kDevOtaMaxChunk) return false;
  out.offset = get32(in);
  out.data = in + kDevOtaChunkHeader;
  out.len = len - kDevOtaChunkHeader;
  return true;
}

EncodeResult encode_dev_ota_status(const DevOtaStatusMsg& in, std::uint8_t* out,
                                    std::size_t cap) noexcept {
  constexpr std::size_t need = 8;
  if (cap < need) return {false, 0};
  out[0] = static_cast<std::uint8_t>(in.phase);
  out[1] = static_cast<std::uint8_t>(in.status);
  out[2] = in.percent;
  out[3] = 0;
  put32(out + 4, in.next_offset);
  return {true, need};
}

bool decode_dev_ota_status(const std::uint8_t* in, std::size_t len,
                            DevOtaStatusMsg& out) noexcept {
  if (len != 8 || !is_known_dev_ota_phase(in[0]) || !is_known_dev_ota_status(in[1])) {
    return false;
  }
  out.phase = static_cast<DevOtaPhase>(in[0]);
  out.status = static_cast<DevOtaStatus>(in[1]);
  out.percent = in[2];
  out.next_offset = get32(in + 4);
  return true;
}

// ---- Matter ---------------------------------------------------------------

namespace {

bool is_known_matter_state(std::uint8_t s) noexcept {
  return s <= static_cast<std::uint8_t>(MatterState::Failed);
}

bool is_known_matter_code(std::uint8_t c) noexcept {
  return c <= static_cast<std::uint8_t>(MatterCode::Failed);
}

}  // namespace

EncodeResult encode_matter_pair(std::uint8_t kind, const std::uint8_t* bytes,
                                std::size_t len, std::uint8_t* out,
                                std::size_t cap) noexcept {
  if (bytes == nullptr || len == 0 || len > kMatterMaxPayload) return {false, 0};
  const std::size_t need = kMatterPairHeader + len;
  if (cap < need) return {false, 0};
  out[0] = kind;
  out[1] = static_cast<std::uint8_t>(len);
  for (std::size_t i = 0; i < len; ++i) out[kMatterPairHeader + i] = bytes[i];
  return {true, need};
}

bool decode_matter_pair(const std::uint8_t* in, std::size_t len,
                        MatterPairView& out) noexcept {
  if (len < kMatterPairHeader + 1) return false;
  const std::uint8_t n = in[1];
  if (n == 0 || n > kMatterMaxPayload) return false;
  if (len != kMatterPairHeader + n) return false;
  if (in[0] > kMatterKindQr) return false;
  out.kind = in[0];
  out.bytes = in + kMatterPairHeader;
  out.len = n;
  return true;
}

EncodeResult encode_matter_result(const MatterResult& in, std::uint8_t* out,
                                  std::size_t cap) noexcept {
  constexpr std::size_t need = 4;
  if (cap < need) return {false, 0};
  out[0] = static_cast<std::uint8_t>(in.state);
  out[1] = static_cast<std::uint8_t>(in.code);
  out[2] = static_cast<std::uint8_t>(in.entity_id & 0xFF);
  out[3] = static_cast<std::uint8_t>((in.entity_id >> 8) & 0xFF);
  return {true, need};
}

bool decode_matter_result(const std::uint8_t* in, std::size_t len,
                           MatterResult& out) noexcept {
  if (len != 4) return false;
  if (!is_known_matter_state(in[0]) || !is_known_matter_code(in[1])) return false;
  out.state = static_cast<MatterState>(in[0]);
  out.code = static_cast<MatterCode>(in[1]);
  out.entity_id = static_cast<std::uint16_t>(in[2] | (in[3] << 8));
  return true;
}

EncodeResult encode_thread_dataset(const std::uint8_t* bytes, std::size_t len,
                                    std::uint8_t* out, std::size_t cap) noexcept {
  if (bytes == nullptr || len == 0 || len > kThreadDatasetMax) return {false, 0};
  const std::size_t need = 1 + len;
  if (cap < need) return {false, 0};
  out[0] = static_cast<std::uint8_t>(len);
  for (std::size_t i = 0; i < len; ++i) out[1 + i] = bytes[i];
  return {true, need};
}

bool decode_thread_dataset(const std::uint8_t* in, std::size_t len,
                           ThreadDatasetView& out) noexcept {
  if (in == nullptr || len < 2) return false;
  const std::size_t n = in[0];
  if (n == 0 || n > kThreadDatasetMax) return false;
  if (len != 1 + n) return false;
  out.bytes = in + 1;
  out.len = n;
  return true;
}

}  // namespace hubproto
