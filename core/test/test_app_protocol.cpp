#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/app_protocol.hpp"
#include "core/entity_registry.hpp"

using namespace core;
using namespace core::app;
using hubproto::Value;
using hubproto::ValueType;

namespace {

using BigReader = BasicJsonReader<4096>;  // the hub's own frames are bigger than client messages

ClientMsg decode(const std::string& s, JsonReader& r) { return decode_client(r, s.data(), s.size()); }
ClientMsg decode(const std::string& s) {
  JsonReader r;
  return decode(s, r);
}

using Registry = EntityRegistry<>;

void declare(Registry& reg, std::uint8_t chip, std::uint16_t id, hubproto::DeviceType dt,
             std::initializer_list<hubproto::AttrDecl> attrs) {
  std::vector<hubproto::AttrDecl> a(attrs);
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_declare(id, dt, a.data(), static_cast<std::uint8_t>(a.size()), buf.data(), buf.size());
  REQUIRE(r.ok);
  hubproto::DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  REQUIRE(reg.declare(chip, v));
}

}  // namespace

// ---- decoding ------------------------------------------------------------------

TEST_CASE("decode_client: hello") {
  auto m = decode(R"({"type":"hello","v":1,"token":"s3cr3t","client":"app/0.1"})");
  CHECK(m.kind == ClientMsg::Kind::Hello);
  CHECK(m.v == 1);
  CHECK(m.token_ok);
  CHECK(std::string(m.token) == "s3cr3t");
  CHECK(std::string(m.client) == "app/0.1");

  m = decode(R"({"type":"hello","v":1})");  // no token
  CHECK(m.kind == ClientMsg::Kind::Hello);
  CHECK_FALSE(m.token_ok);

  m = decode(R"({"type":"hello","v":1,"token":123})");  // not a string
  CHECK_FALSE(m.token_ok);

  m = decode(std::string(R"({"type":"hello","v":1,"token":")") + std::string(65, 'x') + "\"}");  // too long
  CHECK_FALSE(m.token_ok);
  m = decode(std::string(R"({"type":"hello","v":1,"token":")") + std::string(64, 'x') + "\"}");  // exactly the limit
  CHECK(m.token_ok);

  m = decode(R"({"type":"hello","token":"x"})");  // no version
  CHECK(m.v == 0);
}

TEST_CASE("decode_client: cmd carries ids and a raw scalar") {
  auto m = decode(R"({"type":"cmd","id":7,"entity":256,"attr":1,"value":false})");
  CHECK(m.kind == ClientMsg::Kind::Cmd);
  CHECK((m.has_id && m.id == 7));
  CHECK((m.has_entity && m.entity == 256));
  CHECK((m.has_attr && m.attr == 1));
  CHECK(m.value.kind == Scalar::Kind::Bool);
  CHECK_FALSE(m.value.b);

  m = decode(R"({"type":"cmd","id":1,"entity":1,"attr":3,"value":[100,200]})");
  CHECK(m.value.kind == Scalar::Kind::Pair);
  CHECK((m.value.x == 100 && m.value.y == 200));
  m = decode(R"({"type":"cmd","id":1,"entity":1,"attr":2,"value":1.5})");
  CHECK(m.value.kind == Scalar::Kind::Float);
  m = decode(R"({"type":"cmd","id":1,"entity":1,"attr":2,"value":"x"})");
  CHECK(m.value.kind == Scalar::Kind::None);
  m = decode(R"({"type":"cmd","entity":1,"attr":2,"value":1})");  // no id
  CHECK_FALSE(m.has_id);
  m = decode(R"({"type":"cmd","id":-1,"entity":1,"attr":2,"value":1})");
  CHECK_FALSE(m.has_id);
  m = decode(R"({"type":"cmd","id":2147483648,"entity":1,"attr":2,"value":1})");
  CHECK_FALSE(m.has_id);
  m = decode(R"({"type":"cmd","id":1,"entity":"a","attr":2,"value":1})");
  CHECK_FALSE(m.has_entity);
}

TEST_CASE("decode_client: req exposes op and args to the caller") {
  JsonReader r;
  const std::string text = R"({"type":"req","id":8,"op":"name.set","args":{"entity":256,"name":"kitchen"}})";
  auto m = decode(text, r);
  CHECK(m.kind == ClientMsg::Kind::Req);
  CHECK(std::string(m.op) == "name.set");
  REQUIRE(m.args >= 0);
  char name[16];
  REQUIRE(r.get_string(r.find(m.args, "name"), name, sizeof(name)));
  CHECK(std::string(name) == "kitchen");
  CHECK(decode(R"({"type":"req","id":1,"op":"reboot"})").args == -1);
  CHECK(std::string(decode(R"({"type":"req","id":1})").op).empty());
}

TEST_CASE("decode_client: unknown and invalid frames") {
  CHECK(decode(R"({"type":"subscribe"})").kind == ClientMsg::Kind::Unknown);
  CHECK(decode(R"({"type":42})").kind == ClientMsg::Kind::Unknown);
  CHECK(decode(R"({"type":"averyveryverylongtype"})").kind == ClientMsg::Kind::Unknown);
  CHECK(decode(R"({"id":1})").kind == ClientMsg::Kind::Invalid);
  CHECK(decode("[]").kind == ClientMsg::Kind::Invalid);
  CHECK(decode("not json").kind == ClientMsg::Kind::Invalid);
  CHECK(decode("").kind == ClientMsg::Kind::Invalid);
  CHECK(decode(R"({"type":"cmd")").kind == ClientMsg::Kind::Invalid);
  CHECK(decode(std::string(kMaxFrame + 1, ' ')).kind == ClientMsg::Kind::Invalid);
}

// ---- value conversion -------------------------------------------------------------

namespace {
Scalar S_bool(bool b) { Scalar s; s.kind = Scalar::Kind::Bool; s.b = b; return s; }
Scalar S_int(std::int64_t i) { Scalar s; s.kind = Scalar::Kind::Int; s.i = i; return s; }
Scalar S_float(double f) { Scalar s; s.kind = Scalar::Kind::Float; s.f = f; return s; }
Scalar S_pair(std::int64_t x, std::int64_t y) { Scalar s; s.kind = Scalar::Kind::Pair; s.x = x; s.y = y; return s; }
}  // namespace

TEST_CASE("to_value enforces the attribute's type and range") {
  Value v{};
  CHECK((to_value(S_bool(true), ValueType::Bool, v) && v.as_bool()));
  CHECK_FALSE(to_value(S_int(1), ValueType::Bool, v));  // 1 is not a bool

  CHECK((to_value(S_int(0), ValueType::U8, v) && v.as_u8() == 0));
  CHECK((to_value(S_int(255), ValueType::U8, v) && v.as_u8() == 255));
  CHECK_FALSE(to_value(S_int(256), ValueType::U8, v));
  CHECK_FALSE(to_value(S_int(-1), ValueType::U8, v));
  CHECK_FALSE(to_value(S_bool(true), ValueType::U8, v));
  CHECK_FALSE(to_value(S_float(1.5), ValueType::U8, v));

  CHECK((to_value(S_int(-32768), ValueType::I16, v) && v.as_i16() == -32768));
  CHECK_FALSE(to_value(S_int(32768), ValueType::I16, v));
  CHECK((to_value(S_int(65535), ValueType::U16, v) && v.as_u16() == 65535));
  CHECK_FALSE(to_value(S_int(65536), ValueType::U16, v));
  CHECK((to_value(S_int(0xFFFFFFFFLL), ValueType::U32, v) && v.as_u32() == 0xFFFFFFFFu));
  CHECK_FALSE(to_value(S_int(0x100000000LL), ValueType::U32, v));
  CHECK((to_value(S_int(7), ValueType::Enum8, v) && v.as_enum8() == 7));

  CHECK((to_value(S_float(1.5), ValueType::F32, v) && v.as_f32() == doctest::Approx(1.5f)));
  CHECK((to_value(S_int(3), ValueType::F32, v) && v.as_f32() == doctest::Approx(3.0f)));
  CHECK_FALSE(to_value(S_bool(true), ValueType::F32, v));

  std::uint16_t x = 0, y = 0;
  CHECK(to_value(S_pair(100, 65535), ValueType::XyColor, v));
  v.as_xy(x, y);
  CHECK((x == 100 && y == 65535));
  CHECK_FALSE(to_value(S_pair(-1, 0), ValueType::XyColor, v));
  CHECK_FALSE(to_value(S_pair(0, 65536), ValueType::XyColor, v));
  CHECK_FALSE(to_value(S_int(1), ValueType::XyColor, v));
  CHECK_FALSE(to_value(Scalar{}, ValueType::Bool, v));
}

// ---- encoders -------------------------------------------------------------------------

TEST_CASE("single-frame encoders produce the documented JSON") {
  char buf[512];
  {
    JsonWriter w(buf, sizeof(buf));
    encode_welcome(w, HubInfo{"home", "7843023", 1781000000, "UTC", true});
    CHECK(std::string(w.c_str()) == R"({"type":"welcome","v":1,"hub":{"name":"home","fw":"7843023","time":1781000000,"tz":"UTC","synced":true}})");
  }
  {
    JsonWriter w(buf, sizeof(buf));
    encode_error(w, -1, Err::Unauthorized);
    CHECK(std::string(w.c_str()) == R"({"type":"error","code":"unauthorized"})");
  }
  {
    JsonWriter w(buf, sizeof(buf));
    encode_error(w, 7, Err::BadArgs, "value \"x\"");
    CHECK(std::string(w.c_str()) == R"({"type":"error","id":7,"code":"bad_args","message":"value \"x\""})");
  }
  {
    JsonWriter w(buf, sizeof(buf));
    encode_accepted(w, 7, 12);
    CHECK(std::string(w.c_str()) == R"({"type":"accepted","id":7,"seq":12})");
  }
  {
    JsonWriter w(buf, sizeof(buf));
    encode_ack(w, 7, hubproto::CmdAckStatus::Unreachable);
    CHECK(std::string(w.c_str()) == R"({"type":"ack","id":7,"status":"unreachable"})");
  }
  {
    JsonWriter w(buf, sizeof(buf));
    encode_res_ok(w, 8);
    CHECK(std::string(w.c_str()) == R"({"type":"res","id":8,"ok":true})");
  }
  {
    JsonWriter w(buf, sizeof(buf));
    encode_res_error(w, 8, Err::TableFull);
    CHECK(std::string(w.c_str()) == R"({"type":"res","id":8,"ok":false,"code":"table_full"})");
  }
  {
    JsonWriter w(buf, sizeof(buf));
    encode_time(w, 99, "Europe/Prague", false);
    CHECK(std::string(w.c_str()) == R"({"type":"time","time":99,"tz":"Europe/Prague","synced":false})");
  }
}

TEST_CASE("encode_state renders every value type as JSON-native") {
  char buf[128];
  auto st = [&](const Value& v) {
    JsonWriter w(buf, sizeof(buf));
    encode_state(w, 256, 1, v);
    REQUIRE(w.ok());
    return std::string(w.c_str());
  };
  CHECK(st(Value::make_bool(true)) == R"({"type":"state","entity":256,"attr":1,"value":true})");
  CHECK(st(Value::make_u16(3)) == R"({"type":"state","entity":256,"attr":1,"value":3})");
  CHECK(st(Value::make_i16(-215)) == R"({"type":"state","entity":256,"attr":1,"value":-215})");
  CHECK(st(Value::make_u32(4000000000u)) == R"({"type":"state","entity":256,"attr":1,"value":4000000000})");
  CHECK(st(Value::make_xy(100, 200)) == R"({"type":"state","entity":256,"attr":1,"value":[100,200]})");
  CHECK(st(Value::make_f32(1.5f)) == R"({"type":"state","entity":256,"attr":1,"value":1.5})");
}

TEST_CASE("write_entity: names, flags, unknown attributes and unreported values") {
  Registry reg;
  declare(reg, 1, 0x0100, hubproto::DeviceType::Switch,
          {{0x01, ValueType::Bool, 0x07}, {0x13, ValueType::U16, 0x05}, {0x77, ValueType::U8, 0x01}});
  REQUIRE(reg.set_value(0x0100, 0x01, Value::make_bool(true)));
  char buf[1024];
  JsonWriter w(buf, sizeof(buf));
  write_entity(w, *reg.find(0x0100), "plug1");
  REQUIRE(w.ok());
  CHECK(std::string(w.c_str()) ==
        R"({"id":256,"name":"plug1","chip":1,"device":"switch","attrs":[)"
        R"({"id":1,"name":"on_off","vt":"bool","flags":["r","w","rep"],"value":true},)"
        R"({"id":19,"name":"power_w","vt":"u16","flags":["r","rep"],"value":null},)"
        R"({"id":119,"name":"0x77","vt":"u8","flags":["r"],"value":null}]})");

  JsonWriter w2(buf, sizeof(buf));
  write_entity(w2, *reg.find(0x0100), nullptr);  // unnamed
  CHECK(std::string(w2.c_str()).find(R"("name":null,"chip")") != std::string::npos);
}

TEST_CASE("encoded entities and events parse back as valid JSON with the right shape") {
  Registry reg;
  declare(reg, 1, 0x0100, hubproto::DeviceType::Light, {{0x01, ValueType::Bool, 0x07}, {0x03, ValueType::XyColor, 0x07}});
  reg.set_value(0x0100, 0x03, Value::make_xy(1, 2));
  char buf[1024];
  JsonWriter w(buf, sizeof(buf));
  encode_entity_event(w, *reg.find(0x0100), "lamp");
  REQUIRE(w.ok());
  JsonReader r;
  REQUIRE(r.parse(w.c_str(), w.size()));
  CHECK(r.string_equals(r.find(r.root(), "type"), "entity"));
  const int e = r.find(r.root(), "entity");
  REQUIRE(e >= 0);
  CHECK(r.count(r.find(e, "attrs")) == 2);

  JsonWriter w2(buf, sizeof(buf));
  encode_entity_removed(w2, 0x0100);
  CHECK(std::string(w2.c_str()) == R"({"type":"entity","op":"remove","id":256})");
}

TEST_CASE("chips: telemetry members appear only when known") {
  char buf[256];
  ChipView c{1, "zigbee", "ready", 129, 0};
  c.has_heap = true;
  c.heap = 377260;
  c.has_crc = true;
  c.crc_errors = 0;
  JsonWriter w(buf, sizeof(buf));
  encode_chip_event(w, c);
  CHECK(std::string(w.c_str()) ==
        R"({"type":"chip","id":1,"label":"zigbee","status":"ready","boot_id":129,"reboots":0,"telemetry":{"heap":377260,"crc_errors":0}})");
  JsonWriter w2(buf, sizeof(buf));
  write_chip(w2, ChipView{2, "thread", "silent", 3, 1});
  CHECK(std::string(w2.c_str()) == R"({"id":2,"label":"thread","status":"silent","boot_id":3,"reboots":1,"telemetry":{}})");
}

TEST_CASE("rules, schedules and timers are encoded like the console lists them") {
  char buf[640];
  {
    Rule r{};
    r.trigger_entity = 1; r.trigger_attr = 0x70; r.match = nullptr; r.op = CmpOp::Always;
    r.action_entity = 256; r.action_attr = 1; r.action_value = Value::make_bool(true);
    r.window_start = 22 * 60; r.window_end = 6 * 60; r.cooldown_s = 300;
    r.has_cond = true; r.cond_entity = 257; r.cond_attr = 0x13; r.cond_op = CmpOp::Gt; r.cond_operand = Value::make_u16(1000);
    JsonWriter w(buf, sizeof(buf));
    write_rule(w, 0, r);
    CHECK(std::string(w.c_str()) ==
          R"({"index":0,"when":{"entity":1,"attr":112,"op":"any"},"then":{"entity":256,"attr":1,"value":true},)"
          R"("between":{"from":"22:00","to":"06:00"},"and":{"entity":257,"attr":19,"op":">","value":1000},"cooldown_s":300})");
  }
  {
    Rule r{};
    r.trigger_entity = 256; r.trigger_attr = 0x13; r.match = nullptr; r.op = CmpOp::Gt; r.operand = Value::make_u16(1500);
    r.action_entity = 256; r.action_attr = 1; r.action_value = Value::make_bool(false);
    JsonWriter w(buf, sizeof(buf));
    write_rule(w, 3, r);
    CHECK(std::string(w.c_str()) ==
          R"({"index":3,"when":{"entity":256,"attr":19,"op":">","value":1500},"then":{"entity":256,"attr":1,"value":false}})");
  }
  using Sched = ScheduleTable<4>;
  Sched t;
  t.add(0x0100, 1, Value::make_bool(false), 22, 0);
  t.add(0x0101, 1, Value::make_u8(2), 6, 30, kWeekdays);
  t.add(0x0102, 1, Value::make_bool(true), 7, 5, kWeekend);
  t.add(0x0103, 1, Value::make_bool(true), 8, 15, 0x0A);  // mon, wed
  const char* expect[] = {
      R"({"index":0,"time":"22:00","days":"daily","entity":256,"attr":1,"action":"off"})",
      R"({"index":1,"time":"06:30","days":"weekdays","entity":257,"attr":1,"action":"toggle"})",
      R"({"index":2,"time":"07:05","days":"weekends","entity":258,"attr":1,"action":"on"})",
      R"({"index":3,"time":"08:15","days":["mon","wed"],"entity":259,"attr":1,"action":"on"})",
  };
  for (std::size_t i = 0; i < 4; ++i) {
    JsonWriter w(buf, sizeof(buf));
    write_schedule(w, i, *t.get(i));
    CHECK(std::string(w.c_str()) == expect[i]);
  }
  JsonWriter w(buf, sizeof(buf));
  write_timer(w, TimerView{0, 256, 1, Value::make_u8(2), 42});
  CHECK(std::string(w.c_str()) == R"({"index":0,"entity":256,"attr":1,"action":"toggle","left_s":42})");
}

// ---- chunked snapshot ----------------------------------------------------------------

namespace {

struct FakeSource {
  std::size_t entities = 0, chips = 0, rules = 0, schedules = 0, timers = 0, scenes = 0;
  std::size_t count(Section s) const {
    switch (s) {
      case Section::Entities: return entities;
      case Section::Chips: return chips;
      case Section::Rules: return rules;
      case Section::Schedules: return schedules;
      case Section::Timers: return timers;
      case Section::Scenes: return scenes;
    }
    return 0;
  }
  void write(Section s, std::size_t i, JsonWriter& w) {
    // A deliberately fat item (~120 bytes) so a few dozen need several frames.
    w.begin_object().kv("s", static_cast<int>(s)).kv("i", static_cast<std::int64_t>(i))
        .kv("pad", "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789")
        .end_object();
  }
};

struct Reassembled {
  std::size_t frames = 0, max_frame = 0;
  std::size_t items[kSections] = {};
  bool parts_in_order = true;
  bool last_only_at_end = true;
  bool ok = true;
};

Reassembled run(FakeSource& src, std::size_t soft_limit, std::size_t cap = kMaxFrame) {
  Reassembled out;
  SnapshotCursor c;
  std::vector<char> buf(cap);
  int guard = 0;
  while (!c.done() && guard++ < 1000) {
    JsonWriter w(buf.data(), buf.size());
    const std::uint32_t expect_part = c.part;
    if (!encode_snapshot_part(c, src, w, soft_limit)) { out.ok = false; break; }
    ++out.frames;
    out.max_frame = std::max(out.max_frame, w.size());
    BigReader r;
    if (!r.parse(w.c_str(), w.size())) { out.ok = false; break; }
    std::int64_t part = -1;
    r.get_i64(r.find(r.root(), "part"), part);
    if (part != static_cast<std::int64_t>(expect_part)) out.parts_in_order = false;
    bool last = false;
    r.get_bool(r.find(r.root(), "last"), last);
    if (last != c.done()) out.last_only_at_end = false;
    for (int s = 0; s < kSections; ++s) {
      const int arr = r.find(r.root(), section_key(static_cast<Section>(s)));
      if (arr >= 0) out.items[s] += static_cast<std::size_t>(r.count(arr));
    }
  }
  if (!c.done()) out.ok = false;
  return out;
}

}  // namespace

TEST_CASE("a small snapshot is one frame with part 0 and last=true") {
  FakeSource src{3, 2, 1, 1, 0};
  char buf[kMaxFrame];
  JsonWriter w(buf, sizeof(buf));
  SnapshotCursor c;
  REQUIRE(encode_snapshot_part(c, src, w, 3000));
  CHECK(c.done());
  BigReader r;
  REQUIRE(r.parse(w.c_str(), w.size()));
  std::int64_t part = -1;
  REQUIRE(r.get_i64(r.find(r.root(), "part"), part));
  CHECK(part == 0);
  bool last = false;
  REQUIRE(r.get_bool(r.find(r.root(), "last"), last));
  CHECK(last);
  CHECK(r.count(r.find(r.root(), "entities")) == 3);
  CHECK(r.count(r.find(r.root(), "timers")) == 0);
}

TEST_CASE("a big snapshot is split into parts that each fit a frame and cover every item once") {
  FakeSource src{120, 4, 16, 8, 8};
  const auto got = run(src, 3000);
  CHECK(got.ok);
  CHECK(got.frames > 3);
  CHECK(got.max_frame <= kMaxFrame);
  CHECK(got.parts_in_order);
  CHECK(got.last_only_at_end);
  CHECK(got.items[static_cast<int>(Section::Entities)] == 120);
  CHECK(got.items[static_cast<int>(Section::Chips)] == 4);
  CHECK(got.items[static_cast<int>(Section::Rules)] == 16);
  CHECK(got.items[static_cast<int>(Section::Schedules)] == 8);
  CHECK(got.items[static_cast<int>(Section::Timers)] == 8);
}

TEST_CASE("snapshot parts terminate and stay complete even with a tiny soft limit") {
  FakeSource src{10, 2, 2, 2, 2};
  const auto got = run(src, 1);  // every part carries about one item
  CHECK(got.ok);
  CHECK(got.frames >= 18);
  CHECK(got.items[static_cast<int>(Section::Entities)] == 10);
  CHECK(got.items[static_cast<int>(Section::Timers)] == 2);
  CHECK(got.parts_in_order);
}

TEST_CASE("an empty home still yields exactly one complete snapshot") {
  FakeSource src{};
  const auto got = run(src, 3000);
  CHECK(got.ok);
  CHECK(got.frames == 1);
  CHECK(got.last_only_at_end);
}

TEST_CASE("a source that shrinks between parts does not loop or crash") {
  struct Shrinking : FakeSource {
    int calls = 0;
    std::size_t count(Section s) {
      if (s == Section::Entities && ++calls > 2) return 1;  // was 50 when the first part was cut
      return FakeSource::count(s);
    }
  } src;
  src.entities = 50;
  SnapshotCursor c;
  std::vector<char> buf(kMaxFrame);
  int guard = 0;
  while (!c.done() && guard++ < 50) {
    JsonWriter w(buf.data(), buf.size());
    CHECK(encode_snapshot_part(c, src, w, 600));
    BigReader r;
    CHECK(r.parse(w.c_str(), w.size()));
  }
  CHECK(c.done());
}
