#include "wasm_host/wamr_engine.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "wasm_export.h"

namespace wasm_host {

namespace {
constexpr const char* kTag = "wamr-eng";

// Monotonic milliseconds. The engine needs its own clock (the core has
// none — it takes `now_ms` injected), and the callback budget must be
// measured on the thread that waits for the plugin.
std::uint32_t now_ms() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

// There is one engine per hub (one WAMR runtime per process), so a static
// pointer is enough to let the native shims reach it from a free function.
WAMRPluginEngine* g_engine = nullptr;

// Each `core::plugin::PluginInstance*` we hand to the manager is in fact a
// pointer to one of these — the engine owns the memory and never leaks the
// WAMR types across the engine boundary (keeps `core` radio/WAMR-free).
struct Plugin {
  wasm_module_t module;
  wasm_module_inst_t inst;
  wasm_exec_env_t env;
  std::uint8_t* wasm_copy;   // owned heap copy — fast interp patches in place
  std::size_t wasm_len;
};

// -- Pointer translation --------------------------------------------------
//
// A plugin passes host-visible pointers as i32 offsets into its linear
// memory. Translate them to native addresses with the WAMR helpers, which
// also validate that [offset, offset+len) is inside the plugin's memory.
// Returns nullptr on a bad range — the host callback then returns Denied.
const std::uint8_t* xlate(wasm_module_inst_t inst, std::uint32_t off,
                          std::uint32_t len) {
  if (!wasm_runtime_validate_app_addr(inst, off, len)) return nullptr;
  return static_cast<const std::uint8_t*>(
      wasm_runtime_addr_app_to_native(inst, off));
}

// -- Native host-import shims ---------------------------------------------
//
// These names MUST match the imports in the plugin .wasm file and the ABI
// the plugin SDK exports (core/plugin_host_api.hpp today is types-only; the
// textual names "ks_entity_declare" etc. are the convention Plan 1b fixes).
//
// Every shim runs on the executor pthread, INSIDE the plugin's callback. The
// core (bus/registry) is single-threaded (SPEC §3), so the shim does NOT
// call the manager directly: it hands the request to the main thread through
// the engine's host queue and waits for the result (design §5.5).

// ks_entity_declare(desc_ptr i32, desc_len i32) -> i32 (entity_id or 0)
int32_t shim_ks_entity_declare(wasm_exec_env_t env, int32_t desc_ptr,
                               int32_t desc_len) {
  if (g_engine == nullptr) return 0;
  const auto& cb = g_engine->host_callbacks();
  if (cb.declare_entity == nullptr) return 0;
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
  const std::uint8_t* desc =
      xlate(inst, static_cast<std::uint32_t>(desc_ptr),
            static_cast<std::uint32_t>(desc_len));
  if (desc == nullptr) return 0;
  // Marshalled call: the main thread runs the manager's declare_entity.
  WAMRPluginEngine::HostCall c{};
  c.kind = WAMRPluginEngine::HostKind::kDeclare;
  c.len = static_cast<std::size_t>(desc_len);
  c.data = static_cast<std::uint8_t*>(std::malloc(c.len == 0 ? 1 : c.len));
  if (c.data == nullptr) return 0;
  std::memcpy(c.data, desc, c.len);
  const bool ok = g_engine->call_host(c);
  std::free(c.data);
  if (!ok) return 0;
  return static_cast<int32_t>(c.declare_result);
}

// ks_state_publish(entity i32, attr i32, val_ptr i32, val_len i32) -> i32 (KsErr)
int32_t shim_ks_state_publish(wasm_exec_env_t env, int32_t entity, int32_t attr,
                              int32_t val_ptr, int32_t val_len) {
  if (g_engine == nullptr) return static_cast<int32_t>(core::plugin::KsErr::Denied);
  const auto& cb = g_engine->host_callbacks();
  if (cb.publish_state == nullptr) return static_cast<int32_t>(core::plugin::KsErr::Denied);
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
  const std::uint8_t* val =
      xlate(inst, static_cast<std::uint32_t>(val_ptr),
            static_cast<std::uint32_t>(val_len));
  if (val == nullptr) return static_cast<int32_t>(core::plugin::KsErr::BadArg);
  WAMRPluginEngine::HostCall c{};
  c.kind = WAMRPluginEngine::HostKind::kPublishState;
  c.entity = static_cast<std::uint16_t>(entity);
  c.attr = static_cast<std::uint16_t>(attr);
  c.len = static_cast<std::size_t>(val_len);
  c.data = static_cast<std::uint8_t*>(std::malloc(c.len == 0 ? 1 : c.len));
  if (c.data == nullptr) return static_cast<int32_t>(core::plugin::KsErr::NoMem);
  std::memcpy(c.data, val, c.len);
  const bool ok = g_engine->call_host(c);
  std::free(c.data);
  if (!ok) return static_cast<int32_t>(core::plugin::KsErr::Denied);
  return static_cast<int32_t>(c.err);
}

// ks_log(ptr i32, len i32) -> i32 (0).  Harmless, always granted.
int32_t shim_ks_log(wasm_exec_env_t env, int32_t ptr, int32_t len) {
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
  const std::uint8_t* s =
      xlate(inst, static_cast<std::uint32_t>(ptr),
            static_cast<std::uint32_t>(len));
  if (s == nullptr) return -1;
  // Bounded: ESP log API truncates long lines; we pass a stack buffer.
  char buf[128];
  const std::size_t n =
      static_cast<std::size_t>(len) < sizeof(buf) - 1
          ? static_cast<std::size_t>(len)
          : sizeof(buf) - 1;
  std::memcpy(buf, s, n);
  buf[n] = '\0';
  ESP_LOGI(kTag, "plugin: %s", buf);
  return 0;
}

// ks_timer_set(ms i32, id i32) -> i32 (KsErr). Arms a timer; the hub calls
// the plugin's on_timer(id) when it fires. The Timer capability is harmless
// and granted to every plugin (design §5.4).
int32_t shim_ks_timer_set(wasm_exec_env_t env, int32_t ms, int32_t id) {
  if (g_engine == nullptr) return static_cast<int32_t>(core::plugin::KsErr::Denied);
  if (ms <= 0) return static_cast<int32_t>(core::plugin::KsErr::BadArg);
  // The plugin handle is the instance the exec_env belongs to. The engine
  // maps it back from the exec_env's module instance.
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
  void* owner = g_engine->instance_of(inst);
  if (owner == nullptr) {
    ESP_LOGE(kTag, "ks_timer_set: the plugin is not registered");
    return static_cast<int32_t>(core::plugin::KsErr::NotFound);
  }
  g_engine->set_timer(reinterpret_cast<core::plugin::PluginInstance*>(owner),
                      static_cast<std::uint32_t>(ms),
                      static_cast<std::uint32_t>(id));
  ESP_LOGI(kTag, "ks_timer_set: %u ms, id %u", (unsigned)ms, (unsigned)id);
  return static_cast<int32_t>(core::plugin::KsErr::Ok);
}

// ks_timer_set_repeating(period_ms i32, id i32) -> i32 (KsErr). A heartbeat:
// the hub calls on_timer(id) every period_ms until the timer is replaced.
int32_t shim_ks_timer_set_repeating(wasm_exec_env_t env, int32_t period_ms,
                                     int32_t id) {
  if (g_engine == nullptr) return static_cast<int32_t>(core::plugin::KsErr::Denied);
  if (period_ms <= 0) return static_cast<int32_t>(core::plugin::KsErr::BadArg);
  wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
  void* owner = g_engine->instance_of(inst);
  if (owner == nullptr) {
    ESP_LOGE(kTag, "ks_timer_set_repeating: the plugin is not registered");
    return static_cast<int32_t>(core::plugin::KsErr::NotFound);
  }
  g_engine->set_repeating_timer(
      reinterpret_cast<core::plugin::PluginInstance*>(owner),
      static_cast<std::uint32_t>(period_ms), static_cast<std::uint32_t>(id));
  ESP_LOGI(kTag, "ks_timer_set_repeating: %u ms, id %u", (unsigned)period_ms,
           (unsigned)id);
  return static_cast<int32_t>(core::plugin::KsErr::Ok);
}

NativeSymbol kNatives[] = {
    {"ks_entity_declare", (void*)shim_ks_entity_declare, "(ii)i", nullptr},
    {"ks_state_publish", (void*)shim_ks_state_publish, "(iiii)i", nullptr},
    {"ks_log", (void*)shim_ks_log, "(ii)i", nullptr},
    {"ks_timer_set", (void*)shim_ks_timer_set, "(ii)i", nullptr},
    {"ks_timer_set_repeating", (void*)shim_ks_timer_set_repeating, "(ii)i", nullptr},
};
}  // namespace

WAMRPluginEngine::WAMRPluginEngine() = default;
WAMRPluginEngine::~WAMRPluginEngine() { end(); }

bool WAMRPluginEngine::begin() {
  if (inited_) return true;
  if (g_engine != nullptr) {
    ESP_LOGE(kTag, "another engine is already running");
    return false;
  }

  RuntimeInitArgs args{};
  args.mem_alloc_type = Alloc_With_System_Allocator;
  if (!wasm_runtime_full_init(&args)) {
    ESP_LOGE(kTag, "wasm_runtime_full_init failed");
    return false;
  }
  if (!wasm_runtime_register_natives(
          "env", kNatives, sizeof(kNatives) / sizeof(kNatives[0]))) {
    ESP_LOGE(kTag, "register_natives failed");
    wasm_runtime_destroy();
    return false;
  }

  jobs_ = xQueueCreate(4, sizeof(Job*));
  done_ = xSemaphoreCreateBinary();
  // The host-call queue: plugin thread -> main thread. Depth 8 is plenty:
  // the plugin blocks on each call, so at most one is in flight per plugin.
  host_q_ = xQueueCreate(8, sizeof(HostCall*));
  host_done_ = xSemaphoreCreateBinary();
  timer_mu_ = xSemaphoreCreateMutex();
  if (jobs_ == nullptr || done_ == nullptr || host_q_ == nullptr ||
      host_done_ == nullptr || timer_mu_ == nullptr) {
    ESP_LOGE(kTag, "queue/sem alloc failed");
    if (jobs_ != nullptr) vQueueDelete(jobs_);
    if (done_ != nullptr) vSemaphoreDelete(done_);
    if (host_q_ != nullptr) vQueueDelete(host_q_);
    if (host_done_ != nullptr) vSemaphoreDelete(host_done_);
    if (timer_mu_ != nullptr) vSemaphoreDelete(timer_mu_);
    jobs_ = nullptr;
    done_ = nullptr;
    host_q_ = nullptr;
    host_done_ = nullptr;
    timer_mu_ = nullptr;
    wasm_runtime_destroy();
    return false;
  }

  // Executor pthread. Plan 1b design §5.5: the plugin runs on its own thread
  // below the UART/radio priority. pthread_create inherits the parent task's
  // priority by default (OK for first bring-up); a tuned priority is a later
  // dial.
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  g_engine = this;  // visible to the executor BEFORE pthread_create
  main_task_ = xTaskGetCurrentTaskHandle();
  if (pthread_create(&executor_, &attr, &WAMRPluginEngine::executor_main,
                     this) != 0) {
    ESP_LOGE(kTag, "pthread_create failed");
    pthread_attr_destroy(&attr);
    g_engine = nullptr;
    vQueueDelete(jobs_);
    vSemaphoreDelete(done_);
    vQueueDelete(host_q_);
    vSemaphoreDelete(host_done_);
    vSemaphoreDelete(timer_mu_);
    jobs_ = nullptr;
    done_ = nullptr;
    host_q_ = nullptr;
    host_done_ = nullptr;
    timer_mu_ = nullptr;
    wasm_runtime_destroy();
    return false;
  }
  pthread_attr_destroy(&attr);
  executor_running_ = true;
  inited_ = true;
  ESP_LOGI(kTag, "engine up (executor pthread started, callback budget %u ms)",
           static_cast<unsigned>(budget_ms_));
  return true;
}

void WAMRPluginEngine::end() {
  if (!inited_) return;
  if (executor_running_) {
    // Send a sentinel (null-inst Unload) that the executor interprets as
    // "stop". We don't support unloading a plugin whose job is in flight.
    Job* sentinel = new Job{};
    sentinel->kind = JobKind::kUnload;
    sentinel->inst = nullptr;
    xQueueSend(jobs_, &sentinel, portMAX_DELAY);
    pthread_join(executor_, nullptr);
    executor_running_ = false;
  }
  if (jobs_ != nullptr) { vQueueDelete(jobs_); jobs_ = nullptr; }
  if (done_ != nullptr) { vSemaphoreDelete(done_); done_ = nullptr; }
  if (host_q_ != nullptr) { vQueueDelete(host_q_); host_q_ = nullptr; }
  if (host_done_ != nullptr) { vSemaphoreDelete(host_done_); host_done_ = nullptr; }
  if (timer_mu_ != nullptr) { vSemaphoreDelete(timer_mu_); timer_mu_ = nullptr; }
  wasm_runtime_destroy();
  g_engine = nullptr;
  inited_ = false;
}

void WAMRPluginEngine::set_host_callbacks(
    const core::plugin::PluginHostCallbacks& cb) {
  callbacks_ = cb;
}

// The engine's loaded-plugin registry: a plugin's host import only knows
// its exec_env, so the shim needs a way back from a WAMR module instance to
// the plugin that owns it.
void* WAMRPluginEngine::instance_of(wasm_module_inst_t inst) const noexcept {
  for (const auto& e : instances_) {
    if (e.inst != nullptr && e.wasm_inst == inst) return e.inst;
  }
  return nullptr;
}

void WAMRPluginEngine::remember_instance(core::plugin::PluginInstance* inst,
                                          wasm_module_inst_t wasm_inst) noexcept {
  for (auto& e : instances_) {
    if (e.inst == nullptr) {
      e.inst = inst;
      e.wasm_inst = wasm_inst;
      return;
    }
  }
}

void WAMRPluginEngine::forget_instance(
    core::plugin::PluginInstance* inst) noexcept {
  for (auto& e : instances_) {
    if (e.inst == inst) {
      e.inst = nullptr;
      e.wasm_inst = nullptr;
    }
  }
}

// The wall-clock budget and the instruction cap are two halves of one
// watchdog: the cap is what BREAKS a plugin that loops forever (the fast
// interpreter checks it on every opcode dispatch), the budget is what
// NOTICES it. Deriving the cap from the budget keeps them in step.
void WAMRPluginEngine::set_callback_budget_ms(std::uint32_t ms) {
  budget_ms_ = ms;
  instruction_limit_ = static_cast<std::int32_t>(ms) * kInstructionsPerMs;
}

// -- Host-call marshalling ---------------------------------------------------
//
// call_host() runs on the PLUGIN thread. It posts the request to the main
// thread and waits for the answer.
bool WAMRPluginEngine::call_host(HostCall& c) {
  // Defensive: a host import called on the main thread itself (not our case)
  // must not deadlock — serve it inline.
  //
  // NOTE: we compare FreeRTOS task handles, NOT pthread_self(): ESP-IDF's
  // pthread_self() ASSERTS on any task that was not created by
  // pthread_create (spike report §4.4), and the hub's main task is a plain
  // FreeRTOS task. Calling it there would abort the chip.
  if (xTaskGetCurrentTaskHandle() == main_task_) {
    serve_host_call(c);
    return true;
  }
  if (host_q_ == nullptr) return false;
  HostCall* p = &c;
  if (xQueueSend(host_q_, &p, pdMS_TO_TICKS(500)) != pdTRUE) {
    ESP_LOGE(kTag, "host call: queue full or timed out");
    return false;
  }
  ESP_LOGD(kTag, "host call #%u marshalled to the main loop", ++host_calls_);
  // Wait for the main thread to serve it. The bound is generous: the main
  // thread is inside a plugin callback, and the callback budget (100 ms by
  // default) is what actually fires when a plugin misbehaves.
  if (xSemaphoreTake(host_done_, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(kTag, "host call: the main thread did not answer in time");
    return false;
  }
  return true;
}

// serve_host_call() runs on the MAIN thread and performs the manager's
// callback — i.e. the only place the core is touched.
void WAMRPluginEngine::serve_host_call(HostCall& c) {
  const auto& cb = callbacks_;
  switch (c.kind) {
    case HostKind::kDeclare:
      c.declare_result = (cb.declare_entity != nullptr)
                             ? cb.declare_entity(cb.ctx, c.data, c.len)
                             : 0;
      c.err = core::plugin::KsErr::Ok;
      break;
    case HostKind::kPublishState:
      c.err = (cb.publish_state != nullptr)
                  ? cb.publish_state(cb.ctx, c.entity, c.attr, c.data, c.len)
                  : core::plugin::KsErr::Denied;
      break;
    case HostKind::kSendCommand:
      c.err = (cb.send_command != nullptr)
                  ? cb.send_command(cb.ctx, c.entity, c.attr, c.data, c.len, c.seq)
                  : core::plugin::KsErr::Denied;
      break;
  }
}

// drain_host_calls() runs on the MAIN thread: it takes the pending host
// calls and serves them. Called from run_job() while the plugin callback is
// in flight.
void WAMRPluginEngine::drain_host_calls(std::uint32_t budget_ms) {
  if (host_q_ == nullptr) return;
  const std::uint32_t t0 = now_ms();
  for (;;) {
    HostCall* p = nullptr;
    if (xQueueReceive(host_q_, &p, 0) != pdTRUE) break;
    host_inflight_ = p;
    serve_host_call(*p);
    host_inflight_ = nullptr;
    xSemaphoreGive(host_done_);  // let the plugin thread continue
    if (budget_ms != 0) {
      const std::uint32_t dt = now_ms() - t0;
      if (dt >= budget_ms) {
        ESP_LOGE(kTag, "host calls overran the callback budget (%u ms)", dt);
        budget_exceeded_ = true;
        break;
      }
    }
  }
}

// plugin_abi_version() asks the plugin itself which ABI it was built
// against. The plugin's export is the ground truth (a manifest is data and
// can lie); the manifest's declared ABI is checked separately.
//
// The call runs on the executor thread — WAMR may only execute on a pthread
// (spike report §4.4) — so it goes through the job queue like every other
// plugin call.
std::uint32_t WAMRPluginEngine::plugin_abi_version(
    core::plugin::PluginInstance* inst) {
  if (inst == nullptr) return 0;
  Job j{};
  j.kind = JobKind::kReadAbi;
  j.inst = inst;
  j.abi_version = 0;
  if (run_job(j) != core::plugin::KsErr::Ok) return 0;
  return j.abi_version;
}

core::plugin::PluginInstance* WAMRPluginEngine::load(const uint8_t* wasm,
                                                     std::size_t len) {
  if (!inited_) return nullptr;
  // Fast interpreter patches bytecode in place: give it a writable copy.
  auto* copy = static_cast<std::uint8_t*>(std::malloc(len));
  if (copy == nullptr) return nullptr;
  std::memcpy(copy, wasm, len);

  char err[128]{};
  wasm_module_t module = wasm_runtime_load(copy, static_cast<uint32_t>(len),
                                           err, sizeof(err));
  if (module == nullptr) {
    ESP_LOGE(kTag, "load: %s", err);
    std::free(copy);
    return nullptr;
  }
  // Give the plugin a 4 KB heap inside its own linear memory so
  // `wasm_runtime_module_malloc` (used to pass COMMAND bytes into the plugin)
  // has room. The spike used heap=0 because it never passed buffers in; a
  // real plugin needs it.
  wasm_module_inst_t inst = wasm_runtime_instantiate(
      module, 4096 /* stack */, 4096 /* heap */, err, sizeof(err));
  if (inst == nullptr) {
    ESP_LOGE(kTag, "instantiate: %s", err);
    wasm_runtime_unload(module);
    std::free(copy);
    return nullptr;
  }
  wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 4096);
  if (env == nullptr) {
    ESP_LOGE(kTag, "create_exec_env failed");
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(module);
    std::free(copy);
    return nullptr;
  }
  auto* p = new Plugin{module, inst, env, copy, len};
  auto* handle = reinterpret_cast<core::plugin::PluginInstance*>(p);
  remember_instance(handle, inst);

  // The ABI check (design §3, §11). The plugin must export
  // `ks_plugin_abi_version()` and report the ABI the hub speaks; a plugin
  // that does not, or that reports a different one, is REFUSED — it never
  // reaches the manager, never declares an entity, never runs.
  //
  // This reads the plugin's own export, not the manifest's declaration: a
  // manifest is data that can lie (or come from a different source than the
  // binary), while the export inside the binary is the ground truth.
  const std::uint32_t abi = plugin_abi_version(handle);
  if (abi == 0) {
    ESP_LOGE(kTag,
             "load: the plugin does not report a usable ABI — refusing it");
    forget_instance(handle);
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(module);
    std::free(copy);
    delete p;
    return nullptr;
  }
  if (abi != core::plugin::kAbiVersion) {
    ESP_LOGE(kTag, "load: the plugin speaks ABI %u, the hub speaks %u — "
                   "refusing it",
             (unsigned)abi, (unsigned)core::plugin::kAbiVersion);
    forget_instance(handle);
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(module);
    std::free(copy);
    delete p;
    return nullptr;
  }
  ESP_LOGI(kTag, "load: the plugin speaks ABI %u — accepted",
           (unsigned)abi);
  return handle;
}

core::plugin::KsErr WAMRPluginEngine::start(core::plugin::PluginInstance* inst) {
  Job j{};
  j.kind = JobKind::kStart;
  j.inst = inst;
  return run_job(j);
}

void WAMRPluginEngine::stop(core::plugin::PluginInstance* inst) {
  Job j{};
  j.kind = JobKind::kStop;
  j.inst = inst;
  (void)run_job(j);
}

core::plugin::KsErr WAMRPluginEngine::deliver_command(
    core::plugin::PluginInstance* inst, std::uint16_t entity,
    std::uint16_t attr, const std::uint8_t* val, std::size_t len,
    std::uint16_t seq) {
  Job j{};
  j.kind = JobKind::kDeliverCmd;
  j.inst = inst;
  j.entity = entity;
  j.attr = attr;
  j.val = val;
  j.len = len;
  j.seq = seq;
  return run_job(j);
}

core::plugin::KsErr WAMRPluginEngine::deliver_state(
    core::plugin::PluginInstance* inst, std::uint16_t entity,
    std::uint16_t attr, const std::uint8_t* val, std::size_t len) {
  Job j{};
  j.kind = JobKind::kDeliverSt;
  j.inst = inst;
  j.entity = entity;
  j.attr = attr;
  j.val = val;
  j.len = len;
  return run_job(j);
}

void WAMRPluginEngine::unload(core::plugin::PluginInstance* inst) {
  Job j{};
  j.kind = JobKind::kUnload;
  j.inst = inst;
  (void)run_job(j);
}

// ---- the plugin's timers (design §6) -------------------------------------

// set_timer() is called from the plugin thread (the ks_timer_set import) or
// from the main thread. The timer list is the MAIN thread's — the hub's
// clock is the main loop's — so the list is guarded by a mutex. (The plugin
// thread only ever touches ONE entry, under the same lock.)
void WAMRPluginEngine::set_timer(core::plugin::PluginInstance* inst,
                                  std::uint32_t ms, std::uint32_t id) {
  set_repeating_timer(inst, ms, id);
  // A one-shot: clear the period of the slot we just armed.
  xSemaphoreTake(timer_mu_, portMAX_DELAY);
  for (auto& t : timers_) {
    if (t.used && t.inst == inst && t.id == id) t.period_ms = 0;
  }
  xSemaphoreGive(timer_mu_);
}

void WAMRPluginEngine::set_repeating_timer(core::plugin::PluginInstance* inst,
                                           std::uint32_t period_ms,
                                           std::uint32_t id) {
  if (inst == nullptr) return;
  xSemaphoreTake(timer_mu_, portMAX_DELAY);
  // Re-arming the same (plugin, id) replaces the previous timer — the plugin
  // gets exactly one timer per id.
  Timer* slot = nullptr;
  for (auto& t : timers_) {
    if (t.used && t.inst == inst && t.id == id) {
      slot = &t;
      break;
    }
  }
  if (slot == nullptr) {
    for (auto& t : timers_) {
      if (!t.used) {
        slot = &t;
        break;
      }
    }
  }
  if (slot == nullptr) {
    xSemaphoreGive(timer_mu_);
    ESP_LOGW(kTag, "no free timer slot (the plugin has %u)",
             (unsigned)timer_count());
    return;
  }
  slot->inst = inst;
  slot->id = id;
  slot->due_ms = now_ms() + period_ms;
  slot->period_ms = period_ms;
  slot->used = true;
  xSemaphoreGive(timer_mu_);
}

void WAMRPluginEngine::cancel_timer(core::plugin::PluginInstance* inst,
                                     std::uint32_t id) {
  xSemaphoreTake(timer_mu_, portMAX_DELAY);
  for (auto& t : timers_) {
    if (t.used && (inst == nullptr || t.inst == inst) && t.id == id) {
      t.used = false;
    }
  }
  xSemaphoreGive(timer_mu_);
}

// poll_timers() runs on the MAIN thread: it delivers the timers that are
// due. Each delivery goes through the same job queue and the same watchdog
// as every other plugin callback.
std::size_t WAMRPluginEngine::poll_timers(std::uint32_t now,
                                           core::plugin::PluginInstance* inst) {
  // Collect the due timers first (the delivery blocks on the plugin, and we
  // must not hold the lock while it runs).
  struct Due {
    core::plugin::PluginInstance* inst;
    std::uint32_t id;
  };
  Due due[kMaxTimers]{};
  std::size_t n = 0;
  xSemaphoreTake(timer_mu_, portMAX_DELAY);
  for (auto& t : timers_) {
    if (!t.used) continue;
    if (inst != nullptr && t.inst != inst) continue;
    if (static_cast<std::int32_t>(now - t.due_ms) < 0) continue;
    if (n < kMaxTimers) {
      due[n].inst = t.inst;
      due[n].id = t.id;
      ++n;
    }
    if (t.period_ms != 0) {
      // A repeating timer: re-arm it for the next period. The period is
      // measured from the DUE time, not from now, so a late poll does not
      // make the tick drift.
      t.due_ms += t.period_ms;
      // A plugin that was asleep for a long time must not get a burst of
      // catch-up ticks: skip the periods it missed.
      const std::int32_t behind =
          static_cast<std::int32_t>(now - t.due_ms);
      if (behind >= static_cast<std::int32_t>(t.period_ms)) {
        t.due_ms = now + t.period_ms;
      }
    } else {
      t.used = false;  // one-shot
    }
  }
  xSemaphoreGive(timer_mu_);

  for (std::size_t i = 0; i < n; ++i) {
    const core::plugin::KsErr r = deliver_timer(due[i].inst, due[i].id);
    if (r != core::plugin::KsErr::Ok) {
      ESP_LOGE(kTag, "on_timer(%u) failed: %d", (unsigned)due[i].id, (int)r);
    } else {
      ESP_LOGI(kTag, "on_timer(%u) fired", (unsigned)due[i].id);
    }
  }
  return n;
}

core::plugin::KsErr WAMRPluginEngine::deliver_timer(
    core::plugin::PluginInstance* inst, std::uint32_t id) {
  Job j{};
  j.kind = JobKind::kDeliverTimer;
  j.inst = inst;
  j.timer_id = id;
  return run_job(j);
}

// run_job() runs on the MAIN thread: it hands the job to the executor and
// waits for it — while SERVING the host calls the plugin makes meanwhile.
//
// The wait is a slice loop, not a single blocking take, for two reasons:
//   1. the plugin's host imports need the main thread to answer them, and
//   2. the callback budget (design §5.5) can only be enforced by someone
//      watching the clock — that is the main thread.
//
// The watchdog has two halves, and they are not equal:
//
//   - The INSTRUCTION limit is the mechanism. It is armed on the exec_env
//     before the call, and the fast interpreter checks it on EVERY opcode
//     dispatch, so a plugin that loops forever trips it and unwinds CLEANLY:
//     the module instance stays intact and can be unloaded normally.
//   - The WALL-CLOCK check is the backstop. It notices that the callback is
//     over the budget (100 ms) and reports it. Because the instruction limit
//     is calibrated to roughly the same wall time, the plugin normally
//     unwinds on its own a moment later — the main thread waits for that
//     unwinding instead of killing anything.
//
// If the plugin does NOT unwind (a limit that is set too high, or a hang
// that is not an instruction loop), the executor thread is abandoned and
// replaced — the fallback the design doc prescribes for the "can we free the
// module heap on fault?" risk (design §5.5, "бринг-ап-риск").
core::plugin::KsErr WAMRPluginEngine::run_job(Job& j) {
  if (!executor_running_) return core::plugin::KsErr::NoMem;
  budget_exceeded_ = false;
  // The executor writes j.result and gives `done_`.
  Job* p = &j;
  xQueueSend(jobs_, &p, portMAX_DELAY);
  const std::uint32_t t0 = now_ms();
  for (;;) {
    // Serve whatever the plugin asked for while it runs.
    drain_host_calls(budget_ms_);
    if (xSemaphoreTake(done_, pdMS_TO_TICKS(2)) == pdTRUE) break;
    // Still running: check the budget.
    const std::uint32_t dt = now_ms() - t0;
    if (budget_ms_ != 0 && dt > budget_ms_) {
      last_cb_ms_ = dt;
      ESP_LOGE(kTag,
               "callback budget exceeded: %u ms > %u ms — the plugin is faulted",
               dt, static_cast<unsigned>(budget_ms_));
      budget_exceeded_ = true;
      // Give the interpreter time to reach its instruction limit and unwind.
      // It normally does so within a few tens of milliseconds — the limit is
      // calibrated to the budget.
      const std::uint32_t grace_start = now_ms();
      const std::uint32_t grace_ms = 500;
      while (now_ms() - grace_start < grace_ms) {
        drain_host_calls(budget_ms_);
        if (xSemaphoreTake(done_, pdMS_TO_TICKS(5)) == pdTRUE) {
          // The plugin unwound on its own: the trap is clean, the module
          // instance is intact and can be unloaded without leaking.
          ESP_LOGE(kTag, "plugin unwound %u ms after the budget (%u ms total)",
                   now_ms() - grace_start, now_ms() - t0);
          last_cb_ms_ = now_ms() - t0;
          return core::plugin::KsErr::NoMem;
        }
      }
      // It did not unwind: the executor thread is stuck. Abandon it — the
      // fallback the design doc prescribes.
      ESP_LOGE(kTag, "plugin did not unwind — abandoning the executor thread");
      faulted_ = true;
      pthread_detach(executor_);
      executor_running_ = false;
      executor_ = pthread_t{};
      // Wake the (still running) plugin thread's host-call wait so it can
      // notice the fault and unwind.
      if (host_done_ != nullptr) xSemaphoreGive(host_done_);
      cancel_timers_of(j.inst);
      return core::plugin::KsErr::NoMem;
    }
  }
  last_cb_ms_ = now_ms() - t0;
  if (budget_exceeded_) {
    // The plugin overran its budget: it is faulted. Drop its timers — a
    // faulted plugin must not keep waking the hub up.
    cancel_timers_of(j.inst);
    return core::plugin::KsErr::NoMem;
  }
  if (j.result != core::plugin::KsErr::Ok) {
    // A trapped callback: the plugin is faulted too (the manager counts it).
    cancel_timers_of(j.inst);
  }
  return j.result;
}

// cancel_timers_of() drops every timer of one plugin.
void WAMRPluginEngine::cancel_timers_of(core::plugin::PluginInstance* inst) {
  if (inst == nullptr) return;
  xSemaphoreTake(timer_mu_, portMAX_DELAY);
  for (auto& t : timers_) {
    if (t.used && t.inst == inst) t.used = false;
  }
  xSemaphoreGive(timer_mu_);
}

void* WAMRPluginEngine::executor_main(void* arg) {
  auto* self = static_cast<WAMRPluginEngine*>(arg);
  ESP_LOGI(kTag, "executor pthread up");
  for (;;) {
    Job* p = nullptr;
    xQueueReceive(self->jobs_, &p, portMAX_DELAY);
    if (p == nullptr) continue;
    // The sentinel sent by end() carries inst=nullptr and kUnload: shut down.
    if (p->kind == JobKind::kUnload && p->inst == nullptr) {
      delete p;
      break;
    }
    auto* plug = reinterpret_cast<Plugin*>(p->inst);
    // Arm the watchdog: the fast interpreter decrements this counter on
    // every opcode dispatch, so a plugin that never returns trips it and
    // unwinds cleanly (the module stays intact, no thread is abandoned).
    // -1 means "no limit".
#if KS_WAMR_INSTRUCTION_METERING
    wasm_runtime_set_instruction_count_limit(plug->env,
                                             self->instruction_limit_);
    ESP_LOGD(kTag, "armed the instruction cap: %d", (int)self->instruction_limit_);
#endif
    switch (p->kind) {
      case JobKind::kStart: {
        wasm_function_inst_t fn =
            wasm_runtime_lookup_function(plug->inst, "on_start");
        if (fn == nullptr) {
          p->result = core::plugin::KsErr::Ok;  // optional entry point
        } else if (wasm_runtime_call_wasm(plug->env, fn, 0, nullptr)) {
          p->result = core::plugin::KsErr::Ok;
        } else {
          ESP_LOGE(kTag, "on_start: %s",
                   wasm_runtime_get_exception(plug->inst));
          p->result = core::plugin::KsErr::NoMem;
        }
        break;
      }
      case JobKind::kStop: {
        wasm_function_inst_t fn =
            wasm_runtime_lookup_function(plug->inst, "on_stop");
        if (fn != nullptr) {
          (void)wasm_runtime_call_wasm(plug->env, fn, 0, nullptr);
        }
        p->result = core::plugin::KsErr::Ok;
        break;
      }
      case JobKind::kDeliverCmd: {
        wasm_function_inst_t fn =
            wasm_runtime_lookup_function(plug->inst, "on_command");
        if (fn == nullptr) {
          p->result = core::plugin::KsErr::NotFound;
          break;
        }
        // Copy the native val[len] into the plugin's linear memory so the
        // plugin can read it through its val_ptr arg.
        uint64_t app_ptr64 = 0;
        void* native_ptr = nullptr;
        if (p->len > 0) {
          app_ptr64 = wasm_runtime_module_malloc(
              plug->inst, static_cast<uint64_t>(p->len), &native_ptr);
          if (app_ptr64 == 0) {
            ESP_LOGE(kTag, "on_command: module_malloc(%u) failed",
                     (unsigned)p->len);
            p->result = core::plugin::KsErr::NoMem;
            break;
          }
          std::memcpy(native_ptr, p->val, p->len);
        }
        uint32_t argv[5] = {
            static_cast<uint32_t>(p->entity),
            static_cast<uint32_t>(p->attr),
            static_cast<uint32_t>(app_ptr64),
            static_cast<uint32_t>(p->len),
            static_cast<uint32_t>(p->seq),
        };
        const std::uint32_t t_call = now_ms();
        const bool ok = wasm_runtime_call_wasm(plug->env, fn, 5, argv);
        const std::uint32_t dt = now_ms() - t_call;
        if (app_ptr64 != 0) wasm_runtime_module_free(plug->inst, app_ptr64);
        if (!ok) {
          ESP_LOGE(kTag, "on_command trap after %u ms: %s", dt,
                   wasm_runtime_get_exception(plug->inst));
          p->result = core::plugin::KsErr::NoMem;
        } else {
          ESP_LOGD(kTag, "on_command returned in %u ms", dt);
          p->result = core::plugin::KsErr::Ok;
        }
        break;
      }
      case JobKind::kReadAbi: {
        // Ask the plugin which ABI it was built against. The export is
        // REQUIRED (design §11: "Манифест обязан её объявить, иначе
        // AbiIncompatible") — a plugin without it is refused, because we
        // cannot know whether it speaks our host-import ABI.
        wasm_function_inst_t fn =
            wasm_runtime_lookup_function(plug->inst, "ks_plugin_abi_version");
        if (fn == nullptr) {
          ESP_LOGE(kTag, "the plugin does not export ks_plugin_abi_version");
          p->abi_version = 0;
          p->result = core::plugin::KsErr::NotFound;
          break;
        }
        // wasm_runtime_call_wasm does not hand back a return value, so use
        // the variant that does.
        wasm_val_t results[1]{};
        results[0].kind = WASM_I32;
        wasm_val_t args[1]{};
        args[0].kind = WASM_I32;
        if (!wasm_runtime_call_wasm_a(plug->env, fn, 1, results, 0, args)) {
          ESP_LOGE(kTag, "ks_plugin_abi_version trap: %s",
                   wasm_runtime_get_exception(plug->inst));
          p->abi_version = 0;
          p->result = core::plugin::KsErr::NoMem;
          break;
        }
        p->abi_version = static_cast<std::uint32_t>(results[0].of.i32);
        ESP_LOGI(kTag, "the plugin reports ABI %u", (unsigned)p->abi_version);
        p->result = core::plugin::KsErr::Ok;
        break;
      }
      case JobKind::kDeliverSt: {
        // Same pattern — not used in v1 (no bus_read subscriptions), left as
        // a seam.
        p->result = core::plugin::KsErr::Ok;
        break;
      }
      case JobKind::kDeliverTimer: {
        // The plugin's on_timer(id). Optional export: a plugin without it
        // simply has no timers.
        wasm_function_inst_t fn =
            wasm_runtime_lookup_function(plug->inst, "on_timer");
        if (fn == nullptr) {
          p->result = core::plugin::KsErr::NotFound;
          break;
        }
        uint32_t argv[1] = {static_cast<uint32_t>(p->timer_id)};
        const std::uint32_t t_call = now_ms();
        const bool ok = wasm_runtime_call_wasm(plug->env, fn, 1, argv);
        if (!ok) {
          ESP_LOGE(kTag, "on_timer(%u) trap after %u ms: %s (cap %d)",
                   (unsigned)p->timer_id, now_ms() - t_call,
                   wasm_runtime_get_exception(plug->inst),
                   (int)self->instruction_limit_);
          p->result = core::plugin::KsErr::NoMem;
        } else {
          ESP_LOGD(kTag, "on_timer(%u) returned in %u ms", (unsigned)p->timer_id,
                   now_ms() - t_call);
          p->result = core::plugin::KsErr::Ok;
        }
        break;
      }
      case JobKind::kUnload: {
        if (plug != nullptr) {
          self->forget_instance(p->inst);
          // The plugin's timers die with it.
          self->cancel_timers_of(p->inst);
          wasm_runtime_destroy_exec_env(plug->env);
          wasm_runtime_deinstantiate(plug->inst);
          wasm_runtime_unload(plug->module);
          std::free(plug->wasm_copy);
          delete plug;
        }
        p->result = core::plugin::KsErr::Ok;
        break;
      }
    }
    xSemaphoreGive(self->done_);
  }
  ESP_LOGI(kTag, "executor pthread exiting");
  return nullptr;
}

}  // namespace wasm_host
