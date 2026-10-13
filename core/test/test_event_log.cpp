#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/app_protocol.hpp"
#include "core/app_requests.hpp"
#include "core/event_log.hpp"
#include "core/json.hpp"

using namespace core;
using hubproto::Value;

namespace {

Event state_ev(std::uint32_t t, std::uint16_t entity, std::uint8_t attr, Value v) {
  Event e;
  e.t_s = t;
  e.kind = EventKind::State;
  e.entity = entity;
  e.attr = attr;
  e.has_value = true;
  e.value = v;
  return e;
}

struct Collect final : EventSink {
  std::vector<Event> got;
  void emit(const Event& e) noexcept override { got.push_back(e); }
};

std::string encode(const Event* ev, std::size_t n, std::uint32_t oldest = 1, std::uint32_t latest = 9,
                   std::uint32_t now_s = 1000, std::int64_t wall = 1700000000, bool more = false) {
  static char buf[app::kMaxFrame];
  JsonWriter w(buf, sizeof(buf));
  app::encode_res_history(w, 7, wall, now_s, ev, n, oldest, latest, more);
  REQUIRE(w.ok());
  return std::string(buf, w.size());
}

}  // namespace

TEST_CASE("ring: sequence numbers count up from 1, newest first, bounds track") {
  EventRing<4> ring;
  CHECK(ring.latest() == 0);
  CHECK(ring.oldest() == 0);
  CHECK(ring.size() == 0);
  Event out[8];
  CHECK(ring.read(0, 8, out) == 0);

  for (std::uint32_t i = 1; i <= 3; ++i) ring.emit(state_ev(i * 10, 0x100, 1, Value::make_u8(static_cast<std::uint8_t>(i))));
  CHECK(ring.latest() == 3);
  CHECK(ring.oldest() == 1);
  CHECK(ring.size() == 3);
  const std::size_t n = ring.read(0, 8, out);
  REQUIRE(n == 3);
  CHECK(out[0].seq == 3);
  CHECK(out[1].seq == 2);
  CHECK(out[2].seq == 1);
  CHECK(out[0].t_s == 30);
  CHECK(out[0].value.as_u8() == 3);
}

TEST_CASE("ring: the oldest are overwritten, seq is never reused") {
  EventRing<4> ring;
  for (std::uint32_t i = 1; i <= 10; ++i) ring.emit(state_ev(i, 0x100, 1, Value::make_u8(1)));
  CHECK(ring.latest() == 10);
  CHECK(ring.oldest() == 7);
  CHECK(ring.size() == 4);
  Event out[8];
  const std::size_t n = ring.read(0, 8, out);
  REQUIRE(n == 4);
  CHECK(out[0].seq == 10);
  CHECK(out[3].seq == 7);
}

TEST_CASE("ring: paging with `before`, a limit, and a cursor outside the range") {
  EventRing<8> ring;
  for (std::uint32_t i = 1; i <= 8; ++i) ring.emit(state_ev(i, 0x100, 1, Value::make_u8(1)));
  Event out[8];
  std::size_t n = ring.read(0, 3, out);
  REQUIRE(n == 3);
  CHECK(out[2].seq == 6);
  n = ring.read(out[2].seq, 3, out);  // the next page starts strictly older
  REQUIRE(n == 3);
  CHECK(out[0].seq == 5);
  CHECK(out[2].seq == 3);
  n = ring.read(3, 8, out);
  REQUIRE(n == 2);
  CHECK(out[1].seq == 1);
  CHECK(ring.read(1, 8, out) == 0);    // nothing older than the first
  CHECK(ring.read(500, 2, out) == 2);  // a cursor from the future starts at the newest
  CHECK(out[0].seq == 8);
  CHECK(ring.read(0, 0, out) == 0);

  // After wrap-around a cursor that fell out of the ring yields nothing older.
  for (std::uint32_t i = 9; i <= 20; ++i) ring.emit(state_ev(i, 0x100, 1, Value::make_u8(1)));
  CHECK(ring.oldest() == 13);
  CHECK(ring.read(5, 4, out) == 0);
  CHECK(ring.read(14, 4, out) == 1);
}

TEST_CASE("ring: entity filter keeps what concerns that entity, and pages over the rest") {
  EventRing<16> ring;
  ring.emit(state_ev(1, 0x100, 1, Value::make_bool(true)));
  ring.emit(state_ev(2, 0x101, 1, Value::make_bool(true)));
  Event chip;
  chip.kind = EventKind::Chip;
  chip.entity = 0x100;  // not an entity event: never matches
  ring.emit(chip);
  Event rule;
  rule.kind = EventKind::Rule;
  rule.entity = 0x100;
  ring.emit(rule);
  ring.emit(state_ev(5, 0x100, 1, Value::make_bool(false)));
  Event out[8];
  const std::size_t n = ring.read(0, 8, out, true, 0x100);
  REQUIRE(n == 3);
  CHECK(out[0].seq == 5);
  CHECK(out[1].kind == EventKind::Rule);
  CHECK(out[2].seq == 1);
  CHECK(ring.read(0, 1, out, true, 0x100) == 1);  // a limit counts matches, not the events skipped
  CHECK(ring.read(0, 8, out, true, 0x777) == 0);
}

TEST_CASE("fanout: every sink sees every event; a full fanout refuses another") {
  Collect a, b;
  EventRing<4> ring;
  EventFanout<3> fan;
  CHECK(fan.add(&a));
  CHECK(fan.add(&ring));
  CHECK(fan.add(&b));
  CHECK_FALSE(fan.add(&a));
  CHECK_FALSE(EventFanout<2>{}.add(nullptr));
  fan.emit(state_ev(1, 0x100, 1, Value::make_bool(true)));
  fan.emit(state_ev(2, 0x100, 1, Value::make_bool(false)));
  CHECK(a.got.size() == 2);
  CHECK(b.got.size() == 2);
  CHECK(ring.latest() == 2);
}

TEST_CASE("what is worth keeping from the STATE stream") {
  const Value on = Value::make_bool(true);
  CHECK(state_is_historic(0x0100, 0x01, on));        // plug on/off
  CHECK(state_is_historic(0x0102, 0x50, on));        // occupancy
  CHECK(state_is_historic(0x0102, 0x10, Value::make_i16(215)));  // temperature
  CHECK_FALSE(state_is_historic(0x0100, 0x13, Value::make_u16(5)));  // power
  CHECK_FALSE(state_is_historic(0x0100, 0x14, Value::make_u32(5)));  // energy meter
  CHECK_FALSE(state_is_historic(0x0102, 0x12, Value::make_u16(5)));  // light level
  CHECK_FALSE(state_is_historic(0x0001, 0x71, Value::make_u32(5)));  // a chip's own telemetry
  CHECK_FALSE(state_is_historic(0x0002, 0x01, on));  // chip entity
  // A button press is an event; the return to idle is not.
  CHECK(state_is_historic(0x0110, hubproto::attr::kEvent, Value::make_enum8(static_cast<std::uint8_t>(hubproto::attr::ButtonEvent::Press))));
  CHECK_FALSE(state_is_historic(0x0110, hubproto::attr::kEvent, Value::make_enum8(static_cast<std::uint8_t>(hubproto::attr::ButtonEvent::Idle))));
}

TEST_CASE("history JSON: state, command, failure") {
  Event ev[3];
  ev[0] = state_ev(990, 0x0100, 0x01, Value::make_bool(true));
  ev[0].seq = 9;
  ev[1] = ev[0];
  ev[1].kind = EventKind::Command;
  ev[1].seq = 8;
  ev[1].t_s = 400;
  ev[2] = ev[1];
  ev[2].kind = EventKind::CmdFailed;
  ev[2].code = static_cast<std::uint8_t>(hubproto::CmdAckStatus::Unreachable);
  ev[2].seq = 7;
  const std::string j = encode(ev, 3);
  CHECK(j.find("\"now\":1700000000") != std::string::npos);
  CHECK(j.find("\"latest\":9") != std::string::npos);
  CHECK(j.find("\"oldest\":1") != std::string::npos);
  CHECK(j.find("\"more\":false") != std::string::npos);
  CHECK(encode(ev, 3, 1, 9, 1000, 1700000000, true).find("\"more\":true") != std::string::npos);
  CHECK(j.find(R"({"seq":9,"age":10,"kind":"state","entity":256,"attr":1,"value":true})") != std::string::npos);
  CHECK(j.find(R"({"seq":8,"age":600,"kind":"command","entity":256,"attr":1,"value":true})") != std::string::npos);
  CHECK(j.find(R"("kind":"command_failed","what":"unreachable","entity":256,"attr":1)") != std::string::npos);
}

TEST_CASE("history JSON: an empty ring and every kind's fields") {
  CHECK(encode(nullptr, 0, 0, 0).find(R"("events":[])") != std::string::npos);

  Event boot;
  boot.kind = EventKind::Boot;
  boot.code = static_cast<std::uint8_t>(app::ResetWhy::Watchdog);
  CHECK(encode(&boot, 1).find(R"("kind":"boot","what":"watchdog"})") != std::string::npos);

  Event rule;
  rule.kind = EventKind::Rule;
  rule.ref = 2;
  rule.entity = 0x0101;
  rule.attr = 1;
  rule.has_value = true;
  rule.value = Value::make_bool(false);
  CHECK(encode(&rule, 1).find(R"("kind":"rule","entity":257,"attr":1,"ref":2,"value":false})") != std::string::npos);

  rule.entity = kSceneTarget;  // a rule that applies a scene names no entity
  const std::string r2 = encode(&rule, 1);
  CHECK(r2.find("entity") == std::string::npos);
  CHECK(r2.find(R"("kind":"rule","ref":2})") != std::string::npos);

  Event scene;
  scene.kind = EventKind::Scene;
  scene.ref = 3;
  scene.code = static_cast<std::uint8_t>(SceneVia::Schedule);
  CHECK(encode(&scene, 1).find(R"("kind":"scene","what":"schedule","ref":3})") != std::string::npos);

  Event chip;
  chip.kind = EventKind::Chip;
  chip.ref = 1;
  chip.code = static_cast<std::uint8_t>(ChipEv::Lost);
  chip.has_value = true;
  chip.value = Value::make_u32(20);
  CHECK(encode(&chip, 1).find(R"("kind":"chip","what":"lost","ref":1,"value":20})") != std::string::npos);

  Event rec = state_ev(0, 0x100, 1, Value::make_bool(true));
  rec.kind = EventKind::Reconcile;
  rec.code = static_cast<std::uint8_t>(ReconcileEv::Adopted);
  CHECK(encode(&rec, 1).find(R"("kind":"reconcile","what":"adopted","entity":256,"attr":1,"value":true})") != std::string::npos);

  Event pair;
  pair.kind = EventKind::Pairing;
  pair.code = static_cast<std::uint8_t>(PairingEv::Opened);
  pair.ref = 180;
  CHECK(encode(&pair, 1).find(R"("kind":"pairing","what":"opened","ref":180})") != std::string::npos);
  pair.code = static_cast<std::uint8_t>(PairingEv::Unpair);
  pair.entity = 0x0100;
  CHECK(encode(&pair, 1).find(R"("kind":"pairing","what":"unpair","entity":256})") != std::string::npos);
}

TEST_CASE("history JSON: scene names are resolved when the hub answers") {
  static const char* names[] = {nullptr, "evening", "night"};
  const app::SceneNameFn lookup = [](void*, std::uint8_t id) -> const char* { return id < 3 ? names[id] : nullptr; };
  char buf[app::kMaxFrame];
  Event ev[3];
  ev[0].kind = EventKind::Scene;
  ev[0].ref = 2;
  ev[1].kind = EventKind::Timer;  // a timer that applies scene 1
  ev[1].entity = kSceneTarget;
  ev[1].attr = 1;
  ev[2].kind = EventKind::Scene;  // scene 9 was deleted since
  ev[2].ref = 9;
  JsonWriter w(buf, sizeof(buf));
  app::encode_res_history(w, 1, 0, 10, ev, 3, 1, 3, false, lookup, nullptr);
  REQUIRE(w.ok());
  const std::string j(buf, w.size());
  CHECK(j.find(R"("kind":"scene","what":"console","ref":2,"scene":"night"})") != std::string::npos);
  CHECK(j.find(R"("kind":"timer","ref":0,"scene":"evening"})") != std::string::npos);
  CHECK(j.find(R"("kind":"scene","what":"console","ref":9})") != std::string::npos);  // gone: no name, the id stays
}

TEST_CASE("history JSON: an uptime that wrapped still gives a small age") {
  Event e = state_ev(0xFFFFFFF0u, 0x100, 1, Value::make_bool(true));
  CHECK(encode(&e, 1, 1, 1, 5).find("\"age\":21") != std::string::npos);
}

TEST_CASE("history JSON: a full page of the largest events fits one frame") {
  std::vector<Event> evs(app::kHistoryPage);
  for (auto& e : evs) {
    e = state_ev(0, 0xFFFF, 0xFF, Value::make_xy(65535, 65535));
    e.seq = 4000000000u;
    e.kind = EventKind::CmdFailed;
    e.code = static_cast<std::uint8_t>(hubproto::CmdAckStatus::Unreachable);
    e.ref = 65535;
  }
  char buf[app::kMaxFrame];
  JsonWriter w(buf, sizeof(buf));
  app::encode_res_history(w, 2147483647, 4000000000ll, 4000000000u, evs.data(), evs.size(), 4000000000u, 4000000000u, true);
  CHECK(w.ok());
  CHECK(w.size() < app::kMaxFrame - 256);  // room to spare

  // And the reader accepts what the writer produced.
  BasicJsonReader<2048> r;
  CHECK(r.parse(buf, w.size()));
}

TEST_CASE("history request: defaults, limits and filters") {
  using Reader = BasicJsonReader<64>;
  auto parse = [](const char* args, app::HistoryQuery& q) {
    const std::string frame = std::string(R"({"type":"req","id":1,"op":"history")") + (args ? std::string(",\"args\":") + args : "") + "}";
    Reader r;
    const app::ClientMsg m = app::decode_client(r, frame.data(), frame.size());
    REQUIRE(m.kind == app::ClientMsg::Kind::Req);
    return app::history_parse(r, m.args, q);
  };
  app::HistoryQuery q;
  CHECK(parse(nullptr, q).ok);
  CHECK(q.before == 0);
  CHECK(q.limit == app::kHistoryPage);
  CHECK_FALSE(q.entity_only);

  CHECK(parse(R"({"before":120,"limit":10,"entity":256})", q).ok);
  CHECK(q.before == 120);
  CHECK(q.limit == 10);
  CHECK(q.entity_only);
  CHECK(q.entity == 256);

  CHECK(parse("{}", q).ok);
  CHECK(parse(R"({"entity":0})", q).ok);  // a legitimate filter value, not "no filter"
  CHECK(q.entity_only);

  CHECK_FALSE(parse(R"({"limit":0})", q).ok);
  CHECK_FALSE(parse(R"({"limit":25})", q).ok);
  CHECK_FALSE(parse(R"({"limit":"5"})", q).ok);
  CHECK_FALSE(parse(R"({"before":-1})", q).ok);
  CHECK_FALSE(parse(R"({"before":4294967296})", q).ok);
  CHECK_FALSE(parse(R"({"entity":65536})", q).ok);
  CHECK_FALSE(parse(R"({"entity":"a"})", q).ok);
  CHECK(parse(R"({"before":4294967295})", q).ok);
}
