#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/command_dispatcher.hpp"
#include "core/entity_registry.hpp"
#include "core/in_memory_link.hpp"
#include "core/reconciler.hpp"
#include "core/state_machine.hpp"
#include "core/uart_transport.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using core::EntityRegistry;
using core::InMemoryLink;
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
using Rec = core::Reconciler<Dispatcher>;

struct Clock {
  std::uint32_t t = 0;
  static std::uint32_t now(void* c) { return static_cast<Clock*>(c)->t; }
};

void declare_switch(Registry& reg, std::uint16_t entity_id) {
  const std::array<AttrDecl, 1> attrs{{{0x01, ValueType::Bool, 0x07}}};
  std::array<std::uint8_t, 32> buf{};
  auto r = hubproto::encode_declare(entity_id, DeviceType::Switch, attrs.data(), 1,
                                    buf.data(), buf.size());
  REQUIRE(r.ok);
  hubproto::DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  REQUIRE(reg.declare(0x01, v));
}

// The chip: acks commands, records the last value commanded.
struct Chip {
  UartTransport* bus = nullptr;
  int commands = 0;
  bool last = false;
  static void on(void* ctx, const Header& h, const std::uint8_t* p, std::size_t n) {
    if (h.type != MsgType::Command) return;
    auto* c = static_cast<Chip*>(ctx);
    hubproto::Command cmd{};
    REQUIRE(hubproto::decode_command(p, n, ValueType::Bool, cmd));
    ++c->commands;
    c->last = cmd.value.as_bool();
    std::uint8_t b[1];
    hubproto::encode_cmd_ack(CmdAckStatus::Ok, b, 1);
    Header ack{};
    ack.version = hubproto::kProtocolVersion;
    ack.type = MsgType::CmdAck;
    ack.chip_id = h.chip_id;
    ack.seq = h.seq;
    ack.entity_id = h.entity_id;
    c->bus->publish(ack, b, 1);
  }
};

struct Ev {
  Rec::Kind kind;
  bool actual, desired;
};
struct EvLog {
  std::vector<Ev> v;
  static void on(void* c, Rec::Kind k, std::uint16_t, std::uint8_t, const Value& a,
                 const Value& d) {
    static_cast<EvLog*>(c)->v.push_back({k, a.as_bool(), d.as_bool()});
  }
};

struct Rig {
  InMemoryLink la, lb;
  UartTransport brain{la}, chipbus{lb};
  Registry reg;
  core::StateMachine<Registry> sm{brain, reg};
  Dispatcher disp{brain, reg};
  Clock clock;
  Rec rec{disp, &Clock::now, &clock};
  Chip chip{&chipbus};
  EvLog log;
  Rig() {
    InMemoryLink::connect(la, lb);
    declare_switch(reg, 0x0100);
    chipbus.subscribe(&Chip::on, &chip);
    disp.on_send(&Rec::note_command_thunk, &rec);
    sm.on_report(&Rec::on_report_thunk, &rec);
    rec.on_event(&EvLog::on, &log);
  }
  void report(bool on) {
    const std::array<StateEntry, 1> e{{{0x01, Value::make_bool(on)}}};
    std::array<std::uint8_t, 16> buf{};
    auto r = hubproto::encode_state(0x0100, e.data(), 1, buf.data(), buf.size());
    REQUIRE(r.ok);
    Header h{};
    h.version = hubproto::kProtocolVersion;
    h.type = MsgType::State;
    h.chip_id = 0x01;
    h.entity_id = 0x0100;
    chipbus.publish(h, buf.data(), r.len);
  }
  void command(bool on) {
    std::uint16_t seq = 0;
    REQUIRE(disp.send(0x0100, 0x01, Value::make_bool(on), clock.t, seq));
  }
};

}  // namespace

TEST_CASE("a hub command becomes the desired value") {
  Rig r;
  CHECK(r.rec.desired().count() == 0);
  r.command(true);
  REQUIRE(r.rec.desired().count() == 1);
  CHECK(r.rec.desired().find(0x0100, 0x01)->value == Value::make_bool(true));
  CHECK(r.rec.take_dirty());
  CHECK_FALSE(r.rec.take_dirty());
  r.command(true);  // same value: nothing new to persist
  CHECK_FALSE(r.rec.take_dirty());
}

TEST_CASE("no desired -> an observed value is never 'corrected'") {
  Rig r;
  r.report(true);
  r.report(false);
  CHECK(r.chip.commands == 0);
  CHECK(r.log.v.empty());
  CHECK(r.rec.desired().count() == 0);  // and never fabricated from actual
}

TEST_CASE("first value after a gap that differs from desired gets one corrective command") {
  Rig r;
  r.command(true);                 // user turned it on
  r.chip.commands = 0;
  r.reg.clear_chip(0x01);          // chip rebooted: registry forgot the values
  declare_switch(r.reg, 0x0100);
  r.clock.t = 60000;
  r.report(false);                 // it came back off (power loss default)
  CHECK(r.chip.commands == 1);
  CHECK(r.chip.last == true);
  REQUIRE(r.log.v.size() == 1);
  CHECK(r.log.v[0].kind == Rec::Kind::Corrected);
  CHECK(r.log.v[0].actual == false);
  CHECK(r.log.v[0].desired == true);
  // The device obeys and reports on: nothing more happens.
  r.report(true);
  CHECK(r.chip.commands == 1);
  CHECK(r.log.v.size() == 1);
}

TEST_CASE("first value equal to desired needs no correction") {
  Rig r;
  r.command(true);
  r.chip.commands = 0;
  r.reg.clear_chip(0x01);
  declare_switch(r.reg, 0x0100);
  r.report(true);
  CHECK(r.chip.commands == 0);
  CHECK(r.log.v.empty());
}

TEST_CASE("a later physical toggle is adopted, not fought") {
  Rig r;
  r.command(true);
  r.report(true);                  // confirmation of our command
  r.chip.commands = 0;

  r.clock.t = 2000;                // within the grace after our command
  r.report(false);
  CHECK(r.rec.desired().find(0x0100, 0x01)->value == Value::make_bool(true));
  CHECK(r.log.v.empty());

  r.clock.t = 60000;               // long after: a human pressed the button
  r.report(true);
  r.report(false);
  CHECK(r.chip.commands == 0);     // never reverted
  REQUIRE(r.log.v.size() == 1);
  CHECK(r.log.v[0].kind == Rec::Kind::Adopted);
  CHECK(r.rec.desired().find(0x0100, 0x01)->value == Value::make_bool(false));
  CHECK(r.rec.take_dirty());
}

TEST_CASE("DesiredState save/load round-trips and rejects corrupt data") {
  core::DesiredState<8> a;
  REQUIRE(a.set(0x0100, 0x01, Value::make_bool(true), 0));
  REQUIRE(a.set(0x0101, 0x20, Value::make_u16(1234), 0));
  std::uint8_t buf[128];
  const std::size_t n = a.save(buf, sizeof(buf));
  REQUIRE(n == a.serialized_size());
  CHECK(a.save(buf, n - 1) == 0);

  core::DesiredState<8> b;
  REQUIRE(b.set(0x0999, 0x01, Value::make_bool(true), 0));
  REQUIRE(b.load(buf, n));
  CHECK(b.count() == 2);
  CHECK(b.find(0x0999, 0x01) == nullptr);  // replaced
  CHECK(b.find(0x0101, 0x20)->value == Value::make_u16(1234));

  core::DesiredState<8> c;
  CHECK_FALSE(c.load(buf, n - 1));
  std::uint8_t bad[128];
  std::memcpy(bad, buf, n);
  bad[6 + 3] = 0x7F;  // unknown type
  CHECK_FALSE(c.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[6 + 4] = 3;     // wrong length
  CHECK_FALSE(c.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[0] ^= 0xFF;
  CHECK_FALSE(c.load(bad, n));
  CHECK(c.count() == 0);
}

TEST_CASE("DesiredState: clear, full table") {
  core::DesiredState<2> d;
  REQUIRE(d.set(1, 1, Value::make_bool(true), 0));
  REQUIRE(d.set(2, 1, Value::make_bool(true), 0));
  CHECK_FALSE(d.set(3, 1, Value::make_bool(true), 0));
  CHECK(d.set(1, 1, Value::make_bool(false), 0));  // overwrite still fine
  CHECK(d.clear(1, 1));
  CHECK_FALSE(d.clear(1, 1));
  CHECK(d.set(3, 1, Value::make_bool(true), 0));
  CHECK(d.clear_entity(2) == 1);
  CHECK(d.count() == 1);
}
