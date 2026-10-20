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
  `ChipMonitor`, `DesiredState`/`Reconciler`, `TimerTable`/`ScheduleTable`/
  `SceneTable`, `EntityNames`, OTA (`OtaSender`/`OtaReceiver`/`OtaVerifier`), and the
  app-protocol layer (`json.hpp`, `app_protocol.hpp`, `app_requests.hpp`). Portable
  C++17, **zero radio deps**. Host-testable.
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
- `app/` — the phone/web client (Expo, TypeScript): protocol library, screens, a mock
  hub, conformance tools, and the native pinning/mDNS module (`modules/keystone-net`,
  not yet built on a device). See `app/README.md`.
- `scripts/` — `matter-build.sh` (esp-matter Docker build), `gen-hub-cert.sh` (TLS identity).
- `docs/` — per-phase notes (wiring, expected output), `ota.md`, `app-protocol.md`
  (the app protocol and its status), and the backlogs (`backlog.md`, `brain-backlog.md`, `zigbee-backlog.md`,
  `matter-backlog.md`).

## Build & test

**Host (libhubproto + core, no hardware)** — CMake ≥ 3.16, a C++17 compiler; the
first configure fetches doctest (needs network once, cached under `build/_deps`):

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

**App (TypeScript, Node ≥ 22)** — from `app/`: `npm install`, `npm test` (vitest, includes the
UI rendered in jsdom), `npm run typecheck`, `npm run mock-hub` (a fake home to point the app
at). Tools against a real hub: `npm run discover`, `probe`, `hub-check` (see `app/README.md`).

**TLS identity for the brain (optional).** `scripts/gen-hub-cert.sh` creates the hub's own
certificate and key (gitignored, per device) so the app protocol is served over `wss://`;
without it the brain builds and serves plain `ws://` only. See docs/app-protocol.md §10. The same
generated file carries the hub's *setup code* (the password of the first-time setup Wi-Fi
`Keystone-XXXX`, §11): `scripts/gen-hub-cert.sh --setup-code` adds one to an existing identity without
changing the key or certificate; console `setup` shows it and `setup on|off` opens/closes the access point.

**Firmware.** Each app is its own ESP-IDF project. Two toolchains are in use:

- `zigbee-c6` builds with **ESP-IDF 5.5** (native, if `~/esp/esp-idf` is 5.5 — on the Linux
  dev box it is 5.1, so build in the esp-matter Docker image instead:
  `docker run --rm -v $PWD:/project espressif/esp-matter:release-v1.6_idf_v5.5.5 bash -lc
  "cd /project/firmware/zigbee-c6 && idf.py -B build-docker build"`, then flash with
  esptool), because it needs the Zigbee SDK (managed components `espressif/esp-zigbee-lib`
  + `espressif/esp-zboss-lib`), which PlatformIO's bundled IDF was too old for:

  ```bash
  source ~/esp/esp-idf/export.sh
  cd firmware/zigbee-c6
  idf.py set-target esp32c6      # first time only
  idf.py -p <PORT> build flash
  ```

- `brain-s3` builds with **PlatformIO** (`brew install platformio` / `pip install platformio`;
  IDF 6.1, managed components such as `espressif/mdns` are fetched on the first build):

  ```bash
  cd firmware/brain-s3
  pio run -t upload             # port pinned in platformio.ini; override: --upload-port /dev/ttyACM1
  ```

- `thread-c6` (Matter) builds in the esp-matter Docker image: `scripts/matter-build.sh thread-c6`.

The CAM needs an external FTDI (SPEC §10); C6 and S3-Zero flash over USB-C.
Onboard RGB LED GPIO: S3-Zero = 21, C6-DevKitC = 8.

## Hardware gotchas

- **Serial ports** enumerate as `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*` (Linux); the number changes when a
  board is re-plugged (e.g. `1101` → `2101`). On Linux, **always verify by MAC**
  before flashing: `dmesg | grep ttyACM` (the MAC is stable, the port number is
  not). Flashing the wrong board once gave both C6s the same firmware and caused
  88 000+ phantom reboots. `esptool.py -p <PORT> chip_id` also works but requires
  the chip to be in download mode on C6.
- **ESP32-C6 USB-Serial/JTAG** is an on-chip controller. DTR/RTS signals are NOT
  wired to the Reset/Boot pins on the DevKitC, so you **cannot enter download mode
  remotely** — the user must hold the BOOT button while resetting. `esptool.py
  --before usb_reset` resets the chip but boots the app, not the bootloader.
  The console REPL (`esp_console_new_repl_uart`) goes to the UART0 GPIO pins, not
  to the USB port; use `esp_console_new_repl_usb_serial_jtag` for USB access
  (needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, not yet enabled for thread-c6).
- **Capturing the Zigbee console:** the C6 coordinator logs only on events (no
  periodic prints), so a plain reader that attaches after boot sees nothing. Open
  the port with pyserial and pulse `DTR=False; RTS=True` → `RTS=False` to reset
  the chip, then read from boot.
- **Zigbee after a coordinator reboot:** an already-joined device stays in the network but
  does not re-announce, so join-triggered discovery does not re-run. Handled: the device
  table is persisted in NVS and re-discovered by IEEE address on boot (see CLAUDE.md § Status).
- **thread-c6 crash-loop recovery:** if `boot_id` exceeds 50 (= the chip rebooted
  50+ times without NVS being cleared), `app_main` wipes NVS automatically. This
  handles stale Matter fabric data or a corrupt NVS. The chip needs
  re-commissioning in Apple Home after a wipe.
- **UART link wiring:** C6.TX GPIO5 → S3.RX GPIO6, C6.RX GPIO4 ← S3.TX GPIO7,
  common GND (crossed).

## Where to look

- **Design & rationale:** [SPEC.md](SPEC.md) (the source of truth).
- **Current phase status & what's verified:** [CLAUDE.md](CLAUDE.md) § Status.
- **Per-phase wiring/output notes:** `docs/`.
