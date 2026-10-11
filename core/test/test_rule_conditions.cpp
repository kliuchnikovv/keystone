#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "core/rule_engine.hpp"
#include "core/endian.hpp"
#include "hubproto/payload.hpp"

namespace {

using hubproto::Value;

struct FakeDisp {
  int sent = 0;
  std::uint16_t entity = 0;
  std::uint8_t attr = 0;
  Value value{};
  bool send(std::uint16_t e, std::uint8_t a, const Value& v, std::uint32_t /*now*/,
            std::uint16_t& seq) {
    ++sent;
    entity = e;
    attr = a;
    value = v;
    seq = static_cast<std::uint16_t>(sent);
    return true;
  }
};

using Engine = core::RuleEngine<FakeDisp, 4>;

struct Env {
  std::uint32_t now_ms = 1000;
  int minute = 600;  // 10:00; -1 = clock not synced
  bool have_cond_state = true;
  Value cond_state = Value::make_bool(true);
};

std::uint32_t now_fn(void* c) { return static_cast<Env*>(c)->now_ms; }
int minute_fn(void* c) { return static_cast<Env*>(c)->minute; }
bool read_fn(void* c, std::uint16_t, std::uint8_t, Value& out) {
  auto* e = static_cast<Env*>(c);
  if (!e->have_cond_state) return false;
  out = e->cond_state;
  return true;
}

core::Rule base_rule() {
  core::Rule r{};
  r.trigger_entity = 0x10;
  r.trigger_attr = 0x50;
  r.match = nullptr;
  r.op = core::CmpOp::Eq;
  r.operand = Value::make_bool(true);
  r.action_entity = 0x20;
  r.action_attr = 0x01;
  r.action_value = Value::make_bool(true);
  return r;
}

void trigger(Engine& e) {
  Engine::on_state_change(&e, 0x10, 0x50, Value::make_bool(true));
}

struct Fixture {
  FakeDisp disp;
  Env env;
  Engine eng{disp, &now_fn, &env};
  Fixture() {
    eng.set_clock(&minute_fn, &env);
    eng.set_state_reader(&read_fn, &env);
  }
};

}  // namespace

TEST_CASE("a rule without constraints behaves as before") {
  Fixture f;
  REQUIRE(f.eng.add_rule(base_rule()));
  f.env.minute = -1;  // clock/state must not matter
  f.env.have_cond_state = false;
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
}

TEST_CASE("time window: inside fires, outside and unknown clock do not") {
  Fixture f;
  core::Rule r = base_rule();
  r.window_start = 18 * 60;
  r.window_end = 23 * 60;
  REQUIRE(f.eng.add_rule(r));

  f.env.minute = 17 * 60 + 59;
  trigger(f.eng);
  CHECK(f.disp.sent == 0);
  f.env.minute = 18 * 60;  // start is inclusive
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
  f.env.minute = 23 * 60;  // end is exclusive
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
  f.env.minute = -1;  // clock not synced: stay quiet
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
}

TEST_CASE("time window may wrap midnight") {
  Fixture f;
  core::Rule r = base_rule();
  r.window_start = 22 * 60;
  r.window_end = 6 * 60;
  REQUIRE(f.eng.add_rule(r));
  for (int minute : {22 * 60, 23 * 60 + 59, 0, 5 * 60 + 59}) {
    f.env.minute = minute;
    const int before = f.disp.sent;
    trigger(f.eng);
    CHECK(f.disp.sent == before + 1);
  }
  for (int minute : {6 * 60, 12 * 60, 21 * 60 + 59}) {
    f.env.minute = minute;
    const int before = f.disp.sent;
    trigger(f.eng);
    CHECK(f.disp.sent == before);
  }
}

TEST_CASE("extra condition reads the current state of another attribute") {
  Fixture f;
  core::Rule r = base_rule();
  r.has_cond = true;
  r.cond_entity = 0x30;
  r.cond_attr = 0x01;
  r.cond_op = core::CmpOp::Eq;
  r.cond_operand = Value::make_bool(true);
  REQUIRE(f.eng.add_rule(r));

  trigger(f.eng);
  CHECK(f.disp.sent == 1);  // cond holds
  f.env.cond_state = Value::make_bool(false);
  trigger(f.eng);
  CHECK(f.disp.sent == 1);  // cond false
  f.env.have_cond_state = false;
  f.env.cond_state = Value::make_bool(true);
  trigger(f.eng);
  CHECK(f.disp.sent == 1);  // state unknown: quiet
}

TEST_CASE("extra condition without a state reader never fires") {
  FakeDisp disp;
  Env env;
  Engine eng{disp, &now_fn, &env};
  core::Rule r = base_rule();
  r.has_cond = true;
  r.cond_entity = 0x30;
  r.cond_attr = 0x01;
  r.cond_op = core::CmpOp::Always;
  REQUIRE(eng.add_rule(r));
  trigger(eng);
  CHECK(disp.sent == 0);
}

TEST_CASE("cool-down suppresses repeats and only starts when the rule fired") {
  Fixture f;
  core::Rule r = base_rule();
  r.cooldown_s = 60;
  r.has_cond = true;
  r.cond_entity = 0x30;
  r.cond_attr = 0x01;
  r.cond_op = core::CmpOp::Eq;
  r.cond_operand = Value::make_bool(true);
  REQUIRE(f.eng.add_rule(r));

  // Blocked by the condition: must not start the cool-down.
  f.env.cond_state = Value::make_bool(false);
  trigger(f.eng);
  CHECK(f.disp.sent == 0);
  f.env.cond_state = Value::make_bool(true);

  f.env.now_ms = 10000;
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
  f.env.now_ms = 10000 + 59999;
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
  f.env.now_ms = 10000 + 60000;
  trigger(f.eng);
  CHECK(f.disp.sent == 2);
}

TEST_CASE("cool-down survives the millisecond counter wrapping") {
  Fixture f;
  core::Rule r = base_rule();
  r.cooldown_s = 10;
  REQUIRE(f.eng.add_rule(r));
  f.env.now_ms = 0xFFFFFFFFu - 2000u;
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
  f.env.now_ms = 5000;  // 7 s later across the wrap
  trigger(f.eng);
  CHECK(f.disp.sent == 1);
  f.env.now_ms = 9000;  // 11 s later
  trigger(f.eng);
  CHECK(f.disp.sent == 2);
}

TEST_CASE("removing a rule keeps the other rules' cool-downs aligned") {
  Fixture f;
  core::Rule a = base_rule();
  a.trigger_attr = 0x51;  // never triggered here
  core::Rule b = base_rule();
  b.cooldown_s = 60;
  REQUIRE(f.eng.add_rule(a));
  REQUIRE(f.eng.add_rule(b));
  f.env.now_ms = 1000;
  trigger(f.eng);
  REQUIRE(f.disp.sent == 1);  // b fired, cool-down running
  REQUIRE(f.eng.remove_rule(0));  // b becomes #0
  f.env.now_ms = 2000;
  trigger(f.eng);
  CHECK(f.disp.sent == 1);  // still cooling down
}

TEST_CASE("constraints round-trip through save/load") {
  FakeDisp disp;
  Env env;
  Engine a{disp, &now_fn, &env};
  core::Rule r = base_rule();
  r.window_start = 22 * 60;
  r.window_end = 6 * 60;
  r.cooldown_s = 300;
  r.has_cond = true;
  r.cond_entity = 0x30;
  r.cond_attr = 0x01;
  r.cond_op = core::CmpOp::Ne;
  r.cond_operand = Value::make_bool(false);
  REQUIRE(a.add_rule(r));
  REQUIRE(a.add_rule(base_rule()));

  std::uint8_t buf[256];
  const std::size_t n = a.save(buf, sizeof(buf));
  REQUIRE(n == Engine::kHeaderSize + 2 * Engine::kEntryWireSize);

  Engine b{disp, &now_fn, &env};
  REQUIRE(b.load(buf, n));
  REQUIRE(b.rule_count() == 2);
  const core::Rule* r0 = b.rule_at(0);
  CHECK(r0->window_start == 22 * 60);
  CHECK(r0->window_end == 6 * 60);
  CHECK(r0->cooldown_s == 300);
  CHECK(r0->has_cond);
  CHECK(r0->cond_entity == 0x30);
  CHECK(r0->cond_attr == 0x01);
  CHECK(r0->cond_op == core::CmpOp::Ne);
  CHECK(r0->cond_operand == Value::make_bool(false));
  const core::Rule* r1 = b.rule_at(1);
  CHECK(r1->window_start == core::kNoWindow);
  CHECK(r1->cooldown_s == 0);
  CHECK_FALSE(r1->has_cond);
}

TEST_CASE("a v1 blob (rules stored by older firmware) still loads, without constraints") {
  // Header: magic 'UR', version 1, reserved, count=1; one 19-byte entry.
  std::uint8_t buf[Engine::kHeaderSize + Engine::kEntryWireSizeV1] = {};
  core::detail::put_u16_le(buf, Engine::kMagic);
  buf[2] = 1;
  core::detail::put_u16_le(buf + 4, 1);
  std::uint8_t* e = buf + Engine::kHeaderSize;
  core::detail::put_u16_le(e, 0x10);          // trigger entity
  e[2] = 0x50;                                // trigger attr
  e[3] = static_cast<std::uint8_t>(core::CmpOp::Eq);
  e[4] = static_cast<std::uint8_t>(hubproto::ValueType::Bool);  // operand
  e[5] = 1;
  e[6] = 1;
  core::detail::put_u16_le(e + 10, 0x20);     // action entity
  e[12] = 0x01;                               // action attr
  e[13] = static_cast<std::uint8_t>(hubproto::ValueType::Bool);  // action value
  e[14] = 1;
  e[15] = 0;

  FakeDisp disp;
  Env env;
  Engine eng{disp, &now_fn, &env};
  REQUIRE(eng.load(buf, sizeof(buf)));
  REQUIRE(eng.rule_count() == 1);
  const core::Rule* r = eng.rule_at(0);
  CHECK(r->trigger_entity == 0x10);
  CHECK(r->action_entity == 0x20);
  CHECK(r->window_start == core::kNoWindow);
  CHECK(r->cooldown_s == 0);
  CHECK_FALSE(r->has_cond);
}

TEST_CASE("v2 load rejects bad constraints and leaves the rules untouched") {
  FakeDisp disp;
  Env env;
  Engine a{disp, &now_fn, &env};
  core::Rule r = base_rule();
  r.window_start = 60;
  r.window_end = 120;
  r.has_cond = true;
  r.cond_entity = 0x30;
  r.cond_attr = 0x01;
  r.cond_op = core::CmpOp::Eq;
  r.cond_operand = Value::make_bool(true);
  REQUIRE(a.add_rule(r));
  std::uint8_t good[128];
  const std::size_t n = a.save(good, sizeof(good));
  REQUIRE(n > 0);

  Engine b{disp, &now_fn, &env};
  REQUIRE(b.add_rule(base_rule()));
  REQUIRE(b.add_rule(base_rule()));

  const std::size_t e = Engine::kHeaderSize;  // first entry offset
  std::uint8_t bad[128];

  std::memcpy(bad, good, n);
  core::detail::put_u16_le(bad + e + 21, 1440);  // window_end out of range
  CHECK_FALSE(b.load(bad, n));

  std::memcpy(bad, good, n);
  core::detail::put_u16_le(bad + e + 21, 60);  // start == end
  CHECK_FALSE(b.load(bad, n));

  std::memcpy(bad, good, n);
  bad[e + 25] = 2;  // has_cond not a bool
  CHECK_FALSE(b.load(bad, n));

  std::memcpy(bad, good, n);
  bad[e + 29] = 99;  // unknown cond op
  CHECK_FALSE(b.load(bad, n));

  std::memcpy(bad, good, n);
  bad[e + 30] = 0x7E;  // unknown cond value type
  CHECK_FALSE(b.load(bad, n));

  CHECK_FALSE(b.load(good, n - 1));  // truncated
  CHECK(b.rule_count() == 2);        // nothing changed

  CHECK(b.load(good, n));
  CHECK(b.rule_count() == 1);
}

namespace {
struct Hook {
  int calls = 0;
  std::uint16_t entity = 0;
  std::uint8_t attr = 0;
  bool handle = true;
};
bool hook_fn(void* c, std::uint16_t e, std::uint8_t a, const Value&, std::uint32_t) {
  auto* h = static_cast<Hook*>(c);
  ++h->calls;
  h->entity = e;
  h->attr = a;
  return h->handle;
}
}  // namespace

TEST_CASE("the action hook can take over an action; without one, or when it declines, the engine dispatches") {
  Fixture f;
  Hook hook;
  f.eng.set_action_hook(&hook_fn, &hook);
  core::Rule r = base_rule();
  r.action_entity = 0xFFFE;  // a scene target
  r.action_attr = 3;
  REQUIRE(f.eng.add_rule(r));
  trigger(f.eng);
  CHECK(hook.calls == 1);
  CHECK(hook.entity == 0xFFFE);
  CHECK(hook.attr == 3);
  CHECK(f.disp.sent == 0);  // handled: nothing sent

  hook.handle = false;  // declined: normal dispatch
  f.env.now_ms += 1000;
  trigger(f.eng);
  CHECK(hook.calls == 2);
  CHECK(f.disp.sent == 1);
}

TEST_CASE("a handled action still starts the cool-down") {
  Fixture f;
  Hook hook;
  f.eng.set_action_hook(&hook_fn, &hook);
  core::Rule r = base_rule();
  r.cooldown_s = 60;
  REQUIRE(f.eng.add_rule(r));
  trigger(f.eng);
  trigger(f.eng);  // inside the cool-down
  CHECK(hook.calls == 1);
}
