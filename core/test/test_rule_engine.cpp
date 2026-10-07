#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "core/command_dispatcher.hpp"
#include "core/entity_registry.hpp"
#include "core/in_memory_link.hpp"
#include "core/rule_engine.hpp"
#include "core/state_machine.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::EntityRegistry;
using core::InMemoryLink;
using core::Rule;
using core::UartTransport;
using hubproto::AttrDecl;
using hubproto::CmdAckStatus;
using hubproto::DeviceType;
using hubproto::Header;
using hubproto::MsgType;
using hubproto::StateEntry;
using hubproto::Value;
using hubproto::ValueType;

namespace {

using Registry = EntityRegistry<>;
using Dispatcher = core::CommandDispatcher<Registry>;
using Rules = core::RuleEngine<Dispatcher>;

void declare_one(Registry& reg, std::uint8_t chip_id, std::uint16_t entity_id,
                 DeviceType dt, std::uint8_t attr_id, ValueType vt) {
  const std::array<AttrDecl, 1> attrs{{{attr_id, vt, 0x07}}};
  std::array<std::uint8_t, 32> buf{};
  auto r = hubproto::encode_declare(entity_id, dt, attrs.data(), 1, buf.data(),
                                    buf.size());
  REQUIRE(r.ok);
  hubproto::DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  REQUIRE(reg.declare(chip_id, v));
}

void publish_state(UartTransport& chip, std::uint16_t entity_id,
                   std::uint8_t attr_id, const Value& v) {
  const std::array<StateEntry, 1> e{{{attr_id, v}}};
  std::array<std::uint8_t, 32> buf{};
  auto r = hubproto::encode_state(entity_id, e.data(), 1, buf.data(), buf.size());
  REQUIRE(r.ok);
  Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = MsgType::State;
  h.chip_id = 0x01;
  h.entity_id = entity_id;
  chip.publish(h, buf.data(), r.len);
}

// The chip: records COMMANDs (decoded as bool) and acks them.
struct LightChip {
  UartTransport* bus = nullptr;
  int commands = 0;
  Header last{};
  Value last_value{};
  static void on(void* ctx, const Header& h, const std::uint8_t* p, std::size_t n) {
    if (h.type != MsgType::Command) return;
    auto* c = static_cast<LightChip*>(ctx);
    hubproto::Command cmd{};
    REQUIRE(hubproto::decode_command(p, n, ValueType::Bool, cmd));
    ++c->commands;
    c->last = h;
    c->last_value = cmd.value;
    std::uint8_t buf[1];
    hubproto::encode_cmd_ack(CmdAckStatus::Ok, buf, sizeof(buf));
    Header ack{};
    ack.version = hubproto::kProtocolVersion;
    ack.type = MsgType::CmdAck;
    ack.chip_id = h.chip_id;
    ack.seq = h.seq;
    ack.entity_id = h.entity_id;
    c->bus->publish(ack, buf, 1);
  }
};

struct Clock {
  std::uint32_t t = 0;
  static std::uint32_t now(void* ctx) { return static_cast<Clock*>(ctx)->t; }
};

bool truthy(const Value& v) { return v.as_bool(); }

}  // namespace

TEST_CASE("occupancy STATE fires a rule that commands the light on (end to end)") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  Registry reg;
  declare_one(reg, 0x01, 0x0010, DeviceType::Occupancy, 0x50, ValueType::Bool);
  declare_one(reg, 0x01, 0x0020, DeviceType::Light, 0x01, ValueType::Bool);

  core::StateMachine<Registry> sm(brain, reg);
  Dispatcher disp(brain, reg);
  Clock clock;
  Rules rules(disp, &Clock::now, &clock);

  Rule r{};
  r.trigger_entity = 0x0010;
  r.trigger_attr = 0x50;
  r.match = &truthy;
  r.action_entity = 0x0020;
  r.action_attr = 0x01;
  r.action_value = Value::make_bool(true);
  REQUIRE(rules.add_rule(r));

  REQUIRE(sm.on_change(&Rules::on_state_change, &rules));

  LightChip light{&chip};
  chip.subscribe(&LightChip::on, &light);

  // Motion detected.
  publish_state(chip, 0x0010, 0x50, Value::make_bool(true));

  REQUIRE(light.commands == 1);
  CHECK(light.last.entity_id == 0x0020);
  CHECK(light.last.chip_id == 0x01);
  CHECK(light.last_value.as_bool() == true);
  CHECK(disp.pending_count() == 0);  // the chip acked
}

TEST_CASE("a rule does not fire when its predicate is false") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_one(reg, 0x01, 0x0010, DeviceType::Occupancy, 0x50, ValueType::Bool);
  declare_one(reg, 0x01, 0x0020, DeviceType::Light, 0x01, ValueType::Bool);

  core::StateMachine<Registry> sm(brain, reg);
  Dispatcher disp(brain, reg);
  Clock clock;
  Rules rules(disp, &Clock::now, &clock);
  Rule r{};
  r.trigger_entity = 0x0010;
  r.trigger_attr = 0x50;
  r.match = &truthy;
  r.action_entity = 0x0020;
  r.action_attr = 0x01;
  r.action_value = Value::make_bool(true);
  rules.add_rule(r);
  sm.on_change(&Rules::on_state_change, &rules);

  LightChip light{&chip};
  chip.subscribe(&LightChip::on, &light);

  publish_state(chip, 0x0010, 0x50, Value::make_bool(false));  // no motion
  CHECK(light.commands == 0);
}

TEST_CASE("a change to an unrelated attribute does not fire the rule") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_one(reg, 0x01, 0x0010, DeviceType::Occupancy, 0x50, ValueType::Bool);
  declare_one(reg, 0x01, 0x0030, DeviceType::Contact, 0x51, ValueType::Bool);
  declare_one(reg, 0x01, 0x0020, DeviceType::Light, 0x01, ValueType::Bool);

  core::StateMachine<Registry> sm(brain, reg);
  Dispatcher disp(brain, reg);
  Clock clock;
  Rules rules(disp, &Clock::now, &clock);
  Rule r{};
  r.trigger_entity = 0x0010;
  r.trigger_attr = 0x50;
  r.match = &truthy;
  r.action_entity = 0x0020;
  r.action_attr = 0x01;
  r.action_value = Value::make_bool(true);
  rules.add_rule(r);
  sm.on_change(&Rules::on_state_change, &rules);

  LightChip light{&chip};
  chip.subscribe(&LightChip::on, &light);

  publish_state(chip, 0x0030, 0x51, Value::make_bool(true));  // contact, not occupancy
  CHECK(light.commands == 0);
}

TEST_CASE("a null predicate matches any change") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_one(reg, 0x01, 0x0010, DeviceType::Occupancy, 0x50, ValueType::Bool);
  declare_one(reg, 0x01, 0x0020, DeviceType::Light, 0x01, ValueType::Bool);

  core::StateMachine<Registry> sm(brain, reg);
  Dispatcher disp(brain, reg);
  Clock clock;
  Rules rules(disp, &Clock::now, &clock);
  Rule r{};
  r.trigger_entity = 0x0010;
  r.trigger_attr = 0x50;
  r.match = nullptr;  // always
  r.action_entity = 0x0020;
  r.action_attr = 0x01;
  r.action_value = Value::make_bool(false);
  rules.add_rule(r);
  sm.on_change(&Rules::on_state_change, &rules);

  LightChip light{&chip};
  chip.subscribe(&LightChip::on, &light);

  publish_state(chip, 0x0010, 0x50, Value::make_bool(false));
  CHECK(light.commands == 1);
}

TEST_CASE("add_rule reports when the table is full") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  Registry reg;
  Dispatcher disp(brain, reg);
  Clock clock;
  core::RuleEngine<Dispatcher, 1> rules(disp, &Clock::now, &clock);

  Rule r{};
  CHECK(rules.add_rule(r));
  CHECK_FALSE(rules.add_rule(r));
  CHECK(rules.rule_count() == 1);
}

// ---- data-driven rules: comparison + persistence ---------------------------

TEST_CASE("value_matches compares numerically per type") {
  using core::CmpOp;
  CHECK(core::value_matches(Value::make_u16(5), CmpOp::Always, Value::make_u16(0)));
  CHECK(core::value_matches(Value::make_u16(5), CmpOp::Gt, Value::make_u16(3)));
  CHECK_FALSE(core::value_matches(Value::make_u16(3), CmpOp::Gt, Value::make_u16(3)));
  CHECK(core::value_matches(Value::make_u16(3), CmpOp::Ge, Value::make_u16(3)));
  CHECK(core::value_matches(Value::make_i16(-5), CmpOp::Lt, Value::make_i16(2)));
  CHECK(core::value_matches(Value::make_u32(4000000000u), CmpOp::Gt, Value::make_u32(1)));
  CHECK(core::value_matches(Value::make_f32(1.5f), CmpOp::Le, Value::make_f32(1.5f)));
  CHECK(core::value_matches(Value::make_bool(true), CmpOp::Eq, Value::make_bool(true)));
  CHECK(core::value_matches(Value::make_bool(true), CmpOp::Ne, Value::make_bool(false)));
  CHECK(core::value_matches(Value::make_xy(1, 2), CmpOp::Eq, Value::make_xy(1, 2)));
  CHECK_FALSE(core::value_matches(Value::make_xy(1, 2), CmpOp::Lt, Value::make_xy(3, 4)));
  // Type mismatch never matches.
  CHECK_FALSE(core::value_matches(Value::make_u16(1), CmpOp::Eq, Value::make_u8(1)));
}

TEST_CASE("a data rule (op + operand) fires through the engine and can be removed") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);

  Registry reg;
  declare_one(reg, 0x01, 0x0010, DeviceType::Sensor, 0x13, ValueType::U16);
  declare_one(reg, 0x01, 0x0020, DeviceType::Light, 0x01, ValueType::Bool);

  core::StateMachine<Registry> sm(brain, reg);
  Dispatcher disp(brain, reg);
  Clock clock;
  Rules rules(disp, &Clock::now, &clock);

  Rule r{};
  r.trigger_entity = 0x0010;
  r.trigger_attr = 0x13;
  r.match = nullptr;
  r.op = core::CmpOp::Gt;
  r.operand = Value::make_u16(100);
  r.action_entity = 0x0020;
  r.action_attr = 0x01;
  r.action_value = Value::make_bool(true);
  REQUIRE(rules.add_rule(r));
  REQUIRE(sm.on_change(&Rules::on_state_change, &rules));

  LightChip light{&chip};
  chip.subscribe(&LightChip::on, &light);

  publish_state(chip, 0x0010, 0x13, Value::make_u16(50));
  CHECK(light.commands == 0);
  publish_state(chip, 0x0010, 0x13, Value::make_u16(150));
  CHECK(light.commands == 1);

  REQUIRE(rules.remove_rule(0));
  CHECK(rules.rule_count() == 0);
  CHECK_FALSE(rules.remove_rule(0));
  publish_state(chip, 0x0010, 0x13, Value::make_u16(200));
  CHECK(light.commands == 1);
}

TEST_CASE("data rules round-trip through save/load; in-code rules are kept and not stored") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  Registry reg;
  Dispatcher disp(brain, reg);
  Clock clock;
  Rules a(disp, &Clock::now, &clock);

  Rule code_rule{};
  code_rule.match = &truthy;  // in-code: not persisted
  REQUIRE(a.add_rule(code_rule));
  Rule d1{};
  d1.trigger_entity = 0x0102; d1.trigger_attr = 0x13; d1.match = nullptr;
  d1.op = core::CmpOp::Ge; d1.operand = Value::make_u16(1500);
  d1.action_entity = 0x0103; d1.action_attr = 0x01; d1.action_value = Value::make_bool(false);
  Rule d2 = d1;
  d2.op = core::CmpOp::Always; d2.operand = Value::make_bool(true);
  d2.action_value = Value::make_bool(true);
  REQUIRE(a.add_rule(d1));
  REQUIRE(a.add_rule(d2));

  CHECK(a.stored_count() == 2);
  std::uint8_t buf[256];
  const std::size_t n = a.save(buf, sizeof(buf));
  REQUIRE(n == a.serialized_size());
  CHECK(a.save(buf, n - 1) == 0);

  Rules b(disp, &Clock::now, &clock);
  Rule b_code{};
  b_code.match = &truthy;
  REQUIRE(b.add_rule(b_code));
  Rule stale = d1;
  stale.trigger_entity = 0x0999;  // replaced by load
  REQUIRE(b.add_rule(stale));
  REQUIRE(b.load(buf, n));
  REQUIRE(b.rule_count() == 3);
  CHECK(b.rule_at(0)->match != nullptr);
  CHECK(b.rule_at(1)->trigger_entity == 0x0102);
  CHECK(b.rule_at(1)->op == core::CmpOp::Ge);
  CHECK(b.rule_at(1)->operand == Value::make_u16(1500));
  CHECK(b.rule_at(1)->action_value == Value::make_bool(false));
  CHECK(b.rule_at(2)->action_value == Value::make_bool(true));
}

TEST_CASE("load rejects corrupt buffers and leaves the rules untouched") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  Registry reg;
  Dispatcher disp(brain, reg);
  Clock clock;
  Rules a(disp, &Clock::now, &clock);
  Rule d{};
  d.trigger_entity = 1; d.trigger_attr = 2; d.match = nullptr;
  d.op = core::CmpOp::Eq; d.operand = Value::make_u8(7);
  d.action_entity = 3; d.action_attr = 4; d.action_value = Value::make_bool(true);
  REQUIRE(a.add_rule(d));

  std::uint8_t buf[64];
  const std::size_t n = a.save(buf, sizeof(buf));
  REQUIRE(n > 0);

  Rules b(disp, &Clock::now, &clock);
  CHECK_FALSE(b.load(buf, n - 1));          // truncated
  std::uint8_t bad[64];
  std::memcpy(bad, buf, n);
  bad[0] ^= 0xFF;
  CHECK_FALSE(b.load(bad, n));              // bad magic
  std::memcpy(bad, buf, n);
  bad[6 + 3] = 99;                          // unknown op
  CHECK_FALSE(b.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[6 + 4] = 0x7F;                        // unknown value type
  CHECK_FALSE(b.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[6 + 5] = 3;                           // length disagrees with type
  CHECK_FALSE(b.load(bad, n));
  CHECK(b.rule_count() == 0);
  CHECK(b.load(buf, n));
  CHECK(b.rule_count() == 1);
}
