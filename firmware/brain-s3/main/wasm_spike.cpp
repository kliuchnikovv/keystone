// WAMR spike (wasm-plugin-spike), stages 2+3.
//
// Stage 2: bring up the interpreter, load an embedded .wasm that exports
// `add(i32,i32)` and `start()`. `add(3, 4) == 7` proves the interpreter
// runs; the heap deltas measure the runtime's RAM cost.
//
// Stage 3: the SAME module imports a host function `host_publish(i32)`.
// `start()` calls it with the literal 42. On the C side this is registered
// through `wasm_runtime_register_natives(...)` — exactly the point in a real
// PluginEngine where the manager's PluginHostCallbacks would run; proving
// this chain works on S3 is what the spike is after. Nothing else is needed
// to answer the verdict question "does WAMR invoke a host function from a
// plugin on this chip?".
//
// Everything that touches WAMR runs inside a pthread: WAMR's esp-idf
// platform layer (os_self_thread) calls pthread_self(), and ESP-IDF's
// pthread_self() asserts on a non-pthread FreeRTOS task. A real plugin
// executor task has to be a pthread for the same reason.
#include "wasm_spike.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "wasm_export.h"

namespace {
constexpr const char* kTag = "wamr-spike";

// Minimal WebAssembly module (hand-assembled). Exports:
//   add(i32, i32) -> i32              : proves the interpreter dispatches
//   start()                           : proves a plugin entry point runs and
//                                        can reach a host function
// Imports:
//   env.host_publish(i32)             : the native shim registered below
//
// WAT (equivalent):
//   (module
//     (import "env" "host_publish" (func $host_publish (param i32)))
//     (func (export "add") (param i32 i32) (result i32)
//       local.get 0 local.get 1 i32.add)
//     (func (export "start")
//       i32.const 42
//       call $host_publish))
//
// Hand-assembled byte-by-byte (Module format §5). The comments show section
// kind, section length, then its content.
alignas(4) constexpr uint8_t kPluginWasm[] = {
    // --- module header ---
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    // --- type section (id 1), payload 14 bytes = 0x0e ---
    0x01, 0x0e, 0x03,
    // type 0: () -> ()
    0x60, 0x00, 0x00,
    // type 1: (i32) -> ()
    0x60, 0x01, 0x7f, 0x00,
    // type 2: (i32, i32) -> i32
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    // --- import section (id 2), payload 20 bytes = 0x14 ---
    0x02, 0x14, 0x01,
    // import 0: env.host_publish, kind func, type 1
    0x03, 'e', 'n', 'v',
    0x0c, 'h', 'o', 's', 't', '_', 'p', 'u', 'b', 'l', 'i', 's', 'h',
    0x00, 0x01,
    // --- function section (id 3), payload 3 bytes ---
    // count 2; local func 0 (global idx 1 = add) : type 2
    //          local func 1 (global idx 2 = start) : type 0
    0x03, 0x03, 0x02, 0x02, 0x00,
    // --- export section (id 7), payload 15 bytes = 0x0f ---
    0x07, 0x0f, 0x02,
    // "add" kind func idx 1
    0x03, 'a', 'd', 'd', 0x00, 0x01,
    // "start" kind func idx 2
    0x05, 's', 't', 'a', 'r', 't', 0x00, 0x02,
    // --- code section (id 10), payload 16 bytes = 0x10 ---
    0x0a, 0x10, 0x02,
    // add: body len 7
    //   locals_count 0; local.get 0, local.get 1, i32.add, end
    0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b,
    // start: body len 6
    //   locals_count 0; i32.const 42, call 0, end
    0x06, 0x00, 0x41, 0x2a, 0x10, 0x00, 0x0b,
};

void log_heap(const char* where) {
  const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largest_int =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const size_t free_total = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  ESP_LOGI(kTag, "%s: heap internal free=%u largest=%u, total free=%u", where,
           (unsigned)free_int, (unsigned)largest_int, (unsigned)free_total);
}

// The native shim the plugin imports as env.host_publish. On a real plugin
// this would route to PluginManager::publish_state via the ctx; here we just
// log so the chain is visible in the console.
void host_publish_native(wasm_exec_env_t /*env*/, int32_t value) {
  ESP_LOGI(kTag, "host_publish called from plugin: %d", (int)value);
}

void* spike_body(void*) {
  RuntimeInitArgs args{};
  args.mem_alloc_type = Alloc_With_System_Allocator;
  if (!wasm_runtime_full_init(&args)) {
    ESP_LOGE(kTag, "wasm_runtime_full_init failed");
    return nullptr;
  }
  log_heap("after wamr init");

  // Register the native shim. In a PluginEngine the symbols and their
  // function pointers would come from PluginHostCallbacks; here a single
  // static symbol is enough to prove the plumbing. "(i)" is WAMR's signature
  // DSL: one i32 param, no return.
  static NativeSymbol kNatives[] = {
      {"host_publish", (void*)host_publish_native, "(i)", nullptr},
  };
  if (!wasm_runtime_register_natives(
          "env", kNatives, sizeof(kNatives) / sizeof(kNatives[0]))) {
    ESP_LOGE(kTag, "register_natives failed");
    wasm_runtime_destroy();
    return nullptr;
  }
  log_heap("after register_natives");

  // The fast interpreter patches the bytecode in place, so wasm_runtime_load
  // needs a WRITABLE copy — kPluginWasm lives in .rodata (flash).
  uint8_t* wasm_buf = static_cast<uint8_t*>(malloc(sizeof(kPluginWasm)));
  if (!wasm_buf) {
    ESP_LOGE(kTag, "out of heap for wasm copy");
    wasm_runtime_destroy();
    return nullptr;
  }
  memcpy(wasm_buf, kPluginWasm, sizeof(kPluginWasm));

  char err_buf[128]{};
  wasm_module_t module = wasm_runtime_load(wasm_buf, sizeof(kPluginWasm),
                                           err_buf, sizeof(err_buf));
  if (!module) {
    ESP_LOGE(kTag, "wasm_runtime_load: %s", err_buf);
    free(wasm_buf);
    wasm_runtime_destroy();
    return nullptr;
  }
  ESP_LOGI(kTag, "load ok (%u B), instantiating", (unsigned)sizeof(kPluginWasm));

  wasm_module_inst_t inst = wasm_runtime_instantiate(
      module, 4096 /* stack */, 0 /* heap */, err_buf, sizeof(err_buf));
  if (!inst) {
    ESP_LOGE(kTag, "wasm_runtime_instantiate: %s", err_buf);
    wasm_runtime_unload(module);
    free(wasm_buf);
    wasm_runtime_destroy();
    return nullptr;
  }
  log_heap("after instantiate (plugin loaded)");

  wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 4096 /* stack */);

  // Stage 2: call add(3, 4) and expect 7.
  if (wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "add")) {
    uint32_t argv[2] = {3, 4};
    if (!wasm_runtime_call_wasm(env, fn, 2, argv)) {
      ESP_LOGE(kTag, "call add: %s", wasm_runtime_get_exception(inst));
    } else {
      ESP_LOGI(kTag, "add(3, 4) returned %u", (unsigned)argv[0]);
    }
  }

  // Stage 3: call start(), which inside wasm does `host_publish(42)`.
  if (wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "start")) {
    if (!wasm_runtime_call_wasm(env, fn, 0, nullptr)) {
      ESP_LOGE(kTag, "call start: %s", wasm_runtime_get_exception(inst));
    } else {
      ESP_LOGI(kTag, "start() returned");
    }
  }

  wasm_runtime_destroy_exec_env(env);
  log_heap("after both calls");

  wasm_runtime_deinstantiate(inst);
  log_heap("after deinstantiate (runtime kept)");

  wasm_runtime_unload(module);
  free(wasm_buf);
  wasm_runtime_destroy();
  log_heap("after full teardown");
  return nullptr;
}
}  // namespace

void wasm_spike_run() {
  log_heap("before init");
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  pthread_t tid;
  if (pthread_create(&tid, &attr, &spike_body, nullptr) != 0) {
    ESP_LOGE(kTag, "pthread_create failed");
    pthread_attr_destroy(&attr);
    return;
  }
  pthread_attr_destroy(&attr);
  pthread_join(tid, nullptr);
}
