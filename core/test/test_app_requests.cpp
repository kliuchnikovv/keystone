#include <doctest/doctest.h>

#include <array>
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
  Outcome rule_add(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::rule_add(reg, rules, r, g); }); }
  Outcome rule_del(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::rule_del(rules, r, g); }); }
  Outcome sched_add(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::schedule_add(reg, sched, r, g); }); }
  Outcome sched_del(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::schedule_del(sched, r, g); }); }
  Outcome timer_add(const std::string& a) { return call(a, [&](Reader& r, int g) { return core::app::timer_add(reg, timers, r, g, 5000, 1000000); }); }
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
