#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "core/json.hpp"

using core::JsonReader;
using core::JsonWriter;

namespace {

bool parses(const char* s) {
  JsonReader r;
  return r.parse(s, std::strlen(s));
}

}  // namespace

// ---- writer ----------------------------------------------------------------

TEST_CASE("JsonWriter builds nested objects and arrays with correct commas") {
  char buf[256];
  JsonWriter w(buf, sizeof(buf));
  w.begin_object()
      .kv("type", "snapshot")
      .key("ids").begin_array().i64(1).i64(-2).u64(3).end_array()
      .key("o").begin_object().kv("a", true).key("n").null().end_object()
      .key("e").begin_array().end_array()
      .key("list").begin_array().begin_object().kv("x", 1).end_object().begin_object().kv("x", 2).end_object().end_array()
      .end_object();
  REQUIRE(w.ok());
  CHECK(std::string(w.c_str()) ==
        R"({"type":"snapshot","ids":[1,-2,3],"o":{"a":true,"n":null},"e":[],"list":[{"x":1},{"x":2}]})");
  CHECK(w.size() == std::strlen(w.c_str()));
}

TEST_CASE("JsonWriter escapes strings") {
  char buf[128];
  JsonWriter w(buf, sizeof(buf));
  w.begin_array().str("a\"b\\c\n\t\r\b\f").str(std::string("\x01\x1f", 2).c_str()).str("héllo").end_array();
  REQUIRE(w.ok());
  CHECK(std::string(w.c_str()) == "[\"a\\\"b\\\\c\\n\\t\\r\\b\\f\",\"\\u0001\\u001f\",\"héllo\"]");
}

TEST_CASE("JsonWriter numbers: extremes, floats, non-finite become null") {
  char buf[160];
  JsonWriter w(buf, sizeof(buf));
  w.begin_array()
      .i64(INT64_MIN).i64(INT64_MAX).u64(UINT64_MAX)
      .f64(1.5).f64(-0.25).f64(1e10)
      .f64(std::nan("")).f64(HUGE_VAL)
      .end_array();
  REQUIRE(w.ok());
  CHECK(std::string(w.c_str()) ==
        "[-9223372036854775808,9223372036854775807,18446744073709551615,1.5,-0.25,1e+10,null,null]");
}

TEST_CASE("JsonWriter reports overflow and never writes past the buffer") {
  for (std::size_t cap : {0u, 1u, 2u, 5u, 11u}) {
    char buf[64];
    std::memset(buf, 'X', sizeof(buf));
    JsonWriter w(buf, cap);
    w.begin_object().kv("hello", "world!!").end_object();
    CHECK_FALSE(w.ok());
    for (std::size_t i = cap; i < sizeof(buf); ++i) CHECK(buf[i] == 'X');  // untouched
    if (cap > 0) CHECK(buf[std::strlen(buf)] == '\0');
  }
  char exact[20];
  JsonWriter w(exact, sizeof(exact));
  w.begin_object().kv("a", 1).end_object();  // {"a":1} = 7 chars + NUL
  CHECK(w.ok());
}

TEST_CASE("JsonWriter ok() is false for unbalanced output and over-deep nesting") {
  char buf[64];
  JsonWriter a(buf, sizeof(buf));
  a.begin_object().kv("a", 1);
  CHECK_FALSE(a.ok());  // not closed
  JsonWriter b(buf, sizeof(buf));
  b.end_object();
  CHECK_FALSE(b.ok());  // closed without opening
  JsonWriter c(buf, sizeof(buf));
  for (int i = 0; i < 20; ++i) c.begin_array();
  CHECK_FALSE(c.ok());
}

// ---- reader ----------------------------------------------------------------

TEST_CASE("JsonReader reads a protocol-shaped message") {
  const char* text = R"( { "type" : "req", "id": 8, "op": "rule.add",
      "args": { "when": {"entity": 1, "attr": 112, "op": "any"},
                "then": {"entity": 256, "attr": 1, "value": true},
                "between": {"from": "18:00", "to": "23:00"}, "cooldown_s": 300,
                "xy": [100, 200], "f": -1.5e2 } } )";
  JsonReader r;
  REQUIRE(r.parse(text, std::strlen(text)));
  const int root = r.root();
  REQUIRE(r.is(root, JsonReader::Type::Object));
  char s[32];
  REQUIRE(r.get_string(r.find(root, "type"), s, sizeof(s)));
  CHECK(std::string(s) == "req");
  std::int64_t id = 0;
  REQUIRE(r.get_i64(r.find(root, "id"), id));
  CHECK(id == 8);

  const int args = r.find(root, "args");
  REQUIRE(args >= 0);
  const int when = r.find(args, "when");
  std::int64_t e = 0;
  REQUIRE(r.get_i64(r.find(when, "entity"), e));
  CHECK(e == 1);
  CHECK(r.string_equals(r.find(when, "op"), "any"));
  bool b = false;
  REQUIRE(r.get_bool(r.find(r.find(args, "then"), "value"), b));
  CHECK(b);
  // Keys after a nested object are still found (subtree skipping works).
  std::int64_t cd = 0;
  REQUIRE(r.get_i64(r.find(args, "cooldown_s"), cd));
  CHECK(cd == 300);
  const int xy = r.find(args, "xy");
  REQUIRE(r.is(xy, JsonReader::Type::Array));
  CHECK(r.count(xy) == 2);
  std::int64_t y = 0;
  REQUIRE(r.get_i64(r.at(xy, 1), y));
  CHECK(y == 200);
  CHECK(r.at(xy, 2) == -1);
  double f = 0;
  REQUIRE(r.get_double(r.find(args, "f"), f));
  CHECK(f == doctest::Approx(-150.0));
  // Missing keys, wrong types.
  CHECK(r.find(root, "nope") == -1);
  CHECK(r.find(r.find(root, "id"), "x") == -1);  // not an object
  std::int64_t tmp = 0;
  CHECK_FALSE(r.get_i64(r.find(args, "f"), tmp));  // has an exponent: not an integer literal
  CHECK_FALSE(r.get_i64(r.find(root, "op"), tmp)); // a string
}

TEST_CASE("JsonReader decodes string escapes and unicode") {
  const char* text = R"({"s":"a\"b\\c\/d\n\u00e9\u20ac\ud83d\ude00"})";
  JsonReader r;
  REQUIRE(r.parse(text, std::strlen(text)));
  char s[64];
  REQUIRE(r.get_string(r.find(r.root(), "s"), s, sizeof(s)));
  CHECK(std::string(s) == "a\"b\\c/d\n\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
  char tiny[4];
  CHECK_FALSE(r.get_string(r.find(r.root(), "s"), tiny, sizeof(tiny)));  // does not fit
}

TEST_CASE("JsonReader rejects bad surrogates when decoding") {
  for (const char* text : {R"({"s":"\ud83d"})", R"({"s":"\ud83dx"})", R"({"s":"\ude00"})", R"({"s":"\ud83d\u0041"})"}) {
    JsonReader r;
    REQUIRE(r.parse(text, std::strlen(text)));  // syntactically fine...
    char s[16];
    CHECK_FALSE(r.get_string(r.find(r.root(), "s"), s, sizeof(s)));  // ...but not decodable
  }
}

TEST_CASE("JsonReader integer range") {
  const char* text = R"([9223372036854775807, -9223372036854775808, 9223372036854775808, -9223372036854775809, 0, -0, 18446744073709551616])";
  JsonReader r;
  REQUIRE(r.parse(text, std::strlen(text)));
  std::int64_t v = 0;
  REQUIRE(r.get_i64(r.at(r.root(), 0), v));
  CHECK(v == INT64_MAX);
  REQUIRE(r.get_i64(r.at(r.root(), 1), v));
  CHECK(v == INT64_MIN);
  CHECK_FALSE(r.get_i64(r.at(r.root(), 2), v));
  CHECK_FALSE(r.get_i64(r.at(r.root(), 3), v));
  REQUIRE(r.get_i64(r.at(r.root(), 4), v));
  CHECK(v == 0);
  REQUIRE(r.get_i64(r.at(r.root(), 5), v));
  CHECK(v == 0);
  CHECK_FALSE(r.get_i64(r.at(r.root(), 6), v));
}

TEST_CASE("JsonReader accepts valid edge cases") {
  CHECK(parses("{}"));
  CHECK(parses("[]"));
  CHECK(parses(" \t\r\n{ } \n"));
  CHECK(parses(R"({"a":[],"b":{},"c":[[],[[]]]})"));
  CHECK(parses(R"({"k":null,"t":true,"f":false})"));
  CHECK(parses("[0, -1, 1.5, 1e5, 1E-5, 1.5e+3, 0.0]"));
  CHECK(parses(R"({"":1})"));  // empty key
  CHECK(parses(R"({"a":1,"a":2})"));  // duplicate keys are legal JSON
}

TEST_CASE("JsonReader rejects malformed input") {
  for (const char* bad : {
           "", " ", "{", "}", "[", "]", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "[1,]", "[,1]", "{,}",
           "{\"a\":1 \"b\":2}", "{a:1}", "{'a':1}", "[1 2]", "{\"a\":1}}", "{\"a\":1} x", "[1]]",
           "01", "[01]", "[1.]", "[.5]", "[-]", "[+1]", "[1e]", "[1e+]", "[0x10]", "[NaN]", "[Infinity]", "[-Infinity]",
           "tru", "[nul]", "[True]", "[truee]", "[\"a]", "[\"a\\\"]", "[\"\\x\"]", "[\"\\u12\"]", "[\"\\u12G4\"]",
           "{\"a\":1,\"b\"}", "// c\n{}", "/* c */{}", "{\"a\":1}//c"}) {
    CAPTURE(bad);
    CHECK_FALSE(parses(bad));
  }
  CHECK_FALSE(parses("[\"a\x01\"]"));  // raw control character in a string
  CHECK_FALSE(parses("[\"a\nb\"]"));
}

TEST_CASE("JsonReader: every strict prefix of a valid document is rejected") {
  const std::string doc = R"({"type":"req","id":8,"args":{"a":[1,2,{"b":"c\u00e9"}],"d":true,"e":null,"f":-1.5e2}})";
  JsonReader full;
  REQUIRE(full.parse(doc.data(), doc.size()));
  for (std::size_t n = 0; n < doc.size(); ++n) {
    JsonReader r;
    CHECK_FALSE(r.parse(doc.data(), n));
  }
}

TEST_CASE("JsonReader enforces its nesting and token limits") {
  std::string deep(20, '[');
  deep += std::string(20, ']');
  CHECK_FALSE(parses(deep.c_str()));
  std::string ok_depth(7, '[');
  ok_depth += std::string(7, ']');
  CHECK(parses(ok_depth.c_str()));

  // Many tokens: 200 numbers exceed the 96-token budget.
  std::string many = "[";
  for (int i = 0; i < 200; ++i) many += (i ? ",1" : "1");
  many += "]";
  CHECK_FALSE(parses(many.c_str()));
  std::string fits = "[";
  for (int i = 0; i < 90; ++i) fits += (i ? ",1" : "1");
  fits += "]";
  CHECK(parses(fits.c_str()));

  std::string huge(JsonReader::kMaxInput + 1, ' ');
  huge[0] = '{';
  JsonReader r;
  CHECK_FALSE(r.parse(huge.data(), huge.size()));
  CHECK_FALSE(r.parse(nullptr, 5));
}

TEST_CASE("JsonReader is safe on arbitrary mutations of valid messages") {
  const char* seeds[] = {
      R"({"type":"hello","v":1,"token":"s3cr3t","client":"app/0.1"})",
      R"({"type":"cmd","id":7,"entity":256,"attr":1,"value":false})",
      R"({"type":"req","id":8,"op":"rule.add","args":{"when":{"entity":1,"attr":112,"op":"any"},"then":{"entity":256,"attr":1,"value":true},"between":{"from":"18:00","to":"23:00"},"cooldown_s":300}})",
  };
  std::uint32_t rng = 12345;
  auto next = [&rng]() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  int accepted = 0, rejected = 0;
  for (const char* seed : seeds) {
    const std::string base = seed;
    for (int iter = 0; iter < 4000; ++iter) {
      std::string m = base;
      const int edits = 1 + static_cast<int>(next() % 4);
      for (int e = 0; e < edits; ++e) {
        const std::size_t pos = next() % m.size();
        switch (next() % 4) {
          case 0: m[pos] = static_cast<char>(next() & 0xFF); break;
          case 1: m.erase(pos, 1 + next() % 3); break;
          case 2: m.insert(pos, 1, "{}[]\",:\\0123456789tfn-."[next() % 24]); break;
          default: m.resize(pos); break;
        }
        if (m.empty()) m = "{";
      }
      JsonReader r;
      if (r.parse(m.data(), m.size())) {
        ++accepted;
        // Whatever it accepted, every accessor must stay in bounds.
        char s[16];
        std::int64_t i = 0;
        double d = 0;
        bool b = false;
        for (int t = 0; t < 8; ++t) {
          r.get_string(t, s, sizeof(s));
          r.get_i64(t, i);
          r.get_double(t, d);
          r.get_bool(t, b);
          r.find(t, "type");
          r.at(t, 0);
          r.count(t);
        }
      } else {
        ++rejected;
      }
    }
  }
  CHECK(rejected > 0);
  CHECK(accepted + rejected == 12000);
}
