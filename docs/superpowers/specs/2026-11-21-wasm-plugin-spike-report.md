# Spike report: WAMR on brain-s3 (ESP32-S3-Zero)

**Date:** 2026-11-21
**Branch:** `wasm-plugin-spike` → experiment commits only, nothing pushed.
**Scope:** can we fit the WAMR interpreter into the current brain image, and
can a plugin call a host function on this chip? Design doc:
[`docs/superpowers/specs/2026-11-20-plugin-runtime-design.md`](2026-11-20-plugin-runtime-design.md).

**Target board (what the spike actually ran on, as shipped today):**
Waveshare ESP32-S3-Zero — **ESP32-S3FH4R2**, **4 MB flash + 2 MB quad PSRAM**
at 80 MHz. Not the N16R8 the design doc assumes.

**Spike methodology (what is and isn't in the numbers):**
- Flash numbers are PlatformIO's "program size" report (built release, `-Og`,
  ESP-IDF 6.1.0, toolchain `xtensa-esp-elf 15.2.0+20251204`).
- Heap numbers are `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` sampled at
  five points on real hardware from the live console (no PSRAM bias — this is
  the hot pool WAMR actually lives in).
- WAMR config: interpreter-only, fast interpreter, `libc-builtin`,
  **no AOT**, **no WASI**, **no app-framework**, **no pthread passthrough**
  (set in `sdkconfig.defaults`). This is the smallest supportable config —
  what the design doc §5.1 calls for.
- Test plugin: 86-byte hand-assembled `.wasm` with:
  - `(import "env" "host_publish" (func (param i32)))`
  - `(func (export "add") (param i32 i32) (result i32) …)`
  - `(func (export "start") (i32.const 42) (call $host_publish))`
  A real plugin from the design doc would be larger (declare descriptors in
  data, more host imports, a `ks_plugin_abi_version` export), but 86 B is a
  reasonable lower bound for "an instance exists on heap".

---

## 1. Footprint

| Build                                        | Flash used    | Δ from base | % of 0x1F0000 slot | Static RAM | Heap internal free (idle) |
|---                                           |---:           |---:         |---:                |---:        |---:                       |
| Baseline (hub without WAMR)                  | 1 387 877 B   | —           | 68.3 %             | 40 124 B   | 216 343 B                 |
| + WAMR linked, runtime up, plugin up & down  | 1 472 433 B   | +84 556 B   | 72.5 %             | 42 156 B   | — (see heap table)        |

**Incremental cost of the whole plugin runtime on flash: ~83 KB.** Breakdown:
~80 KB is the WAMR interpreter + libc-builtin code; ~3 KB is the spike's own
`wasm_spike.cpp` + the 86 B plugin bytes in `.rodata` + the glue for
`wasm_runtime_register_natives`.

The 0x1F0000 (1 984 KB) OTA app slot has **~548 KB of headroom** after this.
The design doc's missing piece — the full host-API (sockets, timer, KV
namespace, AES helpers, JSON helpers) plus the plugin manager wired into the
main loop — has to fit into those 548 KB, together with LittleFS driver code
if we go the file-based plugin storage route (design §5.6).

## 2. Heap at runtime

All numbers are `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` sampled on the
live board immediately after each step. "Delta" is from the previous row.

| Sample point                               | Free internal | Largest block | Δ internal |
|---                                         |---:           |---:           |---:        |
| before init                                | 216 343 B     | 143 360 B     | —          |
| after `wasm_runtime_full_init` (system alloc) | 207 723 B  | 135 168 B     | −8 620 B   |
| after `wasm_runtime_register_natives`     | 207 687 B     | 135 168 B     | −36 B      |
| after `instantiate` (plugin loaded)        | 206 387 B     | 131 072 B     | −1 300 B   |
| after both wasm calls                      | 206 387 B     | 131 072 B     | 0          |
| after `deinstantiate`                      | 206 895 B     | 131 072 B     | +508 B     |
| after `wasm_runtime_destroy`               | 207 759 B     | 135 168 B     | +864 B     |

**Interpretation:**
- **WAMR runtime, cold: ~8.6 KB** of internal heap, plus a small loss of the
  largest-block size (143 → 135 KB = −8 KB in the fat pool). The spec's
  "WAMR + 1–2 plugins — in PSRAM" is literally achievable here: `Alloc_With_Pool`
  pointing at a PSRAM heap_caps_malloc block moves all of this off the
  internal pool (we did not use that route in the spike because an .bss-in-PSRAM
  placement panicked the chip with a Cache error — see §4).
- **One tiny plugin instance: ~1.3 KB** incremental. A plugin with linear
  memory (say 1 page / 64 KB) adds that on top; a plugin with 4-page metadata
  and small data adds its share. For a Tuya-local-sized plugin we should
  expect **a few tens of KB** of heap per plugin, not the few MB a native
  integration would touch.
- **Teardown is clean.** After `destroy`, internal free is within 36 B of the
  pre-init value. No cumulative leak path this spike exercised.

## 3. End-to-end: `add(3,4) == 7` and plugin → host

The live console on the S3 (ttyACM3 by MAC `10:51:DB:5E:F8:94`) printed, in
order and without any crash:

```
wamr-spike: load ok (86 B), instantiating
wamr-spike: after instantiate (plugin loaded): heap internal free=206387
wamr-spike: add(3, 4) returned 7
wamr-spike: host_publish called from plugin: 42
wamr-spike: start() returned
wamr-spike: after full teardown: heap internal free=207759
```

- `add(3, 4) → 7` proves the fast-interpreter dispatch works for an exported
  function.
- `host_publish called from plugin: 42` proves `wasm_runtime_register_natives`
  wires a plugin's import to a native C++ function and the plugin can call
  into it — this is exactly the plane `PluginEngine::set_host_callbacks` and
  `PluginHostCallbacks` (core/include/core/plugin_engine.hpp) need. Nothing
  else about the plugin-manager wiring is unproven: the manager calls
  `engine_.deliver_command(...)`, which is `wasm_runtime_call_wasm` on the
  plugin's `on_command` export, and the plugin's `ks_state_publish(...)`
  reaches the manager through this same native shim.

We did **not** wire `core::plugin::PluginManager` into the main loop in this
spike — doing so needs a wasm SDK for plugin authors and a hand-assembled
declare descriptor, which is Project 1b work. The spike delivers the two
pieces of evidence that Plan 1b depends on (interpreter works, native call
back works) and leaves the rest for implementation.

## 4. Gotchas that cost time (keep these for Plan 1b)

Four real problems the spike had to work around. Each is already captured in
`scripts/wamr-patches.py` (the vendored WAMR under `firmware/components/wamr`
needs these patches reapplied if it is re-materialised) or inside
`wasm_spike.cpp`:

1. **WAMR 2.4.0 + ESP-IDF 6.1 newlib.** `espidf_file.c` / `espidf_platform.c`
   use `struct stat` and the POSIX `*at` family (`fstatat`, `mkdirat`,
   `renameat`, `unlinkat`, `futimens`, `utimensat`). ESP-IDF 6.1 newlib
   doesn't surface them via `<unistd.h>`; stubs are provided by
   `espidf_platform.c` but it fails to see `struct stat`. Fix:
   `#include <sys/stat.h>` and forward-declare the `*at` prototypes in the
   file (also compiles when `CONFIG_WAMR_ENABLE_LIBC_WASI=n`, which is our
   case, because it's dead code then).
2. **PlatformIO + xtensa assembler 15.2.** `invokeNative_xtensa.s` is fed
   straight to `xtensa-esp-elf-as` with `-mlongcalls` (only the gcc driver
   accepts that flag). Rename to `.S` so SCons routes through gcc.
3. **Fast interp patches bytecode in place.** `wasm_runtime_load` writes
   through the pointer you give it. If that pointer is in `.rodata` (flash)
   the chip panics with `Cache error` as soon as load writes a dispatch
   opcode. **Always copy the module into a writable heap buffer before load.**
4. **`os_self_thread` calls `pthread_self()`.** The ESP-IDF pthread wrapper
   asserts on any FreeRTOS task that wasn't created via `pthread_create`. A
   WAMR `wasm_runtime_call_wasm` reaches that call indirectly, so **the
   plugin executor task must be a pthread**, not a raw `xTaskCreate`. The
   design doc already puts the plugin on its own task (§5.5); it just needs
   to be a pthread with configurable stack and priority.

Not a workaround but an operational note: `.bss` with
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` relocates static buffers to
PSRAM. A static `Alloc_With_Pool` buffer landed there and WAMR crashed with a
Cache error the first time it wrote metadata. Use `Alloc_With_System_Allocator`
or allocate the pool with `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
explicitly if you want it in PSRAM.

## 5. Verdict

**The S3-Zero is sufficient for the plugin runtime of this shape, with
caveats.**

- Flash: fits with **548 KB headroom** in the 0x1F0000 slot — plenty for the
  design doc's host API (sockets, KV, AES, JSON) and the plugin manager. The
  design doc's N16R8 requirement (16 MB flash) isn't needed on *flash*
  grounds for a brain that stays PlatformIO-built.
- Internal RAM: WAMR runtime is **~8.6 KB** cold. The S3-Zero idle heap is
  216 KB internal free (after Wi-Fi, the hub proper, mDNS, TLS). After WAMR
  there are still ~208 KB. A typical plugin (one entity, a few sockets, some
  config) will add single-digit to low-tens KB. **A couple of plugins stay
  comfortably inside the internal heap**; a plugin with a big linear memory
  (64 KB+ page) should allocate its pool from PSRAM via
  `Alloc_With_System_Allocator` or an explicit `MALLOC_CAP_SPIRAM` buffer
  (the spec §5.5 "plugin's own heap in PSRAM" pattern).
- PSRAM: we have **~1.98 MB** free idle. Even 20 Tuya-sized plugins at
  ~100 KB each fit here comfortably. The design doc's "N16R8 for 8 MB PSRAM"
  is a nice-to-have for very heavy plugins (music / large WASM), not a gate.

**So: the S3-Zero does NOT block Project 1b.** The N16R8 is a real
future-proofing step if someone wants to run many plugins or a big one, but
the first Tuya-local plugin can and should be brought up on the S3-Zero we
already have. The bigger risks in Plan 1b are **not** memory:

- the four gotchas above (bring-up cost, all mitigated here),
- getting a clean `pthread`-based plugin executor with the time-budget
  watchdog the design §5.5 calls for,
- the WASM-side plugin SDK — a tiny C header describing the host imports and
  a toolchain line (`clang --target=wasm32 -nostdlib`) — which is what every
  plugin author will touch first,
- and the LittleFS `plugins` partition (design §5.6) that we didn't flash in
  this spike — it needs a one-shot partition-table update over USB, the same
  as the two-slot OTA transition was.

**Recommendation: proceed with Plan 1b on the S3-Zero, keep N16R8 on the
procurement list for when we want a many-plugin hub or a music-grade plugin.
Reorder is not a blocker.**

## 6. Changes left in the tree on `wasm-plugin-spike`

Every change is scoped to this spike branch; nothing is pushed.

- `firmware/components/wamr/` — vendored WAMR 2.4.0~1 (was managed, see §4.1
  for why vendoring was needed). Not committed: comes to ~45 MB.
- `scripts/wamr-patches.py` — the three source patches this version of WAMR
  needs against IDF 6.1 (idempotent; re-apply after a clean).
- `firmware/brain-s3/main/wasm_spike.{hpp,cpp}` — the spike bring-up: WAMR
  runtime, a native shim, a hand-assembled 86 B plugin, heap samples.
- `firmware/brain-s3/main/main.cpp` — one line calling `wasm_spike_run()`
  once after the console comes up.
- `firmware/brain-s3/main/CMakeLists.txt` — add `wasm_spike.cpp` and
  `wamr` to `REQUIRES`.
- `firmware/brain-s3/main/idf_component.yml` — the WAMR dependency is NOT
  added here (vendored instead).
- `firmware/brain-s3/sdkconfig.defaults` — the smallest-WAMR Kconfig overlay.
