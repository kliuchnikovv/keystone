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
    const std::size_t n = static_cast<std::size_t>(started++);
    if (fault_via_callback_on_start) {
      cb_.on_fault(cb_.ctx, "boom");  // plugin faults, then the engine returns Ok
      return KsErr::Ok;
    }
    bool do_fault = fault_on_start;
    if (!start_faults.empty() && n < start_faults.size()) do_fault = start_faults[n];
    if (do_fault) return KsErr::NoMem;  // plugin trapped in on_start
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
    if (send_on_command) {
      last_send_result = cb_.send_command(cb_.ctx, send_entity, send_attr,
                                          send_val.data(), send_val.size(), 1);
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
  bool fault_on_start = false;
  bool fault_via_callback_on_start = false;
  std::vector<bool> start_faults;  // per-start outcome by index, overrides above
  const std::uint8_t* declare_desc = nullptr;
  std::size_t declare_len = 0;
  std::uint16_t declared_eid = 0;
  bool publish_on_command = false;
  std::uint16_t pub_entity = 0, pub_attr = 0;
  std::vector<std::uint8_t> pub_val;
  bool send_on_command = false;
  std::uint16_t send_entity = 0, send_attr = 0;
  std::vector<std::uint8_t> send_val;
  KsErr last_send_result = KsErr::Ok;

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

TEST_CASE("plugin manager: fault on first on_start leaves hub intact") {
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  // An unrelated entity owned by a real chip is already on the hub.
  hp::AttrDecl a{0x01, hp::ValueType::Bool, hp::kFlagWritable};
  REQUIRE(reg.declare_virtual(/*chip_id=*/1, 0x0100, hp::DeviceType::Switch, &a,
                              1));

  engine.fault_on_start = true;  // traps before declaring anything

  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  CHECK(mgr.start("tuya") != KsErr::Ok);
  CHECK(mgr.state("tuya") == PluginState::Faulted);

  // No plugin entity was half-registered; the hub's own entity is intact.
  CHECK(reg.size() == 1);
  CHECK(reg.find(0x0100) != nullptr);
}

TEST_CASE("plugin manager: repeated faults quarantine") {
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  engine.fault_on_start = true;
  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));

  for (int i = 0; i < Manager::kQuarantineThreshold; ++i) {
    CHECK(mgr.start("tuya") != KsErr::Ok);
  }

  // Quarantined: refused without even invoking the engine again.
  const int before = engine.started;
  CHECK(mgr.start("tuya") == KsErr::Denied);
  CHECK(engine.started == before);
  CHECK(mgr.state("tuya") == PluginState::Faulted);
}

TEST_CASE("plugin manager: stop does not lift a quarantine") {
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  engine.fault_on_start = true;
  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  for (int i = 0; i < Manager::kQuarantineThreshold; ++i) mgr.start("tuya");

  mgr.stop("tuya");  // must not clear the Faulted state / un-quarantine
  CHECK(mgr.state("tuya") == PluginState::Faulted);
  CHECK(mgr.start("tuya") == KsErr::Denied);
}

TEST_CASE("plugin manager: a freed entity id is not reused after a fault") {
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  std::uint8_t desc[32];
  const std::size_t dn = make_switch_declare(0x0001, desc, sizeof(desc));
  engine.declare_desc = desc;
  engine.declare_len = dn;

  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);
  const std::uint16_t first = engine.declared_eid;
  REQUIRE(first != 0);

  // The plugin faults: its entity is withdrawn from the registry.
  mgr.on_plugin_fault("tuya", "boom");
  CHECK(reg.find(first) == nullptr);

  // A second plugin declares — it must NOT get the just-freed id back (a stale
  // rule/app reference to `first` must never resolve to a different device).
  core::plugin::Manifest m2{};
  std::strcpy(m2.id, "tuya2");
  m2.abi = core::plugin::kAbiVersion;
  m2.caps.add(Cap::EntityDeclare);
  REQUIRE(mgr.install(m2, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya2") == KsErr::Ok);
  const std::uint16_t second = engine.declared_eid;
  CHECK(second != 0);
  CHECK(second != first);  // monotonic: the freed id is not handed out again
}

TEST_CASE("plugin manager: a plugin cannot command another plugin's entity") {
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  std::uint8_t desc[32];
  const std::size_t dn = make_switch_declare(0x0001, desc, sizeof(desc));
  engine.declare_desc = desc;
  engine.declare_len = dn;

  core::plugin::Manifest m{};
  std::strcpy(m.id, "tuya");
  m.abi = core::plugin::kAbiVersion;
  m.caps.add(Cap::EntityDeclare);
  m.caps.add(Cap::BusCommand);
  // allow-list names an id in the plugin range (another plugin's / a freed id).
  m.allow[0] = Manager::kEntityBase + 50;
  m.allow_n = 1;

  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);
  const std::uint16_t eid = engine.declared_eid;

  // On the next command, the plugin tries to command a plugin-range entity it
  // does not own — it must be denied even though it is on the allow-list.
  engine.send_on_command = true;
  engine.send_entity = Manager::kEntityBase + 50;
  engine.send_attr = 0x01;
  engine.send_val = {1};

  hp::Command cmd{eid, 0x01, hp::Value::make_bool(true)};
  std::uint8_t cbuf[16];
  auto cr = hp::encode_command(cmd, cbuf, sizeof(cbuf));
  hp::Header ch{};
  ch.type = hp::MsgType::Command;
  ch.entity_id = eid;
  bus.deliver(ch, cbuf, cr.len);

  CHECK(engine.last_send_result == KsErr::Denied);
}

TEST_CASE("plugin manager: a plugin cannot command a hub-admin entity") {
  // C1 regression: a plugin must not reach a real hub entity (e.g. a chip
  // self-entity 0x0001, which accepts pair/unpair meta COMMANDs), even by
  // listing it in its own allow-list. send_command is inert in v1.
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  std::uint8_t desc[32];
  const std::size_t dn = make_switch_declare(0x0001, desc, sizeof(desc));
  engine.declare_desc = desc;
  engine.declare_len = dn;

  core::plugin::Manifest m{};
  std::strcpy(m.id, "tuya");
  m.abi = core::plugin::kAbiVersion;
  m.caps.add(Cap::EntityDeclare);
  m.caps.add(Cap::BusCommand);
  m.allow[0] = 0x0001;  // a real chip self-entity
  m.allow_n = 1;

  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);
  const std::uint16_t eid = engine.declared_eid;

  engine.send_on_command = true;
  engine.send_entity = 0x0001;
  engine.send_attr = 0x01;
  engine.send_val = {254};

  const std::size_t published_before = bus.published.size();
  hp::Command cmd{eid, 0x01, hp::Value::make_bool(true)};
  std::uint8_t cbuf[16];
  auto cr = hp::encode_command(cmd, cbuf, sizeof(cbuf));
  hp::Header ch{};
  ch.type = hp::MsgType::Command;
  ch.entity_id = eid;
  bus.deliver(ch, cbuf, cr.len);

  CHECK(engine.last_send_result == KsErr::Denied);
  CHECK(bus.published.size() == published_before);  // nothing went to the hub
}

TEST_CASE("plugin manager: a fault during on_start is not overwritten by Ok") {
  // I2: the plugin calls on_fault during on_start and the engine then returns
  // Ok. The plugin must end up Faulted, not Started.
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  engine.fault_via_callback_on_start = true;
  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  mgr.start("tuya");
  CHECK(mgr.state("tuya") == PluginState::Faulted);
}

TEST_CASE("plugin manager: a clean start does not reset the fault count") {
  // I1: faults are counted over the plugin's life, not reset by a lucky start,
  // so a fault/recover/fault loop still reaches quarantine.
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  // fault, succeed, fault, fault  -> three lifetime faults -> quarantined.
  engine.start_faults = {true, false, true, true};
  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));

  CHECK(mgr.start("tuya") != KsErr::Ok);  // fault 1
  CHECK(mgr.start("tuya") == KsErr::Ok);  // clean start (must NOT reset)
  CHECK(mgr.start("tuya") != KsErr::Ok);  // fault 2
  CHECK(mgr.start("tuya") != KsErr::Ok);  // fault 3 -> threshold
  const int before = engine.started;
  CHECK(mgr.start("tuya") == KsErr::Denied);  // quarantined
  CHECK(engine.started == before);            // engine not invoked
}

TEST_CASE("plugin manager: a single fault is counted once") {
  // I1 double-count: an on_fault callback followed by an engine error return
  // must count as ONE fault, so quarantine needs the full threshold.
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  engine.fault_on_start = true;  // engine returns error (one fault per start)
  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));

  // threshold-1 faults must NOT yet quarantine.
  for (int i = 0; i < Manager::kQuarantineThreshold - 1; ++i) {
    CHECK(mgr.start("tuya") != KsErr::Ok);
  }
  const int before = engine.started;
  CHECK(mgr.start("tuya") != KsErr::Denied);  // still allowed to run (not yet quarantined)
  CHECK(engine.started == before + 1);        // engine WAS invoked
}

TEST_CASE("plugin manager: stop withdraws the plugin's entities") {
  // I4: a stopped plugin must not leave ghost devices in the registry.
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  std::uint8_t desc[32];
  const std::size_t dn = make_switch_declare(0x0001, desc, sizeof(desc));
  engine.declare_desc = desc;
  engine.declare_len = dn;

  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);
  const std::uint16_t eid = engine.declared_eid;
  REQUIRE(reg.find(eid) != nullptr);

  mgr.stop("tuya");
  CHECK(reg.find(eid) == nullptr);
}

TEST_CASE("plugin manager: a plugin cannot be started twice") {
  // I4: a second start must not re-run on_start on a live plugin.
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);
  const int before = engine.started;
  CHECK(mgr.start("tuya") == KsErr::Denied);
  CHECK(engine.started == before);
}

TEST_CASE("plugin manager: publish_state rejects an out-of-range attr") {
  // Minor3: attr is u16 on the wire of the host call but u8 as an attribute id;
  // 0x0170 must not alias attr 0x70.
  Registry reg;
  FakeBus bus;
  FakePluginEngine engine;
  Manager mgr(engine, bus, reg);

  // Declare an entity that really HAS attr 0x70, so truncation of 0x0170 would
  // visibly write into it.
  std::uint8_t desc[48];
  hp::AttrDecl attrs[2] = {{0x01, hp::ValueType::Bool, hp::kFlagWritable},
                           {0x70, hp::ValueType::Bool, hp::kFlagWritable}};
  auto dr = hp::encode_declare(0x0001, hp::DeviceType::Switch, attrs, 2, nullptr,
                               desc, sizeof(desc));
  REQUIRE(dr.ok);
  engine.declare_desc = desc;
  engine.declare_len = dr.len;

  const auto m = declare_manifest();
  const std::uint8_t wasm[] = {0};
  REQUIRE(mgr.install(m, wasm, sizeof(wasm)));
  REQUIRE(mgr.start("tuya") == KsErr::Ok);
  const std::uint16_t eid = engine.declared_eid;

  engine.publish_on_command = true;
  engine.pub_entity = eid;
  engine.pub_attr = 0x0170;  // must NOT alias attr 0x70
  engine.pub_val = {1};

  hp::Command cmd{eid, 0x01, hp::Value::make_bool(true)};
  std::uint8_t cbuf[16];
  auto cr = hp::encode_command(cmd, cbuf, sizeof(cbuf));
  hp::Header ch{};
  ch.type = hp::MsgType::Command;
  ch.entity_id = eid;
  bus.deliver(ch, cbuf, cr.len);

  // attr 0x70 must still be unset — 0x0170 was rejected, not truncated into it.
  hp::Value got{};
  CHECK_FALSE(reg.get_value(eid, 0x70, got));
}
