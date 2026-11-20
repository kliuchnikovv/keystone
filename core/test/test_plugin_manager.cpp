#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/entity_registry.hpp"
#include "core/plugin_engine.hpp"
#include "core/plugin_host_api.hpp"
#include "core/plugin_manager.hpp"
#include "core/plugin_manifest.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace hp = hubproto;
using core::plugin::Cap;
using core::plugin::KsErr;
using core::plugin::PluginState;
using Registry = core::EntityRegistry<>;
using Manager = core::plugin::PluginManager<Registry>;

namespace {

// A Bus double: records published messages and delivers injected inbound ones
// to subscribers — the InMemoryLink of this layer.
class FakeBus : public core::Bus {
 public:
  bool publish(const hp::Header& h, const std::uint8_t* p,
               std::size_t n) noexcept override {
    Published pub;
    pub.header = h;
    pub.payload.assign(p, p + n);
    published.push_back(pub);
    return true;
  }
  bool subscribe(MessageFn fn, void* ctx) noexcept override {
    subs.push_back({fn, ctx});
    return true;
  }
  void deliver(const hp::Header& h, const std::uint8_t* p, std::size_t n) {
    for (auto& s : subs) s.fn(s.ctx, h, p, n);
  }

  struct Published {
    hp::Header header;
    std::vector<std::uint8_t> payload;
  };
  struct Sub {
    MessageFn fn;
    void* ctx;
  };
  std::vector<Published> published;
  std::vector<Sub> subs;
};

// A PluginEngine double that scripts a plugin's behaviour. A plugin may only
// call its host imports WHILE it is executing — i.e. inside one of the engine
// entry points the manager drives — so this double declares on start() and
// publishes STATE on deliver_command(), exactly as a real plugin would.
class FakePluginEngine : public core::plugin::PluginEngine {
 public:
  void set_host_callbacks(
      const core::plugin::PluginHostCallbacks& cb) override {
    cb_ = cb;
  }
  core::plugin::PluginInstance* load(const std::uint8_t*,
                                     std::size_t) override {
    return reinterpret_cast<core::plugin::PluginInstance*>(&token_);
  }
  KsErr start(core::plugin::PluginInstance*) override {
    ++started;
    if (declare_desc != nullptr) {
      declared_eid =
          cb_.declare_entity(cb_.ctx, declare_desc, declare_len);
    }
    return KsErr::Ok;
  }
  void stop(core::plugin::PluginInstance*) override { ++stopped; }
  KsErr deliver_command(core::plugin::PluginInstance*, std::uint16_t e,
                        std::uint16_t a, const std::uint8_t* v, std::size_t n,
                        std::uint16_t s) override {
    last_cmd = {e, a, std::vector<std::uint8_t>(v, v + n), s};
    ++commands;
    if (publish_on_command) {
      cb_.publish_state(cb_.ctx, pub_entity, pub_attr, pub_val.data(),
                        pub_val.size());
    }
    return KsErr::Ok;
  }
  KsErr deliver_state(core::plugin::PluginInstance*, std::uint16_t,
                      std::uint16_t, const std::uint8_t*,
                      std::size_t) override {
    return KsErr::Ok;
  }
  void unload(core::plugin::PluginInstance*) override { ++unloaded; }

  // scripted behaviour
  const std::uint8_t* declare_desc = nullptr;
  std::size_t declare_len = 0;
  std::uint16_t declared_eid = 0;
  bool publish_on_command = false;
  std::uint16_t pub_entity = 0, pub_attr = 0;
  std::vector<std::uint8_t> pub_val;

  struct Cmd {
    std::uint16_t entity;
    std::uint16_t attr;
    std::vector<std::uint8_t> val;
    std::uint16_t seq;
  };
  Cmd last_cmd{};
  int started = 0, stopped = 0, commands = 0, unloaded = 0;

 private:
  core::plugin::PluginHostCallbacks cb_{};
  int token_ = 0;
};

std::size_t make_switch_declare(std::uint16_t id, std::uint8_t* out,
                                std::size_t cap) {
  hp::AttrDecl a{0x01, hp::ValueType::Bool, hp::kFlagWritable};
  auto r = hp::encode_declare(id, hp::DeviceType::Switch, &a, 1, nullptr, out,
                              cap);
  return r.ok ? r.len : 0;
}

core::plugin::Manifest declare_manifest() {
  core::plugin::Manifest m{};
  std::strcpy(m.id, "tuya");
  m.abi = core::plugin::kAbiVersion;
  m.caps.add(Cap::EntityDeclare);
  return m;
}

}  // namespace

TEST_CASE("plugin manager: install, start, declare, command, state") {
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  std::uint8_t desc[32];
  const std::size_t dn = make_switch_declare(0x0001, desc, sizeof(desc));
  REQUIRE(dn > 0);
  engine.declare_desc = desc;
  engine.declare_len = dn;

  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);
  CHECK(mgr.state("tuya") == PluginState::Started);

  // The plugin declared one entity during on_start; it is in the registry,
  // owned by a plugin pseudo-chip.
  const std::uint16_t eid = engine.declared_eid;
  CHECK(eid != 0);
  const auto* ent = reg.find(eid);
  REQUIRE(ent != nullptr);
  CHECK(ent->chip_id >= Manager::kChipBase);

  // When a COMMAND for that entity arrives on the bus, it reaches the engine
  // (seq preserved) and the plugin reacts by publishing STATE.
  engine.publish_on_command = true;
  engine.pub_entity = eid;
  engine.pub_attr = 0x01;
  engine.pub_val = {1};

  hp::Command cmd{eid, 0x01, hp::Value::make_bool(true)};
  std::uint8_t cbuf[16];
  auto cr = hp::encode_command(cmd, cbuf, sizeof(cbuf));
  REQUIRE(cr.ok);
  hp::Header ch{};
  ch.type = hp::MsgType::Command;
  ch.entity_id = eid;
  ch.seq = 7;
  bus.deliver(ch, cbuf, cr.len);

  CHECK(engine.commands == 1);
  CHECK(engine.last_cmd.entity == eid);
  CHECK(engine.last_cmd.attr == 0x01);
  CHECK(engine.last_cmd.seq == 7);

  hp::Value got{};
  REQUIRE(reg.get_value(eid, 0x01, got));
  CHECK(got.as_bool() == true);
}

TEST_CASE("plugin manager: declare colliding id is renumbered, registry intact") {
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  // Occupy the first id the manager would hand out, with a real chip entity.
  hp::AttrDecl a{0x01, hp::ValueType::Bool, hp::kFlagWritable};
  REQUIRE(reg.declare_virtual(/*chip_id=*/1, Manager::kEntityBase,
                              hp::DeviceType::Switch, &a, 1));

  std::uint8_t desc[32];
  const std::size_t dn = make_switch_declare(0x0001, desc, sizeof(desc));
  engine.declare_desc = desc;
  engine.declare_len = dn;

  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);

  const std::uint16_t eid = engine.declared_eid;
  // The manager renumbered past the occupied id.
  CHECK(eid != Manager::kEntityBase);
  CHECK(reg.find(eid) != nullptr);
  // The pre-existing entity is untouched (still owned by chip 1).
  const auto* original = reg.find(Manager::kEntityBase);
  REQUIRE(original != nullptr);
  CHECK(original->chip_id == 1);
}
