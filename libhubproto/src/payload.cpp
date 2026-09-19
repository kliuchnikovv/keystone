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

}  // namespace hubproto
