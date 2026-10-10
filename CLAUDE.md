# CLAUDE.md — keystone-esp

Multi-chip ESP32 smart-home hub. **[SPEC.md](SPEC.md) is the source of truth** —
read it before writing code, and never violate the Design Invariants (SPEC §3).
[AGENTS.md](AGENTS.md) is the tool-agnostic quick-start (project overview, repo
map, build/flash commands, hardware gotchas); this file adds the phase-by-phase
**Status** log below and Claude-specific notes.

Общаемся на русском.

## Layout (SPEC §8)

- `libhubproto/` — shared wire protocol (framing COBS+CRC16, header codec, payloads). Portable C++17, **zero radio/ESP-IDF deps**. Host-testable.
- `core/` — brain: Bus, EntityRegistry, StateMachine, RuleEngine, CommandDispatcher. Portable C++17, **zero radio deps** (must migrate S3 → P4 → Linux). Host-testable.
- `firmware/brain-s3/` — ESP-IDF app, brain (ESP32-S3-Zero).
- `firmware/zigbee-c6/` — ESP-IDF app, Zigbee coordinator (ESP32-C6).
- `firmware/recorder-cam/` — ESP-IDF app, SD event recorder (ESP32-CAM). Phase 7 stub.

## Host build & test (libhubproto + core, no hardware)

Requires CMake ≥ 3.16 and a C++17 compiler. First configure fetches doctest
(needs network once; cached under `build/_deps`).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

macOS: `brew install cmake` if missing.

## Firmware build & flash (ESP-IDF ≥ 5.1)

Each app is its own ESP-IDF project. From the app dir:

```bash
idf.py set-target esp32s3   # brain-s3   (esp32c6 for zigbee-c6)
idf.py build
idf.py -p <PORT> flash monitor
```

C6 and S3-Zero flash over their own USB-C. The CAM needs an external FTDI (SPEC §10).
Onboard RGB LED GPIO: S3-Zero = 21, C6-DevKitC = 8 (adjust in `main.c` if your board differs).

**`zigbee-c6` builds with native ESP-IDF 5.5** (installed at `~/esp/esp-idf`;
`source ~/esp/esp-idf/export.sh` then `idf.py -p /dev/cu.usbmodem1101 build flash`).
It pulls `espressif/esp-zigbee-lib` + `espressif/esp-zboss-lib` (managed
components) — PlatformIO's bundled IDF was too old for the Zigbee SDK. `brain-s3`
still builds/flashes with PlatformIO (`pio run -t upload`, S3=usbmodem101).
The Zigbee console only logs on events; to capture the boot log open the port
with pyserial and pulse DTR=False+RTS=True→RTS=False to reset.

## Conventions

- C++17, 2-space indent, `snake_case` files, warnings-as-signal (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`).
- Build phases are strict and sequential (SPEC §9): do not start a phase before the previous one passes its tests; debug one new thing at a time.
- Protocol/logic changes come with host unit tests. Prefer scaled ints over `float32` on hot paths (SPEC §6.2).
- Keep the `device_type`/`attr_id` table in sync with Matter Device Types (SPEC §6.1).

## Status

- **Phase 0** done: repo layout, host CMake build + doctest, blink apps for S3/C6.
- **Phase 1** done: `libhubproto` complete — CRC16-CCITT, COBS, 8-byte header codec, capability model, and payload codecs (DECLARE, STATE, COMMAND, CMD_ACK) plus the frame layer (`encode_frame` + streaming `FrameReader` with CRC check and 0x00 resync). 38/38 host tests pass. Note: STATE/COMMAND value length depends on each attribute's `value_type`, which is not on the wire — the caller resolves it from the registry (STATE via the `state_reader` pull cursor; COMMAND via an explicit `value_type` arg).

- **Phase 2 (core-side)** done: `core/` gains the transport boundary — `Bus` and `ByteLink` interfaces (SPEC Invariant 1), `UartTransport` (framing + dispatch engine over an injected `ByteLink`, host-tested via `InMemoryLink` loopback). 45/45 host tests pass, core still has zero radio/ESP-IDF deps.
- **Phase 2 (firmware)** written (not yet verified on hardware): `EspUartLink` (ESP-IDF UART `ByteLink`) as a shared component, plus `hubproto`/`core` ESP-IDF component wrappers that reuse the same shared sources. Both `brain-s3` and `zigbee-c6` now run `UartTransport` and exchange HELLO/HEARTBEAT over UART1. Wiring, build/flash, and expected output: [docs/phase2-uart-link.md](docs/phase2-uart-link.md). **Needs a build+flash on real boards to confirm.**

- **Phase 3 (core)** done: `EntityRegistry` (fixed-capacity entity store: declare/find/value_type_of/clear_chip) and `RadioCoordinator` (brain lifecycle: HELLO→QUERY, DECLARE→registry, DECLARE_END→ready, boot_id change→clear+re-QUERY). 56/56 host tests, verified over the `UartTransport`↔`UartTransport` loopback. Core still radio-free.

- **Phase 4 (core)** done: `StateMachine` (applies STATE, edge-triggered change notifications), `CommandDispatcher` (COMMAND routing by chip, CMD_ACK matching by seq, injected-time retries → Unreachable), `RuleEngine` (in-code trigger→predicate→action table). Plus `Value operator==` and `EntityRegistry::set_value/get_value`. Full end-to-end verified over the loopback: STATE → rule fires → COMMAND → CMD_ACK. 72/72 host tests, core still radio-free.

The brain core (Phases 3–4) is a set of composable, radio-free components that all attach to a `Bus`: `EntityRegistry`, `RadioCoordinator`, `StateMachine`, `CommandDispatcher`, `RuleEngine`. Wire them together in an app around one `UartTransport`.

- **Phases 2–4 verified on hardware** (PlatformIO, 2× ESP32, 4 MB): `brain-s3` runs the full core (registry, coordinator, state machine, dispatcher, rules); `zigbee-c6` is a fake device (HELLO→declares occupancy 0x0010 + light 0x0020→reports occupancy STATE→ACKs COMMANDs). Observed on the wire: occupancy STATE → rule → light COMMAND → CMD_ACK Ok, seq-matched, both directions. Firmware build/flash: `cd firmware/<app> && pio run -t upload` (ports pinned in each `platformio.ini`; S3=usbmodem101, C6=usbmodem1101).

- **Phase 5 done (host + hardware)**: `RadioCoordinator` now treats boot_id as the resync primitive on ANY message (not just HELLO) — an unknown chip or a changed boot_id clears that chip's entities and re-queries (self-healing across reboot/watchdog/OTA). `nvs_boot_id` component persists+increments the reboot counter; `zigbee-c6` puts it in every header. 75/75 host tests. On hardware: resetting the C6 bumps boot_id 1→2, the brain re-declares and the rule pipeline resumes.

- **Phase 6 core done (hardware-verified)**: `zigbee-c6` is now a real `esp-zigbee-sdk` coordinator (native ESP-IDF 5.5, on-chip 802.15.4) with the Zigbee→DECLARE/STATE/COMMAND translator, replacing the fake device. On join it discovers a device's endpoints/clusters, reads Basic manufacturer/model, maps each On/Off endpoint to a `switch` entity (numbered from 0x0100), binds + configures On/Off reporting, and sends HELLO so the brain re-runs QUERY→DECLARE (HELLO is the resync primitive for late joins). STATE flows up from On/Off reports; COMMAND(on_off) translates to a ZCL On/Off command down → CMD_ACK. Verified end-to-end with an IKEA INSPELNING smart plug: `brain-s3` (Phase 6 demo harness) toggles the switch every 6 s → real relay switches → CMD_ACK Ok (seq-matched) → On/Off report → STATE back, stable across many cycles. Built on branch `phase6-zigbee`.

**MVP (Phases 0–6): core complete.** Phase 6 polish done: network persistence (Stage 2b, commit `9fd1156`), power metering (Stage 2c), Matter Bridge NVS dedup fix, and boot-time ZDO re-discovery (see below).

- **Phase 6 Stage 2c — power metering (hardware-verified)**: `zigbee-c6` detects the IKEA plug's Electrical Measurement cluster (0x0b04) at join, binds + configures ActivePower reporting (delta ≥1W, 5–30s interval), reads ACPowerMultiplier/Divisor for scaling, and sends `STATE(power_w)` as U16 watts. `thread-c6` adds the Matter Electrical Power Measurement cluster (0x0090) to bridged endpoints that have metering — PowerMode (AC=0) + ActivePower (nullable int64, milliwatts). STATE(power_w) arrives from the brain, is converted W→mW (×1000), and pushed via `attribute::update`. Also fixed a boot race: HELLO now fires after `app_bridge_initialize()` so DECLAREs don't arrive before the bridge infra is ready. Verified end-to-end: IKEA INSPELNING plug → Zigbee report → `zigbee-c6` STATE(power_w=2) → brain forward → `thread-c6` EPM ActivePower=2000 mW.

- **Phase 6 boot-time ZDO re-discovery (hardware-verified)**: on boot, `rearm_restored()` now issues a ZDO NWK Address Request (broadcast by IEEE addr) for each NVS-restored entity instead of blindly trusting the stored short address. The callback (`on_nwk_addr_resp`) updates `short_addr` if it changed, publishes cached STATE for immediate brain visibility, then re-arms reporting and reads fresh state. If a device is offline, the callback logs a warning and the flow defers until that device sends `DEVICE_ANNCE`. Short address changes (e.g. after a coordinator network re-form) are now handled automatically.

- **Phase 6 Stage 2d — Metering cluster / energy_wh (hardware-verified)**: `zigbee-c6` now supports Zigbee Metering cluster (0x0702): detects `CurrentSummationDelivered` at join, declares `energy_wh(0x14, U32)` on the switch entity, binds + configures reporting (UINT48, delta=1 Wh LE, 5 s–1 h), reads Multiplier/Divisor (UINT24, 3 bytes LE) for scaling, and publishes `STATE(energy_wh)` in Wh. Verified on IKEA INSPELNING: Multiplier=1, Divisor=1000 (plug stores mWh, divide by 1000 → Wh), `STATE energy_wh=0` flows correctly. Fixes: SDK crashes on `nullptr` delta for analog types (must pass real UINT48 delta), Multiplier/Divisor are UINT24 not UINT32 (reading 4 bytes gave garbage). Commits `8e03515` + `ea3c...`.

- **Phase 8 Stage C — Matter end-to-end (hardware-verified)**: `thread-c6` is now a full Matter accessory (esp-matter release/v1.6 built via the `espressif/esp-matter:release-v1.6_idf_v5.5.5` Docker image — see [docs/matter-backlog.md](docs/matter-backlog.md) § Route 3): CHIP + NimBLE (BLE commissioning) + on-chip OpenThread FTD + mDNS + one On/Off Light endpoint, mirroring the first Zigbee switch (entity 0x0100). The brain now cross-forwards between chips: COMMAND from chip 0x02 (Matter) → chip 0x01 (Zigbee), STATE from chip 0x01 → mirrored to chip 0x02 (`ChipRouter::kMaxSubs` bumped 4→8 to fit the new subscriber). Verified live: iPhone Apple Home commissioned the C6#2 over BLE with the default esp-matter test-DAC (setup code `3497-0112-332`, discriminator 3840, PIN 20202021), joined Thread, and every toggle in Home now flips the real IKEA plug via Matter → S3 → Zigbee → CmdAck → STATE back → Matter attribute update. This is a full Matter-native path — no cloud, no LAN — through the on-chip Thread radio only. Build: `scripts/matter-build.sh thread-c6`; flash with host esptool (Docker on macOS can't pass through USB).

- **Phase 6 Stage 2e — energy_wh on Matter EEM (hardware-verified)**: `thread-c6` adds the Matter Electrical Energy Measurement cluster (0x0091, features ImportedEnergy+CumulativeEnergy) to bridged endpoints that declare `energy_wh(0x14)`. `electrical_energy_measurement::create()` sets up the cluster; the SDK init callback registers an `ElectricalEnergyMeasurementCluster` server instance in `gServers`. `apply_state()` converts U32 Wh → int64 mWh and calls `NotifyCumulativeEnergyMeasured()`. EEM cluster creation confirmed by log (`added ElectricalEnergyMeasurement cluster to ep 2`); EPM ActivePower=3000 mW flow confirmed end-to-end. Full `STATE(energy_wh)→NotifyCumulativeEnergyMeasured` path activates after ≥1 Wh accumulation (delta threshold). Integration header (`data_model_provider/clusters/electrical_energy_measurement/integration.h`) added via PRIV_INCLUDE_DIRS in CMakeLists. Commits `d3b7c4c`/`84b4a29`.

- **Phase 8 Stage D — Matter Bridge (hardware-verified)**: `thread-c6` is now a proper Matter Bridge — aggregator endpoint (Device Type 0x000E) with dynamic bridged endpoints allocated per registry entity via esp-matter's `app_bridge_*` machinery, persisted in NVS. Our `keystone::BridgedDevice` subclass carries the underlying keystone address (chip_id, entity_id); brain forwards DECLARE (in addition to STATE / COMMAND) from Zigbee → Matter and, when a Matter chip first goes ready, re-plays every known DECLARE so the Bridge can populate itself even for entities that declared before it booted. `on_off_light` cluster wired for Switch device type; other Matter device types are a lookup table away. Requires `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` ≥ 10 (root + aggregator + N bridged); see [docs/matter-backlog.md](docs/matter-backlog.md) step 5 for the roadmap.

- **OTA of the C6 co-processors (zigbee-c6 + thread-c6 hardware-verified)**: the brain downloads an image over HTTP(S) (`ota <chip> <url> <sha256>` console command, `OtaJob`) and streams it over the UART link with a stop-and-wait protocol (hubproto 0x50–0x56, `core::OtaSender` / `OtaReceiver`, portable SHA-256, host-tested incl. lost BEGIN/chunk/END acks). The chip writes the inactive slot via `fw::EspOtaSink`, verifies size + SHA-256, reboots; the bootloader reverts crashing images (`OtaConfirmGate`, 30 s grace). 667 KB in ~78 s. zigbee-c6 and brain-s3 now have a two-slot partition table (needs a one-time USB flash; `zb_storage` kept in place so the Zigbee network survives). thread-c6 (Matter, built in the esp-matter arm64 Docker image, 1.68 MB in 194 s ≈ 8.6 KB/s) uses its existing two 1.92 MB slots and was flashed once over USB. Details: [docs/ota.md](docs/ota.md).
- **Persistent rules (hardware-verified)**: `core::Rule` is now data (`CmpOp` Eq/Ne/Lt/Le/Gt/Ge/Always + typed operand), with `RuleEngine::save/load/remove_rule` (all-or-nothing, host-tested). The brain stores them in NVS (`rules` blob) and edits them with the `rule` console command: `rule add plug power_w > 1500 then plug on_off off`, `rule`, `rule del <n>`. The hard-coded demo rules (occupancy→light) are gone. Verified: rule fires, survives reboot, delete re-indexes.
- **Chip health watchdog (hardware-verified)**: `core::ChipMonitor` judges each C6's liveness from the brain's inbound stream (any frame counts; chips heartbeat every 5 s). 20 s of silence → `LOST` event, `chips` shows `SILENT`, `CommandDispatcher`'s alive filter makes commands to that chip complete as Unreachable immediately (no retries into the void), and the monitor probes it with QUERY every 10 s. boot_id changes are counted as reboots; ≥3 in 5 min logs `FLAPPING` (crash loop, or the bootloader reverting a bad OTA image). Verified by holding the zigbee C6 in the ROM bootloader. Also fixed: thread-c6 echoed the brain's own STATE back as a Matter COMMAND (plug flapped under a state-reacting rule).
- **Desired-state reconciliation (hardware-verified)**: `core::Reconciler` + `DesiredState` remember what the hub last *asked* each attribute to be (any command through `CommandDispatcher::send` — console, timers, rules — plus Matter writes forwarded by the brain; persisted in NVS key `desired`). When the first value is learned after a gap (brain boot, chip reboot cleared the registry) and it differs, ONE corrective COMMAND is sent and `reconcile:` is logged. A later spontaneous change (human pressed the button) is adopted into desired instead of reverted. Desired is never invented from an observed value. Console: `desired`, `desired clear <entity>|all`. Verified: plug pressed off by hand while the brain was down → brain restart put it back on. Limits: a device that power-cycles on its own (outage of just the plug) is not reconciled until its entity is re-learned; physical toggles made during brain downtime are reverted by design.

- **OTA verification (OTA_INFO, hardware-verified on zigbee-c6)**: hubproto 0x57/0x58 + `core::OtaVerifier`. After the chip reboots the brain asks which image it runs and reports VERIFIED / ROLLED BACK / RunningUnconfirmed / NoRecord / NoAnswer in the log and `ota status`. Test hook: `-DKS_TEST_CRASH` image (aborts 8 s after boot) → `FLAPPING` + `ROLLED BACK`. Details: [docs/ota.md](docs/ota.md).
- **Chip self-entities (Phase 8/brain) — hardware-verified** (commit `30f9da2`): every co-processor declares a self entity (`device_type=chip` 0x0A, `entity_id` = its chip_id, so 0x0001/0x0002) and reports uptime, free heap, boot_id, rx/tx frames and CRC errors (attrs 0x70–0x75) as ordinary STATE; `FrameReader::bad_frames()` feeds `crc_errors`. The brain's `chips` command prints the telemetry. Host test: `test_payload_chip_self.cpp`. SPEC §6.1 updated. Flashed all three boards on Linux (S3=ttyACM1 via `pio run -t upload --upload-port`; zigbee-c6 built in the esp-matter Docker image into `build-docker`, because the native `~/esp/esp-idf` here is 5.1; thread-c6 via `scripts/matter-build.sh`; C6s written with esptool, no `erase_flash`). `chips`/`home` show heap, uptime, tx_frames, crc_errors for both chips. Spurious `crc_errors` (355 on thread-c6 after flashing the S3, 8/16 after a plain S3 reset; a thread-c6-only reset gave 0) were noise on the undriven RX line while the peer's TX is high-Z during reset; fixed by `GPIO_PULLUP_ONLY` on the RX pin in `EspUartLink::begin()` (all three apps) — now 0 after both flash and reset. Host `ctest`: 155/155 pass.

- **Matter bridge no longer switches the plug off on a thread-c6 reboot (hardware-verified)**: esp-matter's `on_off_light` defaults `StartUpOnOff` to Off, so each boot forced the bridged endpoint Off (`Toggle ep2 on/off from state 1 to 0`) and the PRE_UPDATE hook forwarded it as `COMMAND on_off=0`. `create_bridged_clusters()` now sets `on_off_lighting.start_up_on_off` to null (keep previous value; the hub owns state). Repro/verify: `on 0x100`, reset thread-c6 over DTR/RTS, `home` — plug stays on.

- **Brain Wi-Fi + SNTP, persisted timers and entity names** (commit `91de2c2`, earlier than the entries above; checked on the live brain: `time` → `synced: yes`, `name` lists `0x0100 -> plug1`): STA Wi-Fi with auto-reconnect (credentials in gitignored `main/wifi_secrets.h`), SNTP → `time`/`tz` console commands; `core::TimerTable` (v2 stores the remaining delay) and `core::EntityNames` persist to NVS (`ent_names`); every console command takes `<id_or_name>`; `name` sets/clears names.

- **Recurring time-of-day schedules (hardware-verified)**: `core::ScheduleTable` (portable, host-tested: firing/once-per-day/late window/weekday mask/save-load/corruption) + brain console `at <HH:MM> <on|off|toggle> <id_or_name> [daily|weekdays|weekends|mon,tue,..]`, `schedules`, `unschedule <n>`; persisted in NVS `schedules`; local time via `tz`, paused until SNTP sync. A schedule fires once per local day within 10 min after its time; the last-fired day is persisted. Verified: `at 19:06 on plug1` fired at 19:06 (plug off → on), a brain reboot inside the window did not fire it again, bad time/days/entity are rejected. 163/163 host tests. Timers and schedules share `run_action()` (toggle resolves against live state).

- **Rule constraints (hardware-verified)**: `core::Rule` gains optional `window_start/end` (local minute of day, may wrap midnight), `cooldown_s` and one extra state condition on another entity (`has_cond/cond_*`); injected `RuleEngine::set_clock` / `set_state_reader` keep the core clock-free; unknown clock/state = rule stays quiet; cool-down only starts when the rule actually fires and is not persisted. NVS `rules` blob is now v2 (36-byte entries); v1 blobs still load. Console: `rule add … then … [between HH:MM-HH:MM] [and <entity> <attr> <op> <value>] [cooldown <sec>]`, `rule` lists them. 174/174 host tests (11 new, incl. v1 migration, wrap-around, corrupt buffers). Verified on the brain: rule outside its window and with a false condition stayed quiet; with window+true condition it fired in 2 s, then a manual `off` stayed off for 80 s (cooldown 120 s) and was undone after the cool-down; rules survive a reboot; bad window/cooldown/entity/keyword are rejected.

- **App protocol, hub side (stage 3, hardware-verified)**: see [docs/app-protocol.md](docs/app-protocol.md). `core/json.hpp` (allocation-free writer + strict bounded reader), `core/app_protocol.hpp` (client decode, value type/range checks, encoders, snapshot split into parts that fit a 4 KB frame) — 40 host tests, 204/204 total; `firmware/brain-s3/main/app_server.{hpp,cpp}` (esp_http_server WebSocket on `ws://<hub>:8080/ws`, plain, development only until TLS; token compare in constant time, 5 s hello timeout, lockout 5 failures/60 s, max 3 clients, per-client backpressure; never touches core, hands decoded commands to the main loop through a queue). Main loop: live `state` events, `chip` events, snapshot re-sent on any structural change (entities, names, rules, schedules, timers) or event-queue overflow; `cmd` → `Dispatcher::send` → `accepted` + `ack` (silent chip: `ack unreachable`). Console: `webtoken [<token>|clear]` (NVS key `webtoken`; the server runs only while a token is set), `heap`. Needs `CONFIG_HTTPD_WS_SUPPORT=y` (in `sdkconfig.brain-s3` and `sdkconfig.defaults`). Verified on the brain with the TypeScript client (`app/`): 20/20 `hub-check` conformance checks, live state/rename/schedule/timer events, command ack `ok` in ~110–230 ms, a held-silent Zigbee chip gives `chip ready→silent` and `ack unreachable`, and recovers. Heap with 3 clients: 210 KB free (211 idle), no leak. Not done yet: management `req` ops, TLS (stage 5), the app screens (stage 4), mDNS (stage 6).

### ESP-IDF component layout

Firmware apps pick up shared code via `EXTRA_COMPONENT_DIRS=../components`. The wrappers in `firmware/components/{hubproto,core}` reference the same sources as the host build (no duplication); `firmware/components/esp_uart_link` holds the only ESP-IDF-specific transport code. App `main` is C++ (`app_main` is `extern "C"`).
