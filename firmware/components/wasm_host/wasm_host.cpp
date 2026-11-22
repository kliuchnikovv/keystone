#include "wasm_host/wasm_host.hpp"

#include "core/entity_registry.hpp"
#include "core/plugin_capabilities.hpp"
#include "core/plugin_manager.hpp"
#include "core/plugin_manifest.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "wasm_host/test_plugin_wasm.hpp"
#include "wasm_host/wamr_engine.hpp"

namespace wasm_host {

namespace {
constexpr const char* kTag = "wasm-host";

void log_heap(const char* where) {
  const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  ESP_LOGI(kTag, "%s: internal free=%u largest=%u", where, (unsigned)free_int,
           (unsigned)largest);
}

// A real co-processor answers every COMMAND with a CMD_ACK whose seq matches —
// that is how CommandDispatcher learns the command landed. A plugin's chip is
// loopback (ChipRouter fan_out, not a real UART), so nothing on the wire sends
// an ACK. This tiny subscriber fills that gap: for any COMMAND addressed to
// a plugin pseudo-chip (0xC0+), publish a CMD_ACK Ok back into the bus.
//
// Subscribe order matters here: we are added AFTER the manager, so by the
// time this runs the engine has already delivered the command synchronously
// (deliver_command blocks on its executor's completion semaphore). A smarter
// engine would propagate the real status, but for Plan 1b's first wiring Ok
// is the right default: `on_command` returned without a trap.
core::Bus* g_plugin_ack_bus = nullptr;

void plugin_ack_on_msg(void* /*ctx*/, const hubproto::Header& h,
                       const std::uint8_t* /*p*/, std::size_t /*n*/) {
  if (h.type != hubproto::MsgType::Command) return;
  if (h.chip_id < 0xC0) return;
  if (g_plugin_ack_bus == nullptr) return;
  std::uint8_t buf[2];
  auto r = hubproto::encode_cmd_ack(hubproto::CmdAckStatus::Ok, buf, sizeof(buf));
  if (!r.ok) return;
  hubproto::Header ack = h;
  ack.type = hubproto::MsgType::CmdAck;
  g_plugin_ack_bus->publish(ack, buf, r.len);
}

// The facade keeps one engine + one manager for the hub's lifetime. They
// outlive `start()` because the manager stays subscribed to the bus; the
// static holds only the template-parameter-free view of the pair.
WAMRPluginEngine g_engine;
}  // namespace

template <typename Registry>
bool start(core::Bus& bus, Registry& reg) {
  log_heap("wasm_host::start: before engine");
  if (!g_engine.begin()) {
    ESP_LOGE(kTag, "engine begin failed");
    return false;
  }
  log_heap("wasm_host::start: after engine");

  // One per lifetime; a static so it survives this function's scope. The
  // manager's constructor registers host callbacks and subscribes to bus.
  static core::plugin::PluginManager<Registry> manager(g_engine, bus, reg);
  // ACK responder — must subscribe AFTER the manager so the manager's handle
  // runs first (synchronous command delivery, then we ack Ok).
  g_plugin_ack_bus = &bus;
  bus.subscribe(&plugin_ack_on_msg, nullptr);
  log_heap("wasm_host::start: after manager");

  // Hand-built manifest — Plan 1b will parse one from JSON; for the first
  // wiring the fields come from here. Must declare EntityDeclare to let the
  // plugin call ks_entity_declare at all.
  core::plugin::Manifest m{};
  std::strncpy(m.id, "test.switch", sizeof(m.id) - 1);
  m.version = 1;
  m.abi = core::plugin::kAbiVersion;
  m.caps.add(core::plugin::Cap::EntityDeclare);
  m.caps.add(core::plugin::Cap::Log);
  m.caps.add(core::plugin::Cap::Timer);

  if (!manager.install(m, kTestPluginWasm, kTestPluginWasmLen)) {
    ESP_LOGE(kTag, "install %s failed", m.id);
    return false;
  }
  log_heap("wasm_host::start: after install");

  const core::plugin::KsErr r = manager.start(m.id);
  if (r != core::plugin::KsErr::Ok) {
    ESP_LOGE(kTag, "start %s: %d", m.id, (int)r);
    return false;
  }
  log_heap("wasm_host::start: after start()");
  ESP_LOGI(kTag, "plugin '%s' started", m.id);
  return true;
}

// Explicit instantiation for the brain's Registry type. The brain's main.cpp
// uses `core::EntityRegistry<>` with the defaults — if that changes, update
// this instantiation to match.
template bool start<core::EntityRegistry<>>(core::Bus&, core::EntityRegistry<>&);

}  // namespace wasm_host
