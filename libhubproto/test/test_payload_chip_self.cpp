#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "hubproto/payload.hpp"
#include "hubproto/message.hpp"

using hubproto::ChipSelf;
using hubproto::DeclareView;
using hubproto::DeviceType;
using hubproto::Value;
using hubproto::ValueType;
using hubproto::kSelfEntityId;
namespace attr = hubproto::chip_attr;

TEST_CASE("chip self declare round-trips") {
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_chip_self_declare(kSelfEntityId, buf.data(), buf.size());
  REQUIRE(r.ok);

  DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  CHECK(v.entity_id == kSelfEntityId);
  CHECK(v.device_type == DeviceType::Chip);
  REQUIRE(v.attr_count == 8);

  auto a0 = hubproto::declare_attr_at(v, 0);
  CHECK(a0.attr_id == attr::kUptimeS);
  CHECK(a0.value_type == ValueType::U32);

  auto a5 = hubproto::declare_attr_at(v, 5);
  CHECK(a5.attr_id == attr::kCrcErrors);
  CHECK(a5.value_type == ValueType::U16);

  auto a6 = hubproto::declare_attr_at(v, 6);
  CHECK(a6.attr_id == attr::kThreadRole);
  CHECK(a6.value_type == ValueType::U8);

  auto a7 = hubproto::declare_attr_at(v, 7);
  CHECK(a7.attr_id == attr::kThreadKids);
  CHECK(a7.value_type == ValueType::U8);
}

TEST_CASE("chip self state round-trips") {
  ChipSelf in{};
  in.uptime_s = 3600;
  in.heap_free = 120000;
  in.boot_id = 42;
  in.rx_frames = 1000;
  in.tx_frames = 1500;
  in.crc_errors = 3;
  in.thread_role = 4;     // leader
  in.thread_children = 2;

  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_chip_self_state(kSelfEntityId, in, buf.data(), buf.size());
  REQUIRE(r.ok);

  hubproto::StateReader rd = hubproto::state_reader(buf.data(), r.len);
  REQUIRE(rd.valid);
  CHECK(rd.entity_id == kSelfEntityId);

  auto resolve = [](std::uint8_t id) -> ValueType {
    switch (id) {
      case attr::kUptimeS:
      case attr::kHeapFree:
      case attr::kRxFrames:
      case attr::kTxFrames: return ValueType::U32;
      case attr::kBootId:   return ValueType::U8;
      case attr::kCrcErrors:return ValueType::U16;
      case attr::kThreadRole:
      case attr::kThreadKids:return ValueType::U8;
      default: return static_cast<ValueType>(0);
    }
  };

  ChipSelf out{};
  std::uint8_t a = 0;
  while (hubproto::state_next_attr(rd, a)) {
    Value v{};
    REQUIRE(hubproto::state_consume(rd, resolve(a), v));
    switch (a) {
      case attr::kUptimeS:   out.uptime_s = v.as_u32(); break;
      case attr::kHeapFree:  out.heap_free = v.as_u32(); break;
      case attr::kBootId:    out.boot_id = v.as_u8(); break;
      case attr::kRxFrames:  out.rx_frames = v.as_u32(); break;
      case attr::kTxFrames:  out.tx_frames = v.as_u32(); break;
      case attr::kCrcErrors: out.crc_errors = v.as_u16(); break;
      case attr::kThreadRole: out.thread_role = v.as_u8(); break;
      case attr::kThreadKids: out.thread_children = v.as_u8(); break;
    }
  }
  REQUIRE(rd.valid);

  CHECK(out.uptime_s == 3600);
  CHECK(out.heap_free == 120000);
  CHECK(out.boot_id == 42);
  CHECK(out.rx_frames == 1000);
  CHECK(out.tx_frames == 1500);
  CHECK(out.crc_errors == 3);
  CHECK(out.thread_role == 4);
  CHECK(out.thread_children == 2);
}