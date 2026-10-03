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

### ESP-IDF component layout

Firmware apps pick up shared code via `EXTRA_COMPONENT_DIRS=../components`. The wrappers in `firmware/components/{hubproto,core}` reference the same sources as the host build (no duplication); `firmware/components/esp_uart_link` holds the only ESP-IDF-specific transport code. App `main` is C++ (`app_main` is `extern "C"`).
