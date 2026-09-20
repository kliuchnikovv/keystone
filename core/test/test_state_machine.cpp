#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "core/entity_registry.hpp"
#include "core/in_memory_link.hpp"
#include "core/state_machine.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::EntityRegistry;
using core::InMemoryLink;
using core::UartTransport;
using hubproto::AttrDecl;
using hubproto::DeviceType;
using hubproto::Header;
using hubproto::MsgType;
using hubproto::StateEntry;
using hubproto::Value;
using hubproto::ValueType;

namespace {

using Registry = EntityRegistry<>;

// Declare a sensor (temperature i16 @0x10, battery u8 @0x1F) directly into reg.
void declare_sensor(Registry& reg, std::uint8_t chip_id, std::uint16_t entity_id) {
  const std::array<AttrDecl, 2> attrs{{
      {0x10, ValueType::I16, 0x05},
      {0x1F, ValueType::U8, 0x05},
  }};
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_declare(entity_id, DeviceType::Sensor, attrs.data(),
                                    2, buf.data(), buf.size());
  REQUIRE(r.ok);
  hubproto::DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  REQUIRE(reg.declare(chip_id, v));
}

void publish_state(UartTransport& chip, std::uint8_t chip_id,
                   std::uint16_t entity_id, const StateEntry* entries,
                   std::uint8_t count) {
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_state(entity_id, entries, count, buf.data(), buf.size());
  REQUIRE(r.ok);
  Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = MsgType::State;
  h.chip_id = chip_id;
  h.entity_id = entity_id;
  chip.publish(h, buf.data(), r.len);
}

struct Change {
  std::uint16_t entity_id;
  std::uint8_t attr_id;
  Value value;
};

struct ChangeLog {
  std::vector<Change> changes;
  static void on(void* ctx, std::uint16_t e, std::uint8_t a, const Value& v) {
    static_cast<ChangeLog*>(ctx)->changes.push_back({e, a, v});
  }
};

}  // namespace

TEST_CASE("STATE updates the registry and fires change notifications") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_sensor(reg, 0x01, 0x0010);
  core::StateMachine<Registry> sm(brain, reg);

  ChangeLog log;
  REQUIRE(sm.on_change(&ChangeLog::on, &log));

  const std::array<StateEntry, 2> entries{{
      {0x10, Value::make_i16(-50)},
      {0x1F, Value::make_u8(95)},
  }};
  publish_state(chip, 0x01, 0x0010, entries.data(), 2);

  Value v{};
  REQUIRE(reg.get_value(0x0010, 0x10, v));
  CHECK(v.as_i16() == -50);
  REQUIRE(reg.get_value(0x0010, 0x1F, v));
  CHECK(v.as_u8() == 95);
  CHECK(log.changes.size() == 2);
}

TEST_CASE("a repeated STATE with the same values fires no change") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_sensor(reg, 0x01, 0x0010);
  core::StateMachine<Registry> sm(brain, reg);
  ChangeLog log;
  sm.on_change(&ChangeLog::on, &log);

  const std::array<StateEntry, 1> t{{{0x10, Value::make_i16(-50)}}};
  publish_state(chip, 0x01, 0x0010, t.data(), 1);
  publish_state(chip, 0x01, 0x0010, t.data(), 1);  // identical
  CHECK(log.changes.size() == 1);

  const std::array<StateEntry, 1> t2{{{0x10, Value::make_i16(-40)}}};
  publish_state(chip, 0x01, 0x0010, t2.data(), 1);  // changed
  REQUIRE(log.changes.size() == 2);
  CHECK(log.changes[1].attr_id == 0x10);
  CHECK(log.changes[1].value.as_i16() == -40);
}

TEST_CASE("STATE for an unknown entity is ignored") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;  // empty
  core::StateMachine<Registry> sm(brain, reg);
  ChangeLog log;
  sm.on_change(&ChangeLog::on, &log);

  const std::array<StateEntry, 1> t{{{0x10, Value::make_i16(-50)}}};
  publish_state(chip, 0x01, 0x0099, t.data(), 1);
  CHECK(log.changes.empty());
}
