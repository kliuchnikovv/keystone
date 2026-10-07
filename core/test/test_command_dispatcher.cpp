#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "core/command_dispatcher.hpp"
#include "core/entity_registry.hpp"
#include "core/in_memory_link.hpp"
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
using hubproto::Value;
using hubproto::ValueType;

namespace {

using Registry = EntityRegistry<>;

void declare_light(Registry& reg, std::uint8_t chip_id, std::uint16_t entity_id) {
  const std::array<AttrDecl, 1> attrs{{{0x01, ValueType::Bool, 0x07}}};
  std::array<std::uint8_t, 32> buf{};
  auto r = hubproto::encode_declare(entity_id, DeviceType::Light, attrs.data(),
                                    1, buf.data(), buf.size());
  REQUIRE(r.ok);
  hubproto::DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  REQUIRE(reg.declare(chip_id, v));
}

// The chip: counts COMMANDs and (optionally) replies CMD_ACK, echoing the seq.
struct AckChip {
  UartTransport* bus = nullptr;
  int received = 0;
  bool do_ack = true;
  CmdAckStatus status = CmdAckStatus::Ok;

  static void on(void* ctx, const Header& h, const std::uint8_t*, std::size_t) {
    auto* c = static_cast<AckChip*>(ctx);
    if (h.type != MsgType::Command) return;
    ++c->received;
    if (!c->do_ack) return;
    std::uint8_t buf[1];
    hubproto::encode_cmd_ack(c->status, buf, sizeof(buf));
    Header ack{};
    ack.version = hubproto::kProtocolVersion;
    ack.type = MsgType::CmdAck;
    ack.chip_id = h.chip_id;
    ack.seq = h.seq;
    ack.entity_id = h.entity_id;
    c->bus->publish(ack, buf, 1);
  }
};

struct Result {
  std::uint16_t seq;
  std::uint16_t entity_id;
  std::uint8_t attr_id;
  CmdAckStatus status;
};
struct ResultLog {
  std::vector<Result> results;
  static void on(void* ctx, std::uint16_t seq, std::uint16_t e, std::uint8_t a,
                 CmdAckStatus s) {
    static_cast<ResultLog*>(ctx)->results.push_back({seq, e, a, s});
  }
};

}  // namespace

TEST_CASE("send delivers a COMMAND and matches the CMD_ACK by seq") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_light(reg, 0x01, 0x0020);
  core::CommandDispatcher<Registry> disp(brain, reg);

  AckChip ackchip{&chip};
  chip.subscribe(&AckChip::on, &ackchip);
  ResultLog log;
  disp.on_result(&ResultLog::on, &log);

  std::uint16_t seq = 0;
  REQUIRE(disp.send(0x0020, 0x01, Value::make_bool(true), 0, seq));

  CHECK(ackchip.received == 1);
  REQUIRE(log.results.size() == 1);
  CHECK(log.results[0].seq == seq);
  CHECK(log.results[0].entity_id == 0x0020);
  CHECK(log.results[0].attr_id == 0x01);
  CHECK(log.results[0].status == CmdAckStatus::Ok);
  CHECK(disp.pending_count() == 0);
}

TEST_CASE("the COMMAND carries the right entity, attr and value") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_light(reg, 0x01, 0x0020);
  core::CommandDispatcher<Registry> disp(brain, reg);

  struct Seen {
    Header h{};
    Value v{};
    bool got = false;
    static void on(void* ctx, const Header& h, const std::uint8_t* p, std::size_t n) {
      if (h.type != MsgType::Command) return;
      auto* s = static_cast<Seen*>(ctx);
      hubproto::Command c{};
      REQUIRE(hubproto::decode_command(p, n, ValueType::Bool, c));
      s->h = h;
      s->v = c.value;
      s->got = true;
    }
  } seen;
  chip.subscribe(&Seen::on, &seen);

  std::uint16_t seq = 0;
  REQUIRE(disp.send(0x0020, 0x01, Value::make_bool(true), 0, seq));
  REQUIRE(seen.got);
  CHECK(seen.h.chip_id == 0x01);        // routed to the owning chip
  CHECK(seen.h.entity_id == 0x0020);
  CHECK(seen.v.as_bool() == true);
}

TEST_CASE("send rejects unknown entity and type mismatch") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_light(reg, 0x01, 0x0020);
  core::CommandDispatcher<Registry> disp(brain, reg);

  std::uint16_t seq = 0;
  CHECK_FALSE(disp.send(0x9999, 0x01, Value::make_bool(true), 0, seq));  // unknown entity
  CHECK_FALSE(disp.send(0x0020, 0x01, Value::make_u8(1), 0, seq));       // wrong type
  CHECK(disp.pending_count() == 0);
}

TEST_CASE("no ACK: retries then escalates to Unreachable") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_light(reg, 0x01, 0x0020);
  core::CommandDispatcher<Registry> disp(brain, reg, {1000, 2});

  AckChip silent{&chip};
  silent.do_ack = false;
  chip.subscribe(&AckChip::on, &silent);
  ResultLog log;
  disp.on_result(&ResultLog::on, &log);

  std::uint16_t seq = 0;
  REQUIRE(disp.send(0x0020, 0x01, Value::make_bool(true), 0, seq));
  CHECK(silent.received == 1);
  CHECK(disp.pending_count() == 1);

  disp.poll(500);   // before timeout
  CHECK(silent.received == 1);

  disp.poll(1000);  // 1st retry
  CHECK(silent.received == 2);
  disp.poll(2000);  // 2nd retry
  CHECK(silent.received == 3);

  disp.poll(3000);  // retries exhausted -> escalate
  CHECK(silent.received == 3);
  REQUIRE(log.results.size() == 1);
  CHECK(log.results[0].status == CmdAckStatus::Unreachable);
  CHECK(disp.pending_count() == 0);
}

TEST_CASE("a CMD_ACK with an unknown seq is ignored") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_light(reg, 0x01, 0x0020);
  core::CommandDispatcher<Registry> disp(brain, reg);
  ResultLog log;
  disp.on_result(&ResultLog::on, &log);

  std::uint8_t buf[1];
  hubproto::encode_cmd_ack(CmdAckStatus::Ok, buf, sizeof(buf));
  Header ack{};
  ack.version = hubproto::kProtocolVersion;
  ack.type = MsgType::CmdAck;
  ack.chip_id = 0x01;
  ack.seq = 0x7777;  // never issued
  chip.publish(ack, buf, 1);

  CHECK(log.results.empty());
}

namespace {
struct Alive {
  bool up = true;
  static bool fn(void* ctx, std::uint8_t) { return static_cast<Alive*>(ctx)->up; }
};
}  // namespace

TEST_CASE("a command to a dead chip fails at once, without being sent") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_light(reg, 0x01, 0x0020);
  core::CommandDispatcher<Registry> disp(brain, reg);
  AckChip ackchip{&chip};
  chip.subscribe(&AckChip::on, &ackchip);
  ResultLog log;
  disp.on_result(&ResultLog::on, &log);
  Alive alive;
  alive.up = false;
  disp.set_alive_filter(&Alive::fn, &alive);

  std::uint16_t seq = 0;
  REQUIRE(disp.send(0x0020, 0x01, Value::make_bool(true), 0, seq));
  CHECK(ackchip.received == 0);
  REQUIRE(log.results.size() == 1);
  CHECK(log.results[0].seq == seq);
  CHECK(log.results[0].status == CmdAckStatus::Unreachable);
  CHECK(disp.pending_count() == 0);

  alive.up = true;  // back: works again
  REQUIRE(disp.send(0x0020, 0x01, Value::make_bool(true), 0, seq));
  CHECK(ackchip.received == 1);
}

TEST_CASE("a pending command fails fast when its chip goes dead") {
  InMemoryLink la, lb;
  InMemoryLink::connect(la, lb);
  UartTransport brain(la);
  UartTransport chip(lb);
  Registry reg;
  declare_light(reg, 0x01, 0x0020);
  core::CommandDispatcher<Registry> disp(brain, reg);
  AckChip ackchip{&chip};
  ackchip.do_ack = false;  // never acks -> stays pending
  chip.subscribe(&AckChip::on, &ackchip);
  ResultLog log;
  disp.on_result(&ResultLog::on, &log);
  Alive alive;
  disp.set_alive_filter(&Alive::fn, &alive);

  std::uint16_t seq = 0;
  REQUIRE(disp.send(0x0020, 0x01, Value::make_bool(true), 0, seq));
  CHECK(disp.pending_count() == 1);
  alive.up = false;
  disp.poll(10);  // well before the retry timeout
  REQUIRE(log.results.size() == 1);
  CHECK(log.results[0].status == CmdAckStatus::Unreachable);
  CHECK(disp.pending_count() == 0);
  CHECK(ackchip.received == 1);  // no retries were sent
}
