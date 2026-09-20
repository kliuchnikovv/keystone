#include <doctest/doctest.h>

#include <array>
#include <cstdint>

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
