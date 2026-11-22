// WAMR-backed PluginEngine (Plan 1b, first cut).
//
// Portable `core::plugin::PluginEngine` describes what the manager needs; this
// class realizes it over WAMR on ESP-IDF. Key constraints from the spike
// (see docs/superpowers/specs/2026-11-21-wasm-plugin-spike-report.md §4):
//
//   - WAMR's esp-idf `os_self_thread()` calls `pthread_self()`; ESP-IDF's
//     `pthread_self()` asserts on a non-pthread FreeRTOS task. Every WAMR
//     call therefore has to run on a real pthread — including `start`,
//     `deliver_command`, host-imports called transitively from the plugin.
//     The engine owns one executor pthread per lifetime and marshals each
//     manager->engine call onto it via a job queue + completion semaphore.
//   - The fast interpreter patches the bytecode in place; `wasm_runtime_load`
//     needs a WRITABLE copy — the engine makes one on `load`.
//
// Because the manager calls the engine synchronously from the main task and
// waits on the completion semaphore, the main task is BLOCKED while the
// plugin runs — the plugin's native host shims therefore touch core's Bus /
// EntityRegistry under an implicit lock (no explicit mutex needed in v1).
// Project-doc §5.5 ultimately wants a proper queue to the main loop; this
// first cut gets the engine on the air with that contract.
#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include "core/plugin_engine.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace wasm_host {

// One instance per engine lifetime. Owns the plugin executor pthread.
class WAMRPluginEngine final : public core::plugin::PluginEngine {
 public:
  WAMRPluginEngine();
  ~WAMRPluginEngine() override;

  // Spin up the WAMR runtime, register the host-import shims, and start the
  // executor pthread. Call once before the manager installs anything.
  // Returns false if WAMR init or the pthread failed.
  bool begin();

  // Stop the executor and tear down WAMR. Idempotent.
  void end();

  // --- core::plugin::PluginEngine ------------------------------------------
  void set_host_callbacks(const core::plugin::PluginHostCallbacks& cb) override;
  core::plugin::PluginInstance* load(const uint8_t* wasm, std::size_t len) override;
  core::plugin::KsErr start(core::plugin::PluginInstance* inst) override;
  void stop(core::plugin::PluginInstance* inst) override;
  core::plugin::KsErr deliver_command(core::plugin::PluginInstance* inst,
                                      std::uint16_t entity, std::uint16_t attr,
                                      const std::uint8_t* val, std::size_t len,
                                      std::uint16_t seq) override;
  core::plugin::KsErr deliver_state(core::plugin::PluginInstance* inst,
                                    std::uint16_t entity, std::uint16_t attr,
                                    const std::uint8_t* val,
                                    std::size_t len) override;
  void unload(core::plugin::PluginInstance* inst) override;

  // Called by the native host-import shims (which run on the executor pthread).
  // The shims use these to reach the callbacks the manager installed.
  const core::plugin::PluginHostCallbacks& host_callbacks() const noexcept {
    return callbacks_;
  }

 private:
  enum class JobKind : std::uint8_t { kStart, kStop, kDeliverCmd, kDeliverSt, kUnload };

  struct Job {
    JobKind kind;
    core::plugin::PluginInstance* inst;
    std::uint16_t entity;
    std::uint16_t attr;
    const std::uint8_t* val;  // native pointer; caller keeps alive for the call
    std::size_t len;
    std::uint16_t seq;
    core::plugin::KsErr result;
  };

  static void* executor_main(void* arg);
  core::plugin::KsErr run_job(Job& j);

  core::plugin::PluginHostCallbacks callbacks_{};
  pthread_t executor_{};
  bool executor_running_ = false;
  // The job queue holds POINTERS (not values) so the completion semaphore can
  // be paired with the specific job — simplifies lifetime.
  QueueHandle_t jobs_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
  bool inited_ = false;
};

}  // namespace wasm_host
