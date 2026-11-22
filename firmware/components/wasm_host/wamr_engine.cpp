#include "wasm_host/wamr_engine.hpp"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "wasm_export.h"

namespace wasm_host {

namespace {
constexpr const char* kTag = "wamr-eng";

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
  return cb.declare_entity(cb.ctx, desc, static_cast<std::size_t>(desc_len));
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
  return static_cast<int32_t>(
      cb.publish_state(cb.ctx, static_cast<std::uint16_t>(entity),
                       static_cast<std::uint16_t>(attr), val,
                       static_cast<std::size_t>(val_len)));
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

NativeSymbol kNatives[] = {
    {"ks_entity_declare", (void*)shim_ks_entity_declare, "(ii)i", nullptr},
    {"ks_state_publish", (void*)shim_ks_state_publish, "(iiii)i", nullptr},
    {"ks_log", (void*)shim_ks_log, "(ii)i", nullptr},
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
  if (jobs_ == nullptr || done_ == nullptr) {
    ESP_LOGE(kTag, "queue/sem alloc failed");
    if (jobs_ != nullptr) vQueueDelete(jobs_);
    if (done_ != nullptr) vSemaphoreDelete(done_);
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
  if (pthread_create(&executor_, &attr, &WAMRPluginEngine::executor_main,
                     this) != 0) {
    ESP_LOGE(kTag, "pthread_create failed");
    pthread_attr_destroy(&attr);
    g_engine = nullptr;
    vQueueDelete(jobs_);
    vSemaphoreDelete(done_);
    jobs_ = nullptr;
    done_ = nullptr;
    wasm_runtime_destroy();
    return false;
  }
  pthread_attr_destroy(&attr);
  executor_running_ = true;
  inited_ = true;
  ESP_LOGI(kTag, "engine up (executor pthread started)");
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
  wasm_runtime_destroy();
  g_engine = nullptr;
  inited_ = false;
}

void WAMRPluginEngine::set_host_callbacks(
    const core::plugin::PluginHostCallbacks& cb) {
  callbacks_ = cb;
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
  return reinterpret_cast<core::plugin::PluginInstance*>(p);
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

core::plugin::KsErr WAMRPluginEngine::run_job(Job& j) {
  if (!executor_running_) return core::plugin::KsErr::NoMem;
  // The executor writes j.result and gives `done_`.
  Job* p = &j;
  xQueueSend(jobs_, &p, portMAX_DELAY);
  xSemaphoreTake(done_, portMAX_DELAY);
  return j.result;
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
        const bool ok = wasm_runtime_call_wasm(plug->env, fn, 5, argv);
        if (app_ptr64 != 0) wasm_runtime_module_free(plug->inst, app_ptr64);
        if (!ok) {
          ESP_LOGE(kTag, "on_command trap: %s",
                   wasm_runtime_get_exception(plug->inst));
          p->result = core::plugin::KsErr::NoMem;
        } else {
          p->result = core::plugin::KsErr::Ok;
        }
        break;
      }
      case JobKind::kDeliverSt: {
        // Same pattern — not used in v1 (no bus_read subscriptions), left as
        // a seam.
        p->result = core::plugin::KsErr::Ok;
        break;
      }
      case JobKind::kUnload: {
        if (plug != nullptr) {
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
