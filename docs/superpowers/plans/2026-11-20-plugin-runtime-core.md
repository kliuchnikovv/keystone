# Plugin Runtime — Core (host-testable) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the portable, host-tested core of the plugin runtime — capability enforcement, manifest parsing, generic plugin config storage, and the plugin lifecycle/bus-glue manager over an abstract engine — so the firmware WAMR layer (Plan 1b) has a concrete, proven contract to implement against.

**Architecture:** Mirror the existing `core/` discipline: everything here is portable C++17, header-only, zero radio/ESP-IDF deps, doctest-tested on the host. The plugin *manager* talks to an abstract `PluginEngine` (realized by WAMR in firmware, by a fake in tests) and to the existing `core::Bus`/`EntityRegistry`. Core stays integration-agnostic: it never knows Tuya exists — plugin config is an opaque versioned blob keyed by plugin id.

**Tech Stack:** C++17, doctest (`core/test/`), existing `core/json.hpp` (strict bounded reader), `core/bus.hpp` / `core/entity_registry.hpp`, CMake.

**Spec:** [docs/superpowers/specs/2026-11-20-plugin-runtime-design.md](../specs/2026-11-20-plugin-runtime-design.md)

**Scope note:** This is Plan **1a** of Project 1. It is fully executable with no hardware. Plan **1b** (firmware component `wasm_host`: WAMR binding, host-import impls, cooperative executor + callback watchdog, LittleFS `plugins` partition, dev-load command, and the Tuya-local `.wasm` plugin with its own config schema) is written after 1a lands, because 1b implements the `PluginEngine` and host-API this plan nails down. The spec's end-to-end "done" criterion (§10) belongs to 1b and is gated on the N16R8 board (being procured).

## Global Constraints

- `core/` stays portable C++17, **zero radio / ESP-IDF deps**, host-testable (SPEC §3 Инвариант 1). No `#include` of WAMR, lwIP, NVS, or esp-* anywhere in these files.
- **Core never knows about Tuya or any specific integration** (design anti-goal B). `plugin_config.hpp` stores an opaque versioned blob per plugin id; it does not parse device fields.
- Capabilities are enforced on **every access**, by the host side, never by trusting the plugin (design §3, §5.4).
- Warnings-as-signal, must compile clean under `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` (links against `keystone_warnings`).
- 2-space indent, `snake_case` filenames, headers in `core/include/core/`, tests in `core/test/test_<name>.cpp` registered in `core/test/CMakeLists.txt`.
- Network capability in v1 is **local subnet only**; the enforcement *decision* (`net_allows`) is host-tested pure logic here, the socket itself is Plan 1b.
- Bounded, all-or-nothing codecs (follow `scene_table.hpp` / `entity_names.hpp` patterns): a truncated or wrong-version buffer fails cleanly, never yields garbage.

## Review Focus

- **Entity-id collision:** a plugin declares an entity id already owned by another chip/plugin → manager must reject or renumber without corrupting the registry. → Task 6.
- **Unknown / ungranted capability in manifest:** manifest names a capability the hub doesn't know, or requests one not allowed → parse fails cleanly, plugin not loaded. → Task 3.
- **Non-local socket target:** plugin asks to reach an IP outside the hub's subnet → `net_allows` denies. → Task 2.
- **Corrupt / old-version config blob (flash rot):** truncated or v0 blob → decode fails cleanly, plugin gets no config rather than garbage. → Task 4.
- **Fault on first `on_start` (before any declare):** plugin traps immediately → manager reaches `faulted` with no half-registered entities, hub continues. → Task 7.

---

### Task 1: Host-API contract (`plugin_host_api.hpp`)

**Files:**
- Create: `core/include/core/plugin_host_api.hpp`
- Test: `core/test/test_plugin_host_api.cpp` (register in `core/test/CMakeLists.txt`)

**Interfaces:**
- Produces: `constexpr uint32_t ks::plugin::kAbiVersion = 1;`
  `enum class KsErr : uint8_t { Ok=0, Denied, BadArg, NoMem, NotFound, Again, Closed };`
  `enum class Cap : uint16_t { NetLocal=1u<<0, BusRead=1u<<1, BusCommand=1u<<2, EntityDeclare=1u<<3, Kv=1u<<4, Aes=1u<<5, Json=1u<<6, Timer=1u<<7, Log=1u<<8 };`
  `struct CapSet { uint16_t bits=0; void add(Cap); bool has(Cap) const; };`

- [ ] **Step 1: Write the failing test** — `test_plugin_host_api.cpp`, TEST_CASE `"plugin host-api: cap set add/has and abi version"`: a default `CapSet` has nothing; after `add(Cap::NetLocal)` it `has(NetLocal)` and not `BusRead`; `kAbiVersion == 1`; `KsErr::Ok == 0`.
- [ ] **Step 2: Run to verify it fails** — `cmake --build build` → compile error (header missing). Expected: FAIL to build.
- [ ] **Step 3: Implement the header** — the constants/enums/`CapSet` above, header-only, `namespace ks::plugin`.
- [ ] **Step 4: Register test + build + run** — add `test_plugin_host_api.cpp` to `core/test/CMakeLists.txt`; `cmake --build build && ctest --test-dir build -R "plugin host-api" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** — `feat(plugin): host-api contract (abi version, errors, capabilities)`.

---

### Task 2: Capability enforcement (`plugin_capabilities.hpp`)

**Files:**
- Create: `core/include/core/plugin_capabilities.hpp`
- Test: `core/test/test_plugin_capabilities.cpp`

**Interfaces:**
- Consumes: `CapSet`, `Cap`, `KsErr` (Task 1).
- Produces:
  `struct Subnet { uint32_t ip; uint32_t mask; };  // host byte order`
  `bool net_allows(const Subnet& local, uint32_t target_ip);  // target in same subnet`
  `struct EntityAccess { bool own_only=true; const uint16_t* allow=nullptr; size_t allow_n=0; };`
  `bool entity_allowed(const EntityAccess&, uint16_t entity_id, bool is_owned_by_plugin);`
  `KsErr check_net(const CapSet&, const Subnet& local, uint32_t target_ip);  // Denied if no NetLocal or not local`

**Decisions for the implementer:** `net_allows` is pure: `(local.ip & local.mask) == (target_ip & local.mask)`. `entity_allowed` returns true if `is_owned_by_plugin`, else membership in `allow[0..allow_n)`.

- [ ] **Step 1: Write the failing test** — TEST_CASE `"plugin caps: net local-only and entity access"`: with `local={192.168.1.10, /24}`, `net_allows` true for `192.168.1.50`, false for `8.8.8.8`; `check_net` with no `NetLocal` cap → `Denied` even for a local target; with `NetLocal` → `Ok` local / `Denied` non-local; `entity_allowed` true for an owned id, false for a non-owned id absent from `allow`, true when that id is in `allow`.
- [ ] **Step 2: Run to verify it fails** — build; FAIL (header missing).
- [ ] **Step 3: Implement** the three functions above, header-only.
- [ ] **Step 4: Register + build + run** — `ctest -R "plugin caps"`. Expected: PASS.
- [ ] **Step 5: Commit** — `feat(plugin): capability enforcement (local-subnet net, entity access)`.

---

### Task 3: Manifest parse/validate (`plugin_manifest.hpp`)

**Files:**
- Create: `core/include/core/plugin_manifest.hpp`
- Test: `core/test/test_plugin_manifest.cpp`

**Interfaces:**
- Consumes: `CapSet`, `Cap`, `kAbiVersion` (Task 1); `core/json.hpp` reader.
- Produces:
  `struct Manifest { char id[24]; uint32_t version; uint32_t abi; CapSet caps; uint16_t allow[8]; size_t allow_n; };`
  `enum class ManifestError { Ok, BadJson, MissingField, AbiIncompatible, BadCapability, TooLong };`
  `ManifestError parse_manifest(const char* json, size_t len, Manifest& out);`

**Decisions:** manifest JSON has `id` (string ≤23), `version` (u32), `abi` (u32), `caps` (array of known capability names → map to `Cap`), optional `allow` (array of entity ids ≤8). Reject `abi != kAbiVersion` with `AbiIncompatible`; an unknown capability name → `BadCapability`; missing `id`/`version`/`abi` → `MissingField`; malformed → `BadJson`; overlong id/arrays → `TooLong`. Use the existing strict bounded reader; do not add a new JSON lib.

- [ ] **Step 1: Write the failing test** — TEST_CASE `"plugin manifest: parse, abi check, bad capability"`: a valid manifest (`abi=1`, caps `["net_local","entity_declare","aes","json"]`) → `Ok`, fields populated, `caps.has(NetLocal)`; `abi=2` → `AbiIncompatible`; caps `["net_local","teleport"]` → `BadCapability`; missing `id` → `MissingField`; `"{bad"` → `BadJson`; 40-char id → `TooLong`.
- [ ] **Step 2: Run to verify it fails** — build; FAIL.
- [ ] **Step 3: Implement** `parse_manifest` using `core/json.hpp`; a static name→`Cap` table.
- [ ] **Step 4: Register + build + run** — `ctest -R "plugin manifest"`. Expected: PASS.
- [ ] **Step 5: Commit** — `feat(plugin): manifest parse and validation`.

---

### Task 4: Generic plugin config codec (`plugin_config.hpp`)

**Files:**
- Create: `core/include/core/plugin_config.hpp`
- Test: `core/test/test_plugin_config.cpp`

**Interfaces:**
- Produces (opaque blob, core does NOT parse contents):
  `size_t encode_plugin_config(const char* plugin_id, const uint8_t* body, size_t body_len, uint8_t* buf, size_t cap);  // 0 on overflow`
  `enum class ConfigError { Ok, BadVersion, Truncated, IdMismatch, TooLong };`
  `ConfigError decode_plugin_config(const uint8_t* buf, size_t len, const char* want_id, const uint8_t** body_out, size_t* body_len_out);`

**Decisions:** blob = `[version:u8=1][id_len:u8][id bytes][body_len:u16 LE][body...]`. All-or-nothing: any short read → `Truncated`; `version!=1` → `BadVersion`; stored id != `want_id` → `IdMismatch`. `body_out` points into `buf` (no copy). Mirrors `scene_table`/`entity_names` bounded-codec style.

- [ ] **Step 1: Write the failing test** — TEST_CASE `"plugin config: opaque blob round-trip and corruption"`: encode `(id="tuya", body=<7 arbitrary bytes>)` then decode with `want_id="tuya"` → `Ok`, body matches; decode a 1-byte-truncated buffer → `Truncated`; flip the version byte → `BadVersion`; decode with `want_id="other"` → `IdMismatch`; encode into a too-small `cap` → returns 0.
- [ ] **Step 2: Run to verify it fails** — build; FAIL.
- [ ] **Step 3: Implement** both functions, header-only, little-endian via `core/endian.hpp` if present.
- [ ] **Step 4: Register + build + run** — `ctest -R "plugin config"`. Expected: PASS.
- [ ] **Step 5: Commit** — `feat(plugin): generic opaque per-plugin config codec`.

---

### Task 5: Plugin engine interface (`plugin_engine.hpp`)

**Files:**
- Create: `core/include/core/plugin_engine.hpp`

**Interfaces:**
- Consumes: `KsErr` (Task 1).
- Produces — the abstract boundary the firmware WAMR layer (Plan 1b) implements and the manager (Task 6) drives:
  `struct PluginInstance;  // opaque handle, engine-owned`
  `class PluginEngine {`
  ` public: virtual ~PluginEngine()=default;`
  ` virtual PluginInstance* load(const uint8_t* wasm, size_t len)=0;  // nullptr on bad module`
  ` virtual KsErr start(PluginInstance*)=0;    // runs on_start under the callback watchdog`
  ` virtual void stop(PluginInstance*)=0;`
  ` virtual KsErr deliver_command(PluginInstance*, uint16_t entity, uint16_t attr, const uint8_t* val, size_t len, uint8_t seq)=0;`
  ` virtual KsErr deliver_state(PluginInstance*, uint16_t entity, uint16_t attr, const uint8_t* val, size_t len)=0;`
  ` virtual void unload(PluginInstance*)=0; };`
  `// Callbacks the engine fires back INTO the manager (set once):`
  `struct PluginHostCallbacks {`
  ` uint16_t (*declare_entity)(void* ctx, const uint8_t* desc, size_t len);`
  ` KsErr (*publish_state)(void* ctx, uint16_t entity, uint16_t attr, const uint8_t* val, size_t len);`
  ` KsErr (*send_command)(void* ctx, uint16_t entity, uint16_t attr, const uint8_t* val, size_t len, uint8_t seq);`
  ` void (*on_fault)(void* ctx, const char* reason); };`

**No standalone test** — this is a pure interface. It is exercised by the `FakePluginEngine` built inside Task 6's test. Verified by: compiles as part of the build (Step below).

- [ ] **Step 1: Write the header** (interface + callbacks struct above), header-only, `namespace ks::plugin`.
- [ ] **Step 2: Build** — `cmake --build build`. Expected: compiles clean (no test yet).
- [ ] **Step 3: Commit** — `feat(plugin): abstract PluginEngine interface and host callbacks`.

---

### Task 6: Plugin manager — lifecycle & bus glue (`plugin_manager.hpp`)

**Files:**
- Create: `core/include/core/plugin_manager.hpp`
- Test: `core/test/test_plugin_manager.cpp` (also defines a `FakePluginEngine`)

**Interfaces:**
- Consumes: `PluginEngine`, `PluginHostCallbacks`, `KsErr` (Task 5); `Manifest`, `CapSet` (Tasks 3,1); `EntityAccess`/`check_net` (Task 2); `core::Bus`, `core::EntityRegistry`.
- Produces:
  `enum class PluginState { Installed, Started, Stopped, Faulted };`
  `class PluginManager {`
  ` public: PluginManager(PluginEngine&, Bus&, EntityRegistry&);`
  ` bool install(const Manifest&, const uint8_t* wasm, size_t len);  // load+register slot`
  ` KsErr start(const char* id);  void stop(const char* id);`
  ` PluginState state(const char* id) const;`
  ` // host callbacks (wired into the engine): declare → registry (owner = plugin), state → bus, command gated by caps`
  ` void on_bus_command(uint16_t entity, uint16_t attr, const uint8_t* val, size_t len, uint8_t seq); };`

**Decisions:** a declared entity is tagged with a plugin-owner marker (reuse the Matter-bridge-style owner convention; concrete id range is a builder decision per spec §9). On declare, if the id collides with an existing registry entity the manager assigns the next free id in the plugin range (never overwrites). COMMAND whose entity is owned by a started plugin is routed to `engine.deliver_command`, gated by `check`/`entity_allowed`; STATE from the plugin publishes through the bus exactly like a chip. `FakePluginEngine` in the test records calls and lets the test fire host callbacks (declare/state) and faults on demand — the `InMemoryLink`-style double for this layer.

- [ ] **Step 1: Write the failing test** — `test_plugin_manager.cpp`, TEST_CASE `"plugin manager: install, start, declare, command, state"`: install+start a fake plugin; have it declare one entity → it appears in the `EntityRegistry` owned by the plugin; a COMMAND on that entity reaches `FakePluginEngine::deliver_command`; a STATE pushed by the plugin via the host callback is visible on the bus/registry. Second TEST_CASE `"plugin manager: declare colliding id is renumbered, registry intact"`: pre-populate the registry with an id, have the plugin declare the same id → manager assigns a different id, original entity untouched.
- [ ] **Step 2: Run to verify it fails** — build; FAIL.
- [ ] **Step 3: Implement** `PluginManager` + wire `PluginHostCallbacks` to registry/bus; collision renumber.
- [ ] **Step 4: Register + build + run** — `ctest -R "plugin manager"`. Expected: PASS.
- [ ] **Step 5: Commit** — `feat(plugin): plugin manager lifecycle and bus glue`.

---

### Task 7: Fault containment & quarantine (extend `plugin_manager.hpp`)

**Files:**
- Modify: `core/include/core/plugin_manager.hpp`
- Test: `core/test/test_plugin_manager.cpp` (add cases)

**Interfaces:**
- Produces: `void on_plugin_fault(const char* id, const char* reason);` plus quarantine policy: N faults within a window → stays `Faulted`, no auto-restart (mirror `ChipMonitor` flapping).

**Decisions:** fault → state `Faulted`, entities owned by that plugin are withdrawn from the registry, the hub (other plugins, chips, bus) is untouched. Restart is manual (`start`); after K consecutive faults the manager refuses to restart (quarantine) and records it. K and the window default per spec §9 (builder numbers).

- [ ] **Step 1: Write the failing test** — TEST_CASE `"plugin manager: fault on first on_start leaves hub intact"`: a `FakePluginEngine` whose `start` reports a fault before any declare → manager state `Faulted`, no entities registered for it, a pre-existing unrelated entity still present and a COMMAND to another (non-plugin) entity still dispatches. TEST_CASE `"plugin manager: repeated faults quarantine"`: fault it K times → `start` is refused and state stays `Faulted`.
- [ ] **Step 2: Run to verify it fails** — build; FAIL.
- [ ] **Step 3: Implement** fault handling, entity withdrawal, quarantine counter.
- [ ] **Step 4: Build + run** — `ctest -R "plugin manager"`. Expected: PASS (all manager cases).
- [ ] **Step 5: Commit** — `feat(plugin): fault containment and quarantine`.

---

### Task 8: Full suite green + plan 1b seam note

**Files:**
- Modify: `docs/superpowers/specs/2026-11-20-plugin-runtime-design.md` (append a short "ABI frozen for 1b" note listing the exact `plugin_engine.hpp` interface and `kAbiVersion`).

- [ ] **Step 1: Run the whole suite** — `cmake --build build && ctest --test-dir build --output-on-failure`. Expected: all pass, including every pre-existing `core` test (no regressions). Record the count.
- [ ] **Step 2: Confirm zero radio/ESP-IDF deps** — `grep -rniE "wamr|lwip|esp_|nvs_|freertos" core/include/core/plugin_*.hpp` → no matches.
- [ ] **Step 3: Append the ABI-frozen note** to the design doc so Plan 1b implements against a fixed `PluginEngine`/`kAbiVersion`.
- [ ] **Step 4: Commit** — `docs(plugin): freeze core plugin ABI for firmware plan 1b`.

---

## Self-review notes

- **Spec coverage:** §5.1 (core files) → Tasks 1–7; §5.4 capabilities → Task 2; §5.5 execution/bus-boundary → the manager marshals via host callbacks, the *task/queue* realization is Plan 1b (firmware) by design; §6 host-API contract → Task 1 (+ 1b for the wire-level import numbering); §5.6 storage → Plan 1b; §7 Tuya → Plan 1b; §9 resolved answers (local-only, manual config shape in the plugin, no signing) → honored (net local-only in Task 2; config opaque in Task 4; no signing code here).
- **Hardware-gated items deliberately deferred to Plan 1b:** WAMR bring-up, sockets, cooperative watchdog, LittleFS partition, dev-load, Tuya `.wasm`, end-to-end §10.
- **Proportion:** plan is close to the spec's length; task bodies carry signatures + test names/assertions, not implementations.
