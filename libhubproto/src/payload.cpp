#include "hubproto/payload.hpp"

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

}  // namespace hubproto
