#include <doctest/doctest.h>

#include <array>
#include <set>
#include <cstdint>
#include <string>
#include <vector>

#include "core/app_requests.hpp"
#include "core/entity_registry.hpp"
#include "core/rule_engine.hpp"
#include "core/schedule_table.hpp"
#include "core/timer_table.hpp"

using namespace core;
using namespace core::app;
using hubproto::Value;
using hubproto::ValueType;

namespace {

struct FakeDisp {
  bool send(std::uint16_t, std::uint8_t, const Value&, std::uint32_t, std::uint16_t&) { return true; }
};
using Registry = EntityRegistry<>;
using Engine = RuleEngine<FakeDisp, 4>;
using Sched = ScheduleTable<4>;
using Timers = TimerTable<4>;
using Reader = BasicJsonReader<192>;

void declare(Registry& reg, std::uint8_t chip, std::uint16_t id, hubproto::DeviceType dt,
             std::vector<hubproto::AttrDecl> attrs) {
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_declare(id, dt, attrs.data(), static_cast<std::uint8_t>(attrs.size()), buf.data(), buf.size());
  REQUIRE(r.ok);
  hubproto::DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  REQUIRE(reg.declare(chip, v));
}

struct Home {
  Registry reg;
  FakeDisp disp;
  Engine rules{disp, nullptr, nullptr};
  Sched sched;
  Timers timers;
  core::SceneTable<3, 4> scenes;
  std::set<std::uint8_t> busy_ids;  // scene ids a schedule/timer/rule uses (what the brain computes)
  std::vector<std::string> sent;  // what scene.apply dispatched, as "entity:attr"

  Home() {
    declare(reg, 1, 0x0100, hubproto::DeviceType::Switch,
            {{0x01, ValueType::Bool, 0x07}, {0x13, ValueType::U16, 0x05}});
    declare(reg, 1, 0x0101, hubproto::DeviceType::Light,
            {{0x01, ValueType::Bool, 0x07}, {0x02, ValueType::U8, 0x07}});
    declare(reg, 1, 0x0102, hubproto::DeviceType::Occupancy, {{0x50, ValueType::Bool, 0x05}});
    declare(reg, 1, 0x0103, hubproto::DeviceType::Switch, {{0x01, ValueType::Bool, 0x05}});  // read-only on/off
    declare(reg, 1, 0x0001, hubproto::DeviceType::Chip, {{0x71, ValueType::U32, 0x05}});     // chip self entity
  }

  // Parse a whole request frame, as the hub does, and hand its args to `op`.
  template <typename Fn>
  Outcome call(const std::string& args_json, Fn op) {
    const std::string frame = R"({"type":"req","id":1,"op":"x","args":)" + args_json + "}";
    Reader r;
    const ClientMsg m = decode_client(r, frame.data(), frame.size());
    REQUIRE(m.kind == ClientMsg::Kind::Req);
    return op(r, m.args);
  }
  Outcome name_set(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::name_set(reg, r, g); }); }
  Outcome name_clear(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::name_clear(reg, r, g); }); }
  Outcome rule_add(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::rule_add(reg, scenes, rules, r, g); }); }
  Outcome rule_del(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::rule_del(rules, r, g); }); }
  Outcome sched_add(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::schedule_add(reg, scenes, sched, r, g); }); }
  Outcome sched_del(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::schedule_del(sched, r, g); }); }
  Outcome timer_add(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::timer_add(reg, scenes, timers, r, g, 5000, 1000000); }); }
  Outcome scene_put(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::scene_put(reg, scenes, r, g); }); }
  Outcome scene_del(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::scene_del(scenes, r, g, [&](std::uint8_t id) { return busy_ids.count(id) != 0; }); }); }
  Outcome scene_apply(const std::string& a, bool accept = true) {
    return call(a, [&](Reader& r, int g) {
      return core::app::scene_apply(reg, scenes, r, g, [&](std::uint16_t e, std::uint8_t at, const Value&) {
        sent.push_back(std::to_string(e) + ":" + std::to_string(at));
        return accept;
      });
    });
  }
  Outcome timer_cancel(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::timer_cancel(timers, r, g); }); }
};

const char* kRule = R"({"when":{"entity":258,"attr":80,"op":"==","value":true},)"
                    R"("then":{"entity":257,"attr":1,"value":true},)"
                    R"("between":{"from":"18:00","to":"23:00"},)"
                    R"("and":{"entity":256,"attr":19,"op":">","value":1},"cooldown_s":300})";

}  // namespace

// ---- names ---------------------------------------------------------------------------

TEST_CASE("name.set names a device, replaces a name, and name.clear removes it") {
  Home h;
  auto o = h.name_set(R"({"entity":256,"name":"plug1"})");
  CHECK(o.ok);
  CHECK(std::string(h.reg.name_of(0x0100)) == "plug1");
  CHECK(h.name_set(R"({"entity":256,"name":"kitchen_2"})").ok);
  CHECK(std::string(h.reg.name_of(0x0100)) == "kitchen_2");
  CHECK(h.name_set(R"({"entity":256,"name":"kitchen_2"})").ok);  // same name again is fine
  CHECK(h.name_clear(R"({"entity":256})").ok);
  CHECK(h.reg.name_of(0x0100) == nullptr);
}

TEST_CASE("name.set rejects bad names, duplicates and unknown or chip entities") {
  Home h;
  REQUIRE(h.name_set(R"({"entity":256,"name":"plug1"})").ok);
  struct Case { const char* args; Err err; };
  const Case bad[] = {
      {R"({"entity":257,"name":"plug1"})", Err::BadArgs},                      // in use
      {R"({"entity":257,"name":"Plug"})", Err::BadArgs},                       // uppercase
      {R"({"entity":257,"name":"my plug"})", Err::BadArgs},                    // space
      {R"({"entity":257,"name":"1plug"})", Err::BadArgs},                      // starts with a digit
      {R"({"entity":257,"name":"_plug"})", Err::BadArgs},
      {R"({"entity":257,"name":"0x10"})", Err::BadArgs},                       // would look like an id
      {R"({"entity":257,"name":""})", Err::BadArgs},
      {R"({"entity":257,"name":"aaaaaaaaaaaaaaaaaaaaaaaaa"})", Err::BadArgs},  // 25 chars
      {R"({"entity":257,"name":7})", Err::BadArgs},
      {R"({"entity":257})", Err::BadArgs},
      {R"({"name":"x"})", Err::BadArgs},
      {R"({"entity":"a","name":"x"})", Err::BadArgs},
      {R"({"entity":99999,"name":"x"})", Err::BadArgs},
      {R"({"entity":4660,"name":"ghost"})", Err::NotFound},
      {R"({"entity":1,"name":"chipname"})", Err::NotFound},                    // chips are not devices
  };
  for (const auto& c : bad) {
    CAPTURE(c.args);
    const auto o = h.name_set(c.args);
    CHECK_FALSE(o.ok);
    CHECK(o.err == c.err);
  }
  CHECK(h.name_set(R"({"entity":257,"name":"aaaaaaaaaaaaaaaaaaaaaaa"})").ok);  // exactly 23
  CHECK(h.name_clear(R"({"entity":4660})").err == Err::NotFound);
  Reader r;  // a request with no args object at all
  CHECK(name_set(h.reg, r, -1).err == Err::BadArgs);
}

// ---- rules ---------------------------------------------------------------------------

TEST_CASE("rule.add builds the rule with every constraint") {
  Home h;
  const auto o = h.rule_add(kRule);
  REQUIRE(o.ok);
  CHECK((o.has_index && o.index == 0));
  REQUIRE(h.rules.rule_count() == 1);
  const Rule* r = h.rules.rule_at(0);
  CHECK((r->trigger_entity == 258 && r->trigger_attr == 0x50));
  CHECK(r->op == CmpOp::Eq);
  CHECK(r->operand == Value::make_bool(true));
  CHECK((r->action_entity == 257 && r->action_attr == 1));
  CHECK(r->action_value == Value::make_bool(true));
  CHECK((r->window_start == 18 * 60 && r->window_end == 23 * 60));
  CHECK(r->has_cond);
  CHECK((r->cond_entity == 256 && r->cond_attr == 0x13 && r->cond_op == CmpOp::Gt));
  CHECK(r->cond_operand == Value::make_u16(1));
  CHECK(r->cooldown_s == 300);
}

TEST_CASE("rule.add: a bare 'any' rule, extra fields ignored, chip telemetry as trigger") {
  Home h;
  auto o = h.rule_add(R"({"when":{"entity":1,"attr":113,"op":"any"},"then":{"entity":257,"attr":2,"value":200},"note":"hi"})");
  REQUIRE(o.ok);
  const Rule* r = h.rules.rule_at(0);
  CHECK(r->op == CmpOp::Always);
  CHECK(r->operand.type == ValueType::U32);  // typed after the trigger attribute
  CHECK(r->action_value == Value::make_u8(200));
  CHECK(r->window_start == kNoWindow);
  CHECK_FALSE(r->has_cond);
  CHECK(r->cooldown_s == 0);
  CHECK(h.rule_add(R"({"when":{"entity":258,"attr":80,"op":"any"},"then":{"entity":257,"attr":1,"value":false}})").index == 1);
}

TEST_CASE("rule.add rejects what the console would let through, and what makes no sense") {
  Home h;
  const std::string w = R"("when":{"entity":258,"attr":80,"op":"==","value":true})";
  const std::string t = R"("then":{"entity":257,"attr":1,"value":true})";
  struct Case { std::string args; Err err; };
  const std::vector<Case> bad = {
      {"{" + w + "}", Err::BadArgs},                                                      // no then
      {"{" + t + "}", Err::BadArgs},                                                      // no when
      {"{}", Err::BadArgs},
      {R"({"when":{"entity":258,"attr":80,"op":"==","value":1},)" + t + "}", Err::BadArgs},  // bool vs number
      {R"({"when":{"entity":258,"attr":80,"op":"==",)" + t.substr(0, 0) + R"("x":1},)" + t + "}", Err::BadArgs},  // no value
      {R"({"when":{"entity":258,"attr":80,"op":"=~","value":true},)" + t + "}", Err::BadArgs},
      {R"({"when":{"entity":4660,"attr":80,"op":"any"},)" + t + "}", Err::NotFound},
      {R"({"when":{"entity":258,"attr":99,"op":"any"},)" + t + "}", Err::NotFound},
      {"{" + w + R"(,"then":{"entity":4660,"attr":1,"value":true}})", Err::NotFound},
      {"{" + w + R"(,"then":{"entity":257,"attr":99,"value":true}})", Err::NotFound},
      {"{" + w + R"(,"then":{"entity":256,"attr":19,"value":5}})", Err::ReadOnly},           // power_w is read-only
      {"{" + w + R"(,"then":{"entity":259,"attr":1,"value":true}})", Err::ReadOnly},         // read-only on_off
      {"{" + w + R"(,"then":{"entity":1,"attr":113,"value":5}})", Err::NotFound},            // chips cannot be commanded
      {"{" + w + R"(,"then":{"entity":257,"attr":2,"value":300}})", Err::BadArgs},           // u8 range
      {"{" + w + "," + t + R"(,"between":{"from":"10:00","to":"10:00"}})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"between":{"from":"24:00","to":"10:00"}})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"between":{"from":"9:00","to":"10:00"}})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"between":{"from":"09:00"}})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"and":{"entity":256,"attr":19,"op":"any","value":1}})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"and":{"entity":256,"attr":19,"op":">","value":true}})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"and":{"entity":4660,"attr":19,"op":">","value":1}})", Err::NotFound},
      {"{" + w + "," + t + R"(,"cooldown_s":0})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"cooldown_s":65536})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"cooldown_s":"60"})", Err::BadArgs},
      {"{" + w + "," + t + R"(,"cooldown_s":1.5})", Err::BadArgs},
  };
  for (const auto& c : bad) {
    CAPTURE(c.args);
    const auto o = h.rule_add(c.args);
    CHECK_FALSE(o.ok);
    CHECK(o.err == c.err);
  }
  CHECK(h.rules.rule_count() == 0);  // nothing half-added
}

TEST_CASE("rule table limit and rule.del") {
  Home h;
  for (int i = 0; i < 4; ++i) REQUIRE(h.rule_add(kRule).ok);
  CHECK(h.rule_add(kRule).err == Err::TableFull);
  CHECK(h.rule_del(R"({"index":1})").ok);
  CHECK(h.rules.rule_count() == 3);
  CHECK(h.rule_del(R"({"index":3})").err == Err::NotFound);
  CHECK(h.rule_del(R"({"index":-1})").err == Err::BadArgs);
  CHECK(h.rule_del(R"({"index":"0"})").err == Err::BadArgs);
  CHECK(h.rule_del(R"({})").err == Err::BadArgs);
}

// ---- schedules -----------------------------------------------------------------------

TEST_CASE("schedule.add: days as a word, a list, or omitted; toggle is the U8 2 convention") {
  Home h;
  auto o = h.sched_add(R"({"time":"22:00","days":"weekdays","entity":256,"action":"off"})");
  REQUIRE(o.ok);
  CHECK(o.index == 0);
  const auto* s = h.sched.get(0);
  CHECK((s->entity_id == 256 && s->attr_id == 1 && s->minute_of_day == 22 * 60));
  CHECK(s->weekdays == kWeekdays);
  CHECK(s->value == Value::make_bool(false));

  REQUIRE(h.sched_add(R"({"time":"06:30","days":["mon","fri"],"entity":257,"action":"toggle"})").ok);
  CHECK(h.sched.get(1)->weekdays == ((1u << 1) | (1u << 5)));
  CHECK(h.sched.get(1)->value == Value::make_u8(2));

  REQUIRE(h.sched_add(R"({"time":"07:00","entity":257,"action":"on"})").ok);  // days omitted
  CHECK(h.sched.get(2)->weekdays == kEveryDay);
}

TEST_CASE("schedule.add and schedule.del reject bad input and reuse freed slots") {
  Home h;
  const char* good = R"("time":"22:00","days":"daily","entity":256,"action":"off")";
  struct Case { const char* args; Err err; };
  const Case bad[] = {
      {R"({"time":"24:00","days":"daily","entity":256,"action":"off"})", Err::BadArgs},
      {R"({"time":"9:30","days":"daily","entity":256,"action":"off"})", Err::BadArgs},
      {R"({"time":"22:60","days":"daily","entity":256,"action":"off"})", Err::BadArgs},
      {R"({"days":"daily","entity":256,"action":"off"})", Err::BadArgs},
      {R"({"time":"22:00","days":"someday","entity":256,"action":"off"})", Err::BadArgs},
      {R"({"time":"22:00","days":[],"entity":256,"action":"off"})", Err::BadArgs},
      {R"({"time":"22:00","days":["funday"],"entity":256,"action":"off"})", Err::BadArgs},
      {R"({"time":"22:00","days":5,"entity":256,"action":"off"})", Err::BadArgs},
      {R"({"time":"22:00","days":"daily","entity":256,"action":"dim"})", Err::BadArgs},
      {R"({"time":"22:00","days":"daily","entity":256})", Err::BadArgs},
      {R"({"time":"22:00","days":"daily","action":"off"})", Err::BadArgs},
      {R"({"time":"22:00","days":"daily","entity":258,"action":"off"})", Err::BadArgs},   // sensor: no on/off
      {R"({"time":"22:00","days":"daily","entity":259,"action":"off"})", Err::BadArgs},   // read-only on/off
      {R"({"time":"22:00","days":"daily","entity":1,"action":"off"})", Err::NotFound},    // chip
      {R"({"time":"22:00","days":"daily","entity":4660,"action":"off"})", Err::NotFound},
  };
  for (const auto& c : bad) {
    CAPTURE(c.args);
    const auto o = h.sched_add(c.args);
    CHECK_FALSE(o.ok);
    CHECK(o.err == c.err);
  }
  CHECK(h.sched.active_count() == 0);

  for (int i = 0; i < 4; ++i) REQUIRE(h.sched_add(std::string("{") + good + "}").ok);
  CHECK(h.sched_add(std::string("{") + good + "}").err == Err::TableFull);
  CHECK(h.sched_del(R"({"index":2})").ok);
  CHECK(h.sched_add(std::string("{") + good + "}").index == 2);  // the freed slot comes back
  CHECK(h.sched_del(R"({"index":9})").err == Err::NotFound);
  CHECK(h.sched_del(R"({"index":-1})").err == Err::BadArgs);
}

// ---- timers --------------------------------------------------------------------------

TEST_CASE("timer.add sets the delay, timer.cancel removes it") {
  Home h;
  auto o = h.timer_add(R"({"entity":256,"action":"off","delay_s":600})");
  REQUIRE(o.ok);
  CHECK(o.index == 0);
  const auto* t = h.timers.get(0);
  REQUIRE(t != nullptr);
  CHECK((t->entity_id == 256 && t->attr_id == 1));
  CHECK(t->value == Value::make_bool(false));
  CHECK(t->when_monotonic_ms == 5000u + 600000u);
  CHECK((t->has_wall_deadline && t->deadline_epoch_s == 1000000u + 600u));
  REQUIRE(h.timer_add(R"({"entity":257,"action":"toggle","delay_s":1})").ok);
  CHECK(h.timers.get(1)->value == Value::make_u8(2));
  CHECK(h.timer_cancel(R"({"index":0})").ok);
  CHECK(h.timers.get(0) == nullptr);
  CHECK(h.timer_cancel(R"({"index":0})").err == Err::NotFound);
  CHECK(h.timer_cancel(R"({"index":"x"})").err == Err::BadArgs);
}

TEST_CASE("timer.add rejects bad delays, actions and targets; the table has a limit") {
  Home h;
  struct Case { const char* args; Err err; };
  const Case bad[] = {
      {R"({"entity":256,"action":"on","delay_s":0})", Err::BadArgs},
      {R"({"entity":256,"action":"on","delay_s":-5})", Err::BadArgs},
      {R"({"entity":256,"action":"on","delay_s":604801})", Err::BadArgs},
      {R"({"entity":256,"action":"on","delay_s":1.5})", Err::BadArgs},
      {R"({"entity":256,"action":"on","delay_s":"60"})", Err::BadArgs},
      {R"({"entity":256,"action":"on"})", Err::BadArgs},
      {R"({"entity":256,"action":"blink","delay_s":5})", Err::BadArgs},
      {R"({"entity":258,"action":"on","delay_s":5})", Err::BadArgs},
      {R"({"entity":4660,"action":"on","delay_s":5})", Err::NotFound},
      {R"({"entity":1,"action":"on","delay_s":5})", Err::NotFound},
  };
  for (const auto& c : bad) {
    CAPTURE(c.args);
    const auto o = h.timer_add(c.args);
    CHECK_FALSE(o.ok);
    CHECK(o.err == c.err);
  }
  CHECK(h.timer_add(R"({"entity":256,"action":"on","delay_s":604800})").ok);  // the upper bound itself
  for (int i = 0; i < 3; ++i) REQUIRE(h.timer_add(R"({"entity":256,"action":"on","delay_s":10})").ok);
  CHECK(h.timer_add(R"({"entity":256,"action":"on","delay_s":10})").err == Err::TableFull);
}

TEST_CASE("encode_res_ok_index carries the new index") {
  char buf[96];
  JsonWriter w(buf, sizeof(buf));
  encode_res_ok_index(w, 8, 3);
  CHECK(std::string(w.c_str()) == R"({"type":"res","id":8,"ok":true,"result":{"index":3}})");
}

TEST_CASE("pair opens the Zigbee network for 1..254 seconds, 180 by default") {
  Home h;
  auto pair = [&](const std::string& a) {
    const std::string frame = R"({"type":"req","id":1,"op":"pair","args":)" + a + "}";
    Reader r;
    const ClientMsg m = decode_client(r, frame.data(), frame.size());
    REQUIRE(m.kind == ClientMsg::Kind::Req);
    return pair_meta(r, m.args, 1);
  };
  auto m = pair("{}");
  CHECK(m.out.ok);
  CHECK(m.chip == 1);
  CHECK(m.attr == kMetaPair);
  CHECK(m.arg == 180);
  m = pair(R"({"seconds":60})");
  CHECK((m.out.ok && m.arg == 60));
  CHECK(pair(R"({"seconds":1})").arg == 1);
  CHECK(pair(R"({"seconds":254})").arg == 254);
  for (const char* bad : {R"({"seconds":0})", R"({"seconds":255})", R"({"seconds":-3})", R"({"seconds":2.5})",
                          R"({"seconds":"60"})", R"({"seconds":true})", R"({"seconds":null})"}) {
    CAPTURE(bad);
    const auto o = pair(bad);
    CHECK_FALSE(o.out.ok);
    CHECK(o.out.err == Err::BadArgs);
  }
  // No args object at all is also "default".
  Reader r;
  const std::string frame = R"({"type":"req","id":1,"op":"pair"})";
  const ClientMsg msg = decode_client(r, frame.data(), frame.size());
  REQUIRE(msg.kind == ClientMsg::Kind::Req);
  CHECK(pair_meta(r, msg.args, 1).arg == 180);
}

TEST_CASE("unpair and identify target a Zigbee device only") {
  Home h;
  declare(h.reg, 2, 0x0200, hubproto::DeviceType::Switch, {{0x01, ValueType::Bool, 0x07}});  // a Matter-side mirror
  auto meta = [&](const std::string& a, std::uint8_t attr) {
    const std::string frame = R"({"type":"req","id":1,"op":"x","args":)" + a + "}";
    Reader r;
    const ClientMsg m = decode_client(r, frame.data(), frame.size());
    REQUIRE(m.kind == ClientMsg::Kind::Req);
    return device_meta(h.reg, r, m.args, attr, 1);
  };
  auto u = meta(R"({"entity":256})", kMetaUnpair);
  CHECK(u.out.ok);
  CHECK((u.chip == 1 && u.attr == kMetaUnpair && u.arg == 0x0100));
  auto i = meta(R"({"entity":257})", kMetaIdentify);
  CHECK((i.out.ok && i.attr == kMetaIdentify && i.arg == 0x0101));
  CHECK(meta(R"({"entity":4660})", kMetaUnpair).out.err == Err::NotFound);   // unknown
  CHECK(meta(R"({"entity":1})", kMetaIdentify).out.err == Err::NotFound);    // a chip is not a device
  CHECK(meta(R"({"entity":512})", kMetaUnpair).out.err == Err::BadArgs);     // lives on the Matter chip
  for (const char* bad : {"{}", R"({"entity":-1})", R"({"entity":65536})", R"({"entity":"256"})", R"({"entity":1.5})"}) {
    CAPTURE(bad);
    const auto o = meta(bad, kMetaUnpair);
    CHECK_FALSE(o.out.ok);
    CHECK(o.out.err == Err::BadArgs);
  }
  // No args object at all.
  Reader r;
  const std::string frame = R"({"type":"req","id":1,"op":"unpair"})";
  const ClientMsg m = decode_client(r, frame.data(), frame.size());
  REQUIRE(m.kind == ClientMsg::Kind::Req);
  CHECK(device_meta(h.reg, r, m.args, kMetaUnpair, 1).out.err == Err::BadArgs);
}

// ---- scenes ----------------------------------------------------------------------------

namespace {
const char* kEvening = R"({"name":"evening","actions":[)"
                       R"({"entity":257,"attr":1,"value":true},{"entity":257,"attr":2,"value":100},)"
                       R"({"entity":256,"attr":1,"value":false}]})";
}

TEST_CASE("scene.put stores a scene, scene.apply sends every action, scene.del removes it") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  const auto* s = h.scenes.find("evening");
  REQUIRE(s != nullptr);
  CHECK(s->count == 3);
  CHECK(s->actions[1].value.type == ValueType::U8);

  const auto o = h.scene_apply(R"({"name":"evening"})");
  CHECK(o.ok);
  CHECK(o.has_counts);
  CHECK(o.sent == 3);
  CHECK(o.skipped == 0);
  CHECK(h.sent == std::vector<std::string>{"257:1", "257:2", "256:1"});

  CHECK(h.scene_del(R"({"name":"evening"})").ok);
  CHECK(h.scene_del(R"({"name":"evening"})").err == Err::NotFound);
  CHECK(h.scene_apply(R"({"name":"evening"})").err == Err::NotFound);
}

TEST_CASE("scene.put replaces by name and rejects bad input with specific codes") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  CHECK(h.scene_put(R"({"name":"evening","actions":[{"entity":256,"attr":1,"value":true}]})").ok);
  CHECK(h.scenes.find("evening")->count == 1);
  CHECK(h.scenes.active_count() == 1);

  struct Case { const char* args; Err err; };
  const Case bad[] = {
      {R"({"actions":[{"entity":256,"attr":1,"value":true}]})", Err::BadArgs},                  // no name
      {R"({"name":"Evening","actions":[{"entity":256,"attr":1,"value":true}]})", Err::BadArgs},  // bad name
      {R"({"name":"x"})", Err::BadArgs},                                                          // no actions
      {R"({"name":"x","actions":[]})", Err::BadArgs},
      {R"({"name":"x","actions":{"entity":256}})", Err::BadArgs},
      {R"({"name":"x","actions":[{"entity":256,"attr":1,"value":true},{"entity":256,"attr":1,"value":false}]})", Err::BadArgs},  // twice
      {R"({"name":"x","actions":[{"entity":256,"attr":1,"value":5}]})", Err::BadArgs},          // wrong type
      {R"({"name":"x","actions":[{"entity":257,"attr":2,"value":300}]})", Err::BadArgs},        // out of range
      {R"({"name":"x","actions":[{"entity":256,"attr":19,"value":1}]})", Err::ReadOnly},        // power is read-only
      {R"({"name":"x","actions":[{"entity":259,"attr":1,"value":true}]})", Err::ReadOnly},      // read-only on/off
      {R"({"name":"x","actions":[{"entity":4660,"attr":1,"value":true}]})", Err::NotFound},
      {R"({"name":"x","actions":[{"entity":1,"attr":113,"value":1}]})", Err::NotFound},         // a chip is not a device
      {R"({"name":"x","actions":[{"entity":256,"attr":99,"value":true}]})", Err::NotFound},
      {R"({"name":"x","actions":[{"entity":256,"attr":1,"value":true},{"entity":257,"attr":1,"value":true},{"entity":257,"attr":2,"value":1},{"entity":258,"attr":80,"value":true},{"entity":256,"attr":19,"value":1}]})", Err::BadArgs},  // too many (5 > 4)
  };
  for (const auto& c : bad) {
    CAPTURE(c.args);
    const auto o = h.scene_put(c.args);
    CHECK_FALSE(o.ok);
    CHECK(o.err == c.err);
  }
  CHECK(h.scenes.find("x") == nullptr);        // nothing half-stored
  CHECK(h.scenes.find("evening")->count == 1); // and the existing one is untouched

  const char* one = R"("actions":[{"entity":256,"attr":1,"value":true}]})";
  REQUIRE(h.scene_put(std::string(R"({"name":"a",)") + one).ok);
  REQUIRE(h.scene_put(std::string(R"({"name":"b",)") + one).ok);
  CHECK(h.scene_put(std::string(R"({"name":"c",)") + one).err == Err::TableFull);
}

TEST_CASE("scene.apply skips actions whose target went away, and counts what the dispatcher refused") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  h.reg.clear_chip(1);  // every device on chip 1 vanishes
  auto o = h.scene_apply(R"({"name":"evening"})");
  CHECK(o.ok);
  CHECK(o.sent == 0);
  CHECK(o.skipped == 3);
  CHECK(h.sent.empty());

  Home g;
  REQUIRE(g.scene_put(kEvening).ok);
  o = g.scene_apply(R"({"name":"evening"})", /*accept=*/false);  // the dispatcher queue is full
  CHECK(o.ok);
  CHECK(o.sent == 0);
  CHECK(o.skipped == 3);
  CHECK(g.sent.size() == 3);  // each was offered
}

TEST_CASE("scene.apply refuses a missing or malformed name") {
  Home h;
  CHECK(h.scene_apply("{}").err == Err::BadArgs);
  CHECK(h.scene_apply(R"({"name":5})").err == Err::BadArgs);
  CHECK(h.scene_apply(R"({"name":"nope"})").err == Err::NotFound);
}

TEST_CASE("write_scene and encode_res_ok_counts") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  char buf[512];
  JsonWriter w(buf, sizeof(buf));
  core::app::write_scene(w, *h.scenes.find("evening"));
  CHECK(std::string(w.c_str()) ==
        R"({"name":"evening","actions":[{"entity":257,"attr":1,"value":true},)"
        R"({"entity":257,"attr":2,"value":100},{"entity":256,"attr":1,"value":false}]})");
  JsonWriter r(buf, sizeof(buf));
  core::app::encode_res_ok_counts(r, 4, 2, 1);
  CHECK(std::string(r.c_str()) == R"({"type":"res","id":4,"ok":true,"result":{"sent":2,"skipped":1}})");
}

// ---- schedules, timers and rules that apply a scene ------------------------------------

TEST_CASE("a schedule or timer can apply a scene instead of switching one device") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  const std::uint8_t id = h.scenes.find("evening")->id;

  auto o = h.sched_add(R"({"time":"22:00","days":"weekdays","scene":"evening"})");
  REQUIRE(o.ok);
  const auto* s = h.sched.get(static_cast<std::size_t>(o.index));
  REQUIRE(s != nullptr);
  CHECK(s->entity_id == core::kSceneTarget);
  CHECK(s->attr_id == id);
  CHECK(s->minute_of_day == 22 * 60);

  o = h.timer_add(R"({"delay_s":60,"scene":"evening"})");
  REQUIRE(o.ok);
  const auto* t = h.timers.get(static_cast<std::size_t>(o.index));
  REQUIRE(t != nullptr);
  CHECK(t->entity_id == core::kSceneTarget);
  CHECK(t->attr_id == id);
}

TEST_CASE("a scene target is refused when ambiguous, unknown or malformed") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  struct Case { const char* args; Err err; };
  const Case bad[] = {
      {R"({"time":"22:00","scene":"evening","entity":256})", Err::BadArgs},
      {R"({"time":"22:00","scene":"evening","action":"on"})", Err::BadArgs},
      {R"({"time":"22:00","scene":"nope"})", Err::NotFound},
      {R"({"time":"22:00","scene":"Bad Name"})", Err::BadArgs},
      {R"({"time":"22:00","scene":5})", Err::BadArgs},
      {R"({"time":"22:00"})", Err::BadArgs},  // neither
  };
  for (const auto& c : bad) {
    CAPTURE(c.args);
    const auto o = h.sched_add(c.args);
    CHECK_FALSE(o.ok);
    CHECK(o.err == c.err);
  }
  CHECK(h.sched.active_count() == 0);
  CHECK(h.timer_add(R"({"delay_s":5,"scene":"nope"})").err == Err::NotFound);
  CHECK(h.timers.active_count() == 0);
}

TEST_CASE("a rule can apply a scene") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  const std::uint8_t id = h.scenes.find("evening")->id;
  const auto o = h.rule_add(R"({"when":{"entity":258,"attr":80,"op":"==","value":true},"then":{"scene":"evening"},"cooldown_s":30})");
  REQUIRE(o.ok);
  const auto* r = h.rules.rule_at(static_cast<std::size_t>(o.index));
  REQUIRE(r != nullptr);
  CHECK(r->action_entity == core::kSceneTarget);
  CHECK(r->action_attr == id);

  for (const char* bad : {R"({"when":{"entity":258,"attr":80,"op":"any"},"then":{"scene":"nope"}})",
                          R"({"when":{"entity":258,"attr":80,"op":"any"},"then":{"scene":"evening","entity":257}})",
                          R"({"when":{"entity":258,"attr":80,"op":"any"},"then":{"scene":"evening","value":true}})"}) {
    CAPTURE(bad);
    CHECK_FALSE(h.rule_add(bad).ok);
  }
  CHECK(h.rules.rule_count() == 1);
}

TEST_CASE("a scene that something uses cannot be deleted") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  h.busy_ids.insert(h.scenes.find("evening")->id);
  const auto o = h.scene_del(R"({"name":"evening"})");
  CHECK_FALSE(o.ok);
  CHECK(o.err == Err::BadArgs);
  CHECK(h.scenes.find("evening") != nullptr);
  h.busy_ids.clear();
  CHECK(h.scene_del(R"({"name":"evening"})").ok);
}

TEST_CASE("schedules, timers and rules that apply a scene are written with the scene's name") {
  Home h;
  REQUIRE(h.scene_put(kEvening).ok);
  REQUIRE(h.sched_add(R"({"time":"22:00","scene":"evening"})").ok);
  REQUIRE(h.rule_add(R"({"when":{"entity":258,"attr":80,"op":"any"},"then":{"scene":"evening"}})").ok);
  char buf[512];
  JsonWriter w(buf, sizeof(buf));
  core::app::write_schedule(w, 0, *h.sched.get(0), "evening");
  CHECK(std::string(w.c_str()) == R"({"index":0,"time":"22:00","days":"daily","scene":"evening"})");
  JsonWriter t(buf, sizeof(buf));
  core::app::write_timer(t, core::app::TimerView{1, core::kSceneTarget, 4, Value::make_bool(true), 30, "evening"});
  CHECK(std::string(t.c_str()) == R"({"index":1,"scene":"evening","left_s":30})");
  JsonWriter ru(buf, sizeof(buf));
  core::app::write_rule(ru, 0, *h.rules.rule_at(0), "evening");
  CHECK(std::string(ru.c_str()).find(R"("then":{"scene":"evening"})") != std::string::npos);
  JsonWriter gone(buf, sizeof(buf));
  core::app::write_rule(gone, 0, *h.rules.rule_at(0), nullptr);
  CHECK(std::string(gone.c_str()).find(R"("then":{"scene":"?"})") != std::string::npos);
}

// ---- sun-relative times ---------------------------------------------------------------------

TEST_CASE("schedule.add accepts sunrise/sunset times with an offset, and writes them back the same way") {
  Home h;
  for (const char* t : {"sunset", "sunset+30", "sunrise-15", "sunrise+180"}) {
    CAPTURE(t);
    const auto o = h.sched_add(std::string(R"({"time":")") + t + R"(","days":"daily","entity":256,"action":"on"})");
    REQUIRE(o.ok);
    char buf[160];
    JsonWriter w(buf, sizeof(buf));
    core::app::write_schedule(w, 0, *h.sched.get(static_cast<std::size_t>(o.index)));
    CHECK(std::string(w.c_str()).find(std::string(R"("time":")") + t + "\"") != std::string::npos);
    CHECK(h.sched_del(std::string(R"({"index":)") + std::to_string(o.index) + "}").ok);
  }
  for (const char* bad : {"sunset+181", "sunrise-181", "sunset+", "Sunset", "noon", "25:00", "sunset 30"}) {
    CAPTURE(bad);
    const auto o = h.sched_add(std::string(R"({"time":")") + bad + R"(","entity":256,"action":"on"})");
    CHECK_FALSE(o.ok);
    CHECK(o.err == Err::BadArgs);
  }
  CHECK(h.sched.active_count() == 0);
}

TEST_CASE("a rule window can start or end at the sun") {
  Home h;
  const std::string base = R"({"when":{"entity":258,"attr":80,"op":"any"},"then":{"entity":257,"attr":1,"value":true},"between":)";
  auto o = h.rule_add(base + R"({"from":"sunset","to":"23:00"}})");
  REQUIRE(o.ok);
  const auto* r = h.rules.rule_at(static_cast<std::size_t>(o.index));
  CHECK(r->window_start == core::solar_point(true, 0));
  CHECK(r->window_end == 23 * 60);
  char buf[512];
  JsonWriter w(buf, sizeof(buf));
  core::app::write_rule(w, 0, *r);
  CHECK(std::string(w.c_str()).find(R"("between":{"from":"sunset","to":"23:00"})") != std::string::npos);

  CHECK(h.rule_add(base + R"({"from":"sunset+30","to":"sunrise-15"}})").ok);
  for (const char* bad : {R"({"from":"sunset","to":"sunset"}})", R"({"from":"sunset+999","to":"23:00"}})",
                          R"({"from":"dusk","to":"23:00"}})", R"({"from":"sunset"}})"}) {
    CAPTURE(bad);
    CHECK_FALSE(h.rule_add(base + bad).ok);
  }
  CHECK(h.rules.rule_count() == 2);
}

TEST_CASE("location.set takes numeric lat/lon in range and nothing else") {
  auto parse = [](const std::string& args, double& lat, double& lon) {
    const std::string frame = R"({"type":"req","id":1,"op":"location.set","args":)" + args + "}";
    Reader r;
    const ClientMsg m = decode_client(r, frame.data(), frame.size());
    REQUIRE(m.kind == ClientMsg::Kind::Req);
    return core::app::location_parse(r, m.args, lat, lon);
  };
  double lat = 0, lon = 0;
  CHECK(parse(R"({"lat":50.08,"lon":14.43})", lat, lon).ok);
  CHECK(lat == doctest::Approx(50.08));
  CHECK(lon == doctest::Approx(14.43));
  CHECK(parse(R"({"lat":-90,"lon":180})", lat, lon).ok);   // the limits themselves
  CHECK(parse(R"({"lat":90,"lon":-180})", lat, lon).ok);
  for (const char* bad : {R"({})", R"({"lat":50})", R"({"lon":14})", R"({"lat":"50","lon":14})", R"({"lat":null,"lon":14})",
                          R"({"lat":90.1,"lon":0})", R"({"lat":-91,"lon":0})", R"({"lat":0,"lon":180.5})", R"({"lat":0,"lon":-181})",
                          R"({"lat":true,"lon":1})", R"({"lat":1e999,"lon":1})"}) {
    CAPTURE(bad);
    const auto o = parse(bad, lat, lon);
    CHECK_FALSE(o.ok);
    CHECK(o.err == Err::BadArgs);
  }
}

TEST_CASE("wifi.set needs a valid SSID and password and refuses anything else with a reason") {
  auto parse = [](const std::string& args, std::string& ssid_out, std::string& pass_out) {
    const std::string frame = R"({"type":"req","id":1,"op":"wifi.set","args":)" + args + "}";
    Reader r;
    const ClientMsg m = decode_client(r, frame.data(), frame.size());
    REQUIRE(m.kind == ClientMsg::Kind::Req);
    char ssid[64] = {}, pass[96] = {};
    const auto o = core::app::wifi_parse(r, m.args, ssid, sizeof(ssid), pass, sizeof(pass));
    ssid_out = ssid;
    pass_out = pass;
    return o;
  };
  std::string s, p;
  CHECK(parse(R"({"ssid":"home","password":"correct horse"})", s, p).ok);
  CHECK(s == "home");
  CHECK(p == "correct horse");
  CHECK(parse(R"({"ssid":"cafe","password":""})", s, p).ok);  // open network
  CHECK(parse(R"({"ssid":"Тест","password":"12345678"})", s, p).ok);
  for (const char* bad : {R"({})", R"({"ssid":"home"})", R"({"password":"12345678"})", R"({"ssid":"","password":"12345678"})",
                          R"({"ssid":"home","password":"short"})", R"({"ssid":5,"password":"12345678"})",
                          R"({"ssid":"home","password":null})", R"({"ssid":"a\nb","password":"12345678"})",
                          R"({"ssid":"home","password":"123456789012345678901234567890123456789012345678901234567890123456"})"}) {
    CAPTURE(bad);
    const auto o = parse(bad, s, p);
    CHECK_FALSE(o.ok);
    CHECK(o.err == Err::BadArgs);
  }
}
