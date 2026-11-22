# Keystone plugins

A Keystone plugin is a **freestanding WebAssembly module**. It has no libc,
no WASI, no filesystem and no network of its own: everything it needs comes
from the hub through the host imports declared in
[`sdk/keystone.h`](sdk/keystone.h).

The hub runs the plugin inside WAMR with a **budget per callback** (100 ms of
wall time and a matching instruction cap). A plugin that loops forever is
interrupted, put into the `faulted` state, and its entities are withdrawn —
**the hub keeps running**. See the design doc
[`docs/superpowers/specs/2026-11-20-plugin-runtime-design.md`](../docs/superpowers/specs/2026-11-20-plugin-runtime-design.md)
§5.3–5.5.

---

## The SDK

[`sdk/keystone.h`](sdk/keystone.h) is the whole contract:

| Host import                | What it does                                    |
|----------------------------|-------------------------------------------------|
| `ks_entity_declare(desc, len)` | declare a virtual entity; returns its id     |
| `ks_state_publish(entity, attr, value, len)` | publish STATE for one of your own entities |
| `ks_log(msg, len)`         | write a line to the hub's log                   |

And the exports the hub calls:

| Export                     | When it is called                              |
|----------------------------|------------------------------------------------|
| `ks_plugin_abi_version()`  | **required** — must return `KS_ABI_VERSION` (1) |
| `on_start()`               | once, when the plugin starts                    |
| `on_stop()`                | when the plugin stops                           |
| `on_command(entity, attr, value, len, seq)` | a COMMAND arrived for one of your entities |

A plugin that does not export `ks_plugin_abi_version`, or exports a version
the hub does not know, is refused.

### The DECLARE payload

`ks_entity_declare` takes the hub's DECLARE payload (SPEC §6.1):

```
u16 entity_id    the hub overrides it — it assigns the id
u8  device_type  KS_DEVICE_SWITCH, KS_DEVICE_LIGHT, ...
u8  attr_count
attr_count x { u8 attr_id, u8 value_type, u8 flags }
```

`ks_build_declare()` in the SDK header fills this in for you.

---

## Building a plugin

You need **clang with the wasm32 target** and **wasm-ld**:

```bash
# Debian / Ubuntu
sudo apt install clang lld
# macOS
brew install llvm
```

Then:

```bash
clang --target=wasm32 -nostdlib -O2 -I plugins/sdk \
      -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
      -Wl,--export=ks_plugin_abi_version -Wl,--export=memory \
      -o my-plugin.wasm my-plugin.c
```

What each flag is for:

| Flag | Why |
|------|-----|
| `--target=wasm32` | the WebAssembly target |
| `-nostdlib` | no libc, no compiler-rt — a plugin is freestanding |
| `-Wl,--no-entry` | wasm-ld must not look for a `_start` |
| `-Wl,--export-dynamic` | keep the plugin's own exports visible |
| `-Wl,--allow-undefined` | the host imports are resolved by the **hub** at runtime, not by the linker |
| `-Wl,--export=ks_plugin_abi_version` | the hub refuses a plugin that does not export it |
| `-Wl,--export=memory` | the host reads the plugin's linear memory |

[`../scripts/build-plugins.sh`](../scripts/build-plugins.sh) builds the
examples in this directory with exactly these flags.

To bake a plugin into the firmware image (the current mechanism — the
LittleFS `plugins` partition is a separate piece of work, design §5.6):

```bash
python3 ../scripts/wasm-to-header.py my-plugin.wasm \
        ../firmware/components/wasm_host/include/wasm_host/my_plugin_wasm.hpp \
        kMyPluginWasm
```

and add it to the `kBundled[]` table in
`firmware/components/wasm_host/wasm_host.cpp`.

---

## The examples

### `test-switch/` — the well-behaved plugin

A wall switch: `on_start` declares a `Switch` with one writable, reportable
`Bool` attribute `on_off`; `on_command` echoes the incoming value back as
the entity's state. It is what the hub ships with, and what the end-to-end
test drives:

```
home>      ->  0xe000 switch chip192: on_off=?
home> on 0xe000
home>      ->  0xe000 switch chip192: on_off=on
home> off 0xe000
home>      ->  0xe000 switch chip192: on_off=off
```

### The same plugin, built with `-DKS_PLUGIN_CRASH=1`

Its `on_command` never returns — it spins in an endless loop. It exists to
prove the watchdog: the hub must notice the overrun, fault the plugin,
withdraw its entity and keep running (design §10).

On the live S3-Zero:

```
home> plugin test.crash
home> on 0xe001
E wamr-eng: on_command trap after 137 ms: Exception: instruction limit exceeded
home> plugins
test.switch: Started
test.crash: Faulted
home> home
5 entities:            <- 0xe001 is gone, the hub is fine
```

---

## How the sandbox actually holds the plugin

* **The core stays single-threaded.** A plugin's host imports do not touch
  the hub's registry or bus directly: they are **marshalled through the main
  loop** and the answer comes back through a semaphore (design §5.5). The
  plugin never sees a pointer into the hub's memory.
* **The plugin cannot declare what it did not ask for.** Every host call is
  capability-checked against the plugin's manifest on every call — the
  plugin is never trusted (design §5.4).
* **The plugin can only touch its own entities.** `ks_state_publish` refuses
  an entity the plugin did not declare.
* **A callback has a budget.** 100 ms of wall time and a matching
  instruction cap (the fast interpreter checks the counter on every opcode
  dispatch, so an endless loop becomes a clean trap). Overrun → the plugin
  is faulted, its entities are withdrawn, the hub carries on.
* **No native code.** A plugin is a `.wasm` module; there is no way to load
  native code into the hub's address space (design §4 — native plugins are
  an anti-goal).
