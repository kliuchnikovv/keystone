#include <doctest/doctest.h>

#include <string>

#include "core/app_requests.hpp"
#include "core/setup_mode.hpp"

using namespace core;

TEST_CASE("a setup code is 12 characters of the unambiguous alphabet") {
  CHECK(setup_code_valid("ABCDEFGHJKMN"));
  CHECK(setup_code_valid("234567892345"));
  CHECK_FALSE(setup_code_valid("ABCDEFGHJKM"));     // too short
  CHECK_FALSE(setup_code_valid("ABCDEFGHJKMNP"));   // too long
  CHECK_FALSE(setup_code_valid("ABCDEFGHJKM0"));    // 0 looks like O
  CHECK_FALSE(setup_code_valid("ABCDEFGHJKMI"));    // I looks like 1
  CHECK_FALSE(setup_code_valid("abcdefghjkmn"));    // stored upper-case
  CHECK_FALSE(setup_code_valid(nullptr));
  CHECK_FALSE(setup_code_valid(""));
}

TEST_CASE("what a person types is normalised: case, dashes and spaces do not matter") {
  char out[kSetupCodeLen + 1];
  CHECK(setup_code_normalize("abcd-efgh-jkmn", out, sizeof(out)));
  CHECK(std::string(out) == "ABCDEFGHJKMN");
  CHECK(setup_code_normalize(" ABCD EFGH JKMN ", out, sizeof(out)));
  CHECK(std::string(out) == "ABCDEFGHJKMN");
  CHECK_FALSE(setup_code_normalize("ABCD-EFGH-JKM", out, sizeof(out)));
  CHECK(std::string(out).empty());
  CHECK_FALSE(setup_code_normalize("ABCD-EFGH-JKMN-P", out, sizeof(out)));
  CHECK_FALSE(setup_code_normalize("ABCD-EFGH-JKM0", out, sizeof(out)));
  CHECK_FALSE(setup_code_normalize(nullptr, out, sizeof(out)));
  char small[4];
  CHECK_FALSE(setup_code_normalize("ABCDEFGHJKMN", small, sizeof(small)));
}

TEST_CASE("the access point is named after the last two bytes of the MAC") {
  const std::uint8_t mac[6] = {0x10, 0x51, 0xDB, 0x5E, 0xF8, 0x94};
  char ssid[20];
  setup_ap_ssid(mac, ssid, sizeof(ssid));
  CHECK(std::string(ssid) == "Keystone-F894");
}

TEST_CASE("a setup client may scan and apply, nothing else") {
  CHECK(setup_op_allowed("wifi.scan"));
  CHECK(setup_op_allowed("setup.apply"));
  for (const char* op : {"wifi.set", "rule.add", "rule.del", "schedule.add", "timer.add", "scene.put", "scene.apply", "scene.del",
                         "name.set", "pair", "unpair", "identify", "location.set", "location.clear", ""}) {
    CHECK_FALSE(setup_op_allowed(op));
  }
  CHECK_FALSE(setup_op_allowed(nullptr));
}

TEST_CASE("with no credentials at all the access point opens at once") {
  SetupPolicy p;
  CHECK_FALSE(p.ap_wanted());
  p.update(1000, false, false);
  CHECK(p.ap_wanted());
}

TEST_CASE("with credentials the access point waits ten minutes without a connection") {
  SetupPolicy p;
  p.update(0, false, true);  // boot: the router has not answered yet
  p.update(SetupPolicy::kDownMs - 1, false, true);
  CHECK_FALSE(p.ap_wanted());
  p.update(SetupPolicy::kDownMs, false, true);
  CHECK(p.ap_wanted());
}

TEST_CASE("a connection that comes back in time means no access point") {
  SetupPolicy p;
  p.update(0, false, true);
  p.update(5000, true, true);
  p.update(60000, true, true);
  CHECK_FALSE(p.ap_wanted());
  p.update(100000, false, true);  // a blip
  p.update(100000 + SetupPolicy::kDownMs - 1, false, true);
  CHECK_FALSE(p.ap_wanted());
  p.update(100000 + SetupPolicy::kDownMs - 1000, true, true);  // back before the deadline: the clock restarts
  CHECK_FALSE(p.ap_wanted());
}

TEST_CASE("once open the access point stays until the hub has had its network for a minute") {
  SetupPolicy p;
  p.update(0, false, false);
  CHECK(p.ap_wanted());
  p.update(10000, false, true);  // the setup saved credentials; still joining
  CHECK(p.ap_wanted());
  p.update(20000, true, true);  // joined
  p.update(20000 + SetupPolicy::kLingerMs - 1, true, true);
  CHECK(p.ap_wanted());  // the app is still reading the result
  p.update(20000 + SetupPolicy::kLingerMs, true, true);
  CHECK_FALSE(p.ap_wanted());
}

TEST_CASE("a wrong password keeps the access point so the person can try again") {
  SetupPolicy p;
  p.update(0, false, false);
  p.update(5000, false, true);  // credentials saved, but they do not work
  for (std::uint32_t t = 10000; t < 20u * 60u * 1000u; t += 30000) {
    p.update(t, false, true);
    CHECK(p.ap_wanted());
  }
}

TEST_CASE("a connection that drops again before the minute is over keeps the access point") {
  SetupPolicy p;
  p.update(0, false, false);
  p.update(1000, true, true);
  p.update(30000, false, true);  // lost it again
  p.update(30000 + SetupPolicy::kLingerMs + 1, true, true);
  CHECK(p.ap_wanted());  // the minute counts from the new connection
}

TEST_CASE("the console can force the access point on and off") {
  SetupPolicy p;
  p.update(0, true, true);
  CHECK_FALSE(p.ap_wanted());
  p.force(true);
  CHECK(p.ap_wanted());
  p.update(1000000, true, true);  // being connected does not close a forced one
  CHECK(p.ap_wanted());
  p.force(false);
  CHECK_FALSE(p.ap_wanted());
}

TEST_CASE("the clock wrapping around does not confuse the policy") {
  SetupPolicy p;
  const std::uint32_t t0 = 0xFFFFFFFFu - 1000u;
  p.update(t0, false, true);
  p.update(t0 + SetupPolicy::kDownMs - 1, false, true);  // wraps past zero
  CHECK_FALSE(p.ap_wanted());
  p.update(t0 + SetupPolicy::kDownMs, false, true);
  CHECK(p.ap_wanted());
}

namespace {
struct Applied {
  core::app::Outcome out;
  std::string ssid, pass, token;
};
Applied parse_setup(const std::string& args) {
  using namespace core::app;
  const std::string frame = R"({"type":"req","id":1,"op":"setup.apply","args":)" + args + "}";
  JsonReader r;
  const ClientMsg m = decode_client(r, frame.data(), frame.size());
  REQUIRE(m.kind == ClientMsg::Kind::Req);
  char ssid[64] = {}, pass[96] = {}, token[96] = {};
  Applied a;
  a.out = setup_parse(r, m.args, ssid, sizeof(ssid), pass, sizeof(pass), token, sizeof(token));
  a.ssid = ssid;
  a.pass = pass;
  a.token = token;
  return a;
}
}  // namespace

TEST_CASE("setup.apply carries the network and the token, and checks both") {
  const auto ok = parse_setup(R"({"ssid":"home","password":"correct horse","token":"0123456789abcdef"})");
  CHECK(ok.out.ok);
  CHECK(ok.ssid == "home");
  CHECK(ok.pass == "correct horse");
  CHECK(ok.token == "0123456789abcdef");
  CHECK(parse_setup(R"({"ssid":"cafe","password":"","token":"0123456789abcdef"})").out.ok);  // open network

  for (const char* bad : {R"({})", R"({"ssid":"home","password":"correct horse"})",
                          R"({"ssid":"home","password":"short","token":"0123456789abcdef"})",
                          R"({"ssid":"home","password":"correct horse","token":"short"})",
                          R"({"ssid":"home","password":"correct horse","token":"has a space in it"})",
                          R"({"ssid":"home","password":"correct horse","token":5})",
                          R"({"ssid":"home","password":"correct horse","token":"0123456789012345678901234567890123456789012345678901234567890123456789"})"}) {
    const auto a = parse_setup(bad);
    CHECK_FALSE(a.out.ok);
    CHECK(a.out.err == core::app::Err::BadArgs);
  }
}

TEST_CASE("a token is 8..64 printable characters without spaces") {
  CHECK(core::app::token_valid("abcdefgh"));
  CHECK(core::app::token_valid("a!@#$%^&*()_+-="));
  CHECK_FALSE(core::app::token_valid("abcdefg"));
  CHECK_FALSE(core::app::token_valid("abc defgh"));
  CHECK_FALSE(core::app::token_valid(nullptr));
  CHECK_FALSE(core::app::token_valid("tab\tcharacter"));
}

TEST_CASE("a setup client's welcome says so") {
  char buf[512];
  core::JsonWriter w(buf, sizeof(buf));
  core::app::encode_welcome(w, core::app::HubInfo{"home", "7", 0, "UTC", false}, true);
  const std::string s = w.c_str();
  CHECK(s.find(R"("setup":true})") != std::string::npos);
  core::JsonWriter w2(buf, sizeof(buf));
  core::app::encode_welcome(w2, core::app::HubInfo{"home", "7", 0, "UTC", false});
  CHECK(std::string(w2.c_str()).find("setup") == std::string::npos);
}
