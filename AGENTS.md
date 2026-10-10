# AGENTS.md — keystone-esp

Guidance for AI coding agents working in this repo (tool-agnostic). Claude Code
also reads [CLAUDE.md](CLAUDE.md); this file holds the stable essentials, while
the **current phase status lives in [CLAUDE.md](CLAUDE.md) § Status** and the
**design is the source of truth in [SPEC.md](SPEC.md)**. Keep volatile progress
notes in CLAUDE.md, not here, so the two files don't drift.

## What this is

A multi-chip ESP32 smart-home hub: a **brain** (ESP32-S3) runs portable,
radio-free logic; **radio co-processors** (ESP32-C6 Zigbee coordinator, later
others) attach over a UART wire protocol. End devices (Zigbee lamps, sensors,
plugs) are distributed; the hub is one box. Long game: expose everything as
Matter-over-WiFi.

## Golden rules

- **Read [SPEC.md](SPEC.md) before writing code**, and never violate the Design
  Invariants (SPEC §3) — e.g. the core stays radio-free so it can migrate
  S3 → P4 → Linux; transports sit behind the `Bus`/`ByteLink` boundary.
- **Communicate in Russian** (Общаемся на русском) — the maintainer's preference.
- **Build phases are strict and sequential** (SPEC §9): don't start a phase before
  the previous one passes its tests, and debug one new thing at a time (protocol
  bring-up separate from radio bring-up).
- **Protocol/logic changes come with host unit tests.** Keep the
  `device_type`/`attr_id` table in sync with Matter Device Types (SPEC §6.1).
- Prefer scaled ints over `float32` on hot paths (SPEC §6.2).

## Repository map (SPEC §8)

- `libhubproto/` — shared wire protocol (COBS+CRC16 framing, 8-byte header codec,
  payload codecs). Portable C++17, **zero radio/ESP-IDF deps**. Host-testable.
- `core/` — brain logic: `Bus`/`ByteLink`, `UartTransport`, `EntityRegistry`,
  `RadioCoordinator`, `StateMachine`, `CommandDispatcher`, `RuleEngine`,
  `ChipMonitor`, `DesiredState`/`Reconciler`, OTA (`OtaSender`/`OtaReceiver`/
  `OtaVerifier`). Portable C++17, **zero radio deps**. Host-testable.
- `firmware/brain-s3/` — ESP-IDF app: the brain (ESP32-S3-Zero).
- `firmware/zigbee-c6/` — ESP-IDF app: the Zigbee coordinator (ESP32-C6).
- `firmware/thread-c6/` — ESP-IDF app: Matter Bridge over on-chip Thread
  (ESP32-C6, chip 0x02); built in the esp-matter Docker image
  (`scripts/matter-build.sh thread-c6`).
- `firmware/recorder-cam/` — ESP-IDF app: SD event recorder (ESP32-CAM), stub.
- `firmware/components/` — shared ESP-IDF wrappers (`hubproto`, `core` reference
  the same sources as the host build; `esp_uart_link` is the only ESP-IDF-specific
  transport; `nvs_boot_id` persists the reboot counter). Apps pick them up via
  `EXTRA_COMPONENT_DIRS=../components`. App `main` is C++ (`app_main` is
  `extern "C"`).
- `docs/` — per-phase notes (wiring, expected output), `ota.md`, and the
  backlogs (`backlog.md`, `brain-backlog.md`, `zigbee-backlog.md`,
  `matter-backlog.md`).

## Build & test

**Host (libhubproto + core, no hardware)** — CMake ≥ 3.16, a C++17 compiler; the
first configure fetches doctest (needs network once, cached under `build/_deps`):

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

**TLS identity for the brain (optional).** `scripts/gen-hub-cert.sh` creates the hub's own
certificate and key (gitignored, per device) so the app protocol is served over `wss://`;
without it the brain builds and serves plain `ws://` only. See docs/app-protocol.md §10.

**Firmware.** Each app is its own ESP-IDF project. Two toolchains are in use:

- `zigbee-c6` builds with **native ESP-IDF 5.5** (installed at `~/esp/esp-idf`),
  because it needs the Zigbee SDK (managed components `espressif/esp-zigbee-lib`
  + `espressif/esp-zboss-lib`), which PlatformIO's bundled IDF was too old for:

  ```bash
  source ~/esp/esp-idf/export.sh
  cd firmware/zigbee-c6
  idf.py set-target esp32c6      # first time only
  idf.py -p <PORT> build flash
  ```

- `brain-s3` builds with **PlatformIO** (`brew install platformio`):

  ```bash
  cd firmware/brain-s3
  pio run -t upload             # port pinned in platformio.ini
  ```

The CAM needs an external FTDI (SPEC §10); C6 and S3-Zero flash over USB-C.
Onboard RGB LED GPIO: S3-Zero = 21, C6-DevKitC = 8.

## Hardware gotchas

- **Serial ports** enumerate as `/dev/cu.usbmodem*`; the number changes when a
  board is re-plugged (e.g. `1101` → `2101`). Identify a board with
  `esptool.py -p <PORT> chip_id`. If a flash says "port is busy or doesn't exist"
  and `ls /dev/cu.usbmodem*` shows nothing, the board de-enumerated — re-plug it.
- **Capturing the Zigbee console:** the C6 coordinator logs only on events (no
  periodic prints), so a plain reader that attaches after boot sees nothing. Open
  the port with pyserial and pulse `DTR=False; RTS=True` → `RTS=False` to reset
  the chip, then read from boot.
- **Zigbee re-pairing:** after a coordinator reboot an already-joined device stays
  in the network but does not re-announce, so join-triggered discovery won't
  re-run until the device is factory-reset and re-paired. (Being addressed by
  persisting the device table in NVS — see CLAUDE.md § Status.)
- **UART link wiring:** C6.TX GPIO5 → S3.RX GPIO6, C6.RX GPIO4 ← S3.TX GPIO7,
  common GND (crossed).

## Where to look

- **Design & rationale:** [SPEC.md](SPEC.md) (the source of truth).
- **Current phase status & what's verified:** [CLAUDE.md](CLAUDE.md) § Status.
- **Per-phase wiring/output notes:** `docs/`.
