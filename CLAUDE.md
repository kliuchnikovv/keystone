# CLAUDE.md — keystone-esp

Multi-chip ESP32 smart-home hub. **[SPEC.md](SPEC.md) is the source of truth** —
read it before writing code, and never violate the Design Invariants (SPEC §3).

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

**MVP (Phases 0–6): core complete.** Remaining Phase 6 polish (**Stage 2c**): power/energy metering — the IKEA plug also exposes Electrical Measurement (0x0b04) + Metering (0x0702); needs new attr_ids in SPEC §6.1 (e.g. power, energy) + reporting → STATE. Also nice-to-have: boot-time re-discovery of already-joined devices (today an app reflash needs a plug factory-reset to re-trigger discovery).

### ESP-IDF component layout

Firmware apps pick up shared code via `EXTRA_COMPONENT_DIRS=../components`. The wrappers in `firmware/components/{hubproto,core}` reference the same sources as the host build (no duplication); `firmware/components/esp_uart_link` holds the only ESP-IDF-specific transport code. App `main` is C++ (`app_main` is `extern "C"`).
