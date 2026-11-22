// WAMR-backed PluginEngine (Plan 1b).
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
// THREADING MODEL (design doc §5.5, "граница с ядром — очередь в главный
// цикл"): the core bus/registry must stay single-threaded. A plugin's host
// imports (ks_entity_declare, ks_state_publish, ...) therefore NEVER touch
// the core directly from the executor pthread. Instead:
//
//   plugin thread            main thread (the hub's main loop)
//   -----------            --------------------------
//   ks_state_publish()  -->  host_q_  -->  drain_host_calls()
//   (post + wait)              |              |
//   ^                           |              v
//   |                           |        manager's callbacks
//   |                           |        (declare/publish on the core)
//   |                           v
//   +---- read result <--- give host_done_ (per-request)
//
// The pump that drains `host_q_` lives in `run_job()`, i.e. it runs ON THE
// MAIN THREAD, inside the window where the manager has set its `active_`
// slot (the manager sets `active_` around `engine_.start()` /
// `engine_.deliver_command()` and clears it after they return). So a
// marshalled host call still lands in the right plugin slot, and the core is
// touched by exactly one thread. `core/` needs no change for this.
//
// The main thread waits on the job's completion semaphore in short slices and
// services the host queue between slices — that is also what makes the
// callback watchdog (Task 2) possible: the main thread knows the wall-clock
// budget and can declare the plugin faulted.
//
// If a host import is somehow called on the main thread itself (not our
// case, but defensive), it runs inline — no queue, no deadlock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include "core/plugin_engine.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

  // The per-callback time budget (design §5.5). A plugin callback that has
  // not returned within this many milliseconds is faulted: the plugin is
  // stopped and its entities withdrawn, the hub keeps running.
  // 0 disables the watchdog (not recommended).
  void set_callback_budget_ms(std::uint32_t ms) noexcept {
    budget_ms_ = ms;
  }
  std::uint32_t callback_budget_ms() const noexcept { return budget_ms_; }

  // How many interpreter instructions the plugin may execute per callback.
  // The fast interpreter checks this counter on EVERY opcode dispatch, so
  // the limit is what actually breaks a plugin that loops forever (the
  // wall-clock watchdog on the main thread only *notices*). The default is
  // calibrated for the S3 at 240 MHz: roughly 3 M instructions per 100 ms
  // of budget (measured: ~30 M instr/s for the fast interpreter on S3).
  void set_instruction_limit(std::int32_t limit) noexcept {
    instruction_limit_ = limit;
  }
  std::int32_t instruction_limit() const noexcept { return instruction_limit_; }

  // The wall-clock time the last callback took, in ms (for the log line the
  // budget check prints).
  std::uint32_t last_callback_ms() const noexcept { return last_cb_ms_; }

  // How many host calls have been marshalled through the main loop so far
  // (the counter proves the plugin never touches the core directly).
  std::uint32_t host_call_count() const noexcept { return host_calls_; }

  // --- the host-call protocol (public: the native shims use it) ------------
  //
  // A host import made by the plugin, waiting to be served by the main
  // thread. The plugin thread blocks until the main thread has run it.
  enum class HostKind : std::uint8_t { kDeclare, kPublishState, kSendCommand };

  struct HostCall {
    HostKind kind;
    std::uint8_t* data = nullptr;   // heap copy of the plugin's bytes
    std::size_t len = 0;
    std::uint16_t entity = 0;
    std::uint16_t attr = 0;
    std::uint16_t seq = 0;
    // results
    std::uint16_t declare_result = 0;
    core::plugin::KsErr err = core::plugin::KsErr::Ok;
  };

  // Called by the native shims ON THE PLUGIN THREAD: hands the request to
  // the main thread and blocks until it has been served. Returns false if
  // the request could not be delivered (the plugin must then behave as if
  // the call failed).
  bool call_host(HostCall& c);

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

  // Run one host call on THIS (the main) thread and fill in its result.
  void serve_host_call(HostCall& c);

  // Drain the host queue until it is empty (or until `budget` ms pass).
  // Called from run_job() on the main thread.
  void drain_host_calls(std::uint32_t budget_ms);

  core::plugin::PluginHostCallbacks callbacks_{};
  pthread_t executor_{};
  bool executor_running_ = false;
  // The job queue holds POINTERS (not values) so the completion semaphore can
  // be paired with the specific job — simplifies lifetime.
  QueueHandle_t jobs_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
  bool inited_ = false;

  // --- host-call marshalling (main thread <-> plugin thread) -----------------
  QueueHandle_t host_q_ = nullptr;      // plugin -> main
  SemaphoreHandle_t host_done_ = nullptr;  // main -> plugin
  HostCall* host_inflight_ = nullptr;   // the call being served right now
  // The hub's main task. Compared by task handle, NOT by pthread_self():
  // ESP-IDF's pthread_self() asserts on a task that pthread_create did not
  // make (spike report §4.4), and the main task is a plain FreeRTOS task.
  TaskHandle_t main_task_ = nullptr;
  std::uint32_t budget_ms_ = 100;      // callback time budget (design §5.5)
  std::int32_t instruction_limit_ = 3'000'000;  // per-callback instruction cap
  std::uint32_t last_cb_ms_ = 0;
  std::uint32_t host_calls_ = 0;       // marshalled host calls so far
  // Set when a callback overran its budget: the plugin is faulted.
  bool budget_exceeded_ = false;
  // Set when the executor thread was abandoned because a callback hung
  // (the design doc's fallback for the "free the module heap on fault?" risk).
  bool faulted_ = false;
};

}  // namespace wasm_host
