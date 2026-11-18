# AGENTS.md — keystone

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
- `relay/` — the remote-access relay server, **Go** (one static binary): `policy.go`
  (the security rules, unit-tested), `server.go` (sockets, tunnels, status page),
  `main.go` (flags and environment). `go test ./...` inside it. The wire protocol
  must not change: the hub firmware speaks it.
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
- `relay/` — the relay server in **Go** (`coder/websocket`): one static binary to deploy
  on a VPS for remote access, see [relay/README.md](relay/README.md) and
  [docs/relay-deploy.md](docs/relay-deploy.md). Build and test with `go build ./...` /
  `go test ./...` in that directory (Go ≥ 1.24).
- `scripts/` — `matter-build.sh` (esp-matter Docker build), `gen-hub-cert.sh` (TLS identity),
  `build-relay.sh` (the Go relay binary), `make-label.sh` (the label that goes on the box),
  `kcon.py` (a board's console over USB: batch or interactive, `--watch`), and the relay
  deployment files for a VPS (`keystone-relay.service`, `keystone-relay.env.example`,
  `Caddyfile` — see [docs/relay-deploy.md](docs/relay-deploy.md)).
- `docs/` — per-phase notes (wiring, expected output), `ota.md`, `app-protocol.md`
  (the app protocol and its status), **the roadmap (`roadmap.md` — the single source of
  truth for priorities)**, and the per-subsystem backlogs (`backlog.md`, `brain-backlog.md`, `zigbee-backlog.md`,
  `matter-backlog.md`).
- `docs/design/` — the client app's design system: `keystone-spec.html` (the spec: palette,
  typography, icons, every screen with its states, tables for the engineer), `tokens.json`
  (machine-readable tokens), `icons/` (SVG set generated from the app's own `Glyph.tsx`) and
  `keystone.css`. The design brief and its reconciliation with the running app are in
  `docs/design-brief.md` (§14 lists what changed and why). **Two rules, split:** the
  *visual* requirements of the brief are the source of truth and live in the app (the brief's
  wording stays as written); only *facts* (protocol, data, available operations) are corrected
  in the brief's body — §14.1 lists what was implemented in the app, §14.2 what was corrected
  in the text.

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

**Relay (Go).** From `relay/`: `go test ./...` (29 cases; `go mod download` once — the one
dependency is `coder/websocket`) and `CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o relay .`.
It is the server that gets deployed on a VPS; the Node implementation it replaced was removed.
The secrets are `RELAY_SECRET` / `RELAY_MASTER_SECRET` (the relay derives the secret of every
hub from it) — see [relay/README.md](relay/README.md) and [docs/relay-security.md](docs/relay-security.md).

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

**The Matter build is resource-bounded on purpose.** It compiles ~1700 translation units of
CHIP and links ~400 KB of objects, and it is memory hungry: measured on a Raspberry Pi 5 (4 GB),
**one** compile job already peaks at ~3.6 GB of the host's memory, and every extra job adds
about 2 GB. Two jobs push it over the edge: the kernel compresses pages into **zram** (the Pi's
swap *is* RAM), which is so CPU-heavy that the machine stops answering. The interactive session
(ssh/mosh) dies first, and then systemd's hardware watchdog reboots the Pi
(`RuntimeWatchdogUSec=1min`, `RebootWatchdogUSec=2min`) — so the machine is simply restarted,
with no log of the failure.

So `scripts/matter-build.sh` picks the **job count** from the total RAM: roughly 1.5 GB for
everything else plus 2.2 GB per job. A 4 GB Pi gets one job, a 16 GB workstation four. The
memory limit (`--memory`) is a second line of defence, but it only applies where the kernel has
the memory cgroup controller — this Pi's cmdline carries `cgroup_disable=memory`, so Docker
prints "Your kernel does not support memory limit capabilities" and discards it. The script
checks for the controller and says so when it is missing.

```bash
MATTER_JOBS=4 MATTER_MEM=4g MATTER_CPUS=3 ./scripts/matter-build.sh thread-c6
```

Lower `MATTER_JOBS` first if the build feels heavy — it is the parallelism, not the image, that
costs the memory. Run it in the background (`nohup ... &`) if you want to keep working meanwhile.

**Keep the journal on disk.** The system journal was in `/run/log/journal` (a tmpfs), so every
reboot threw the previous boot's log away — and on this machine the Pi reboots itself when a
build makes it stall, which means exactly the log you want is the one that disappears. Run
`sudo scripts/enable-persistent-journal.sh` once; it keeps the journal in `/var/log/journal`.

⚠️ **Why the obvious fix does not work here — there are TWO traps.** Raspberry Pi OS ships
`/usr/lib/systemd/journald.conf.d/40-rpi-volatile-storage.conf` with `Storage=volatile`
(package `raspberrypi-sys-mods`). A drop-in beats the main config, and the drop-ins are sorted
by **filename** (the directory does not matter), so editing `/etc/systemd/journald.conf` alone
is silently overridden — the journal stays in tmpfs and the script appears to succeed. The
script therefore writes its own drop-in, `60-keystone-persistent.conf` (the range
`journald.conf.d(5)` recommends for the administrator: vendor 10-40, admin 60-90).

The second trap: journald opens `/var/log/journal/<machine-id>/system.journal` and **falls back
to the runtime journal when it cannot** — it does not create that directory itself, and the
stock tmpfiles rule `z /var/log/journal/%m` only *adjusts* the mode of an existing directory
(`z` never creates one). So the per-machine directory has to exist. The script creates it and
declares it in `/etc/tmpfiles.d/60-keystone-journal.conf` (type `d` creates, `z` adjusts) so it
comes back after a reboot.

The script therefore **checks the result** (`journalctl --header` must show
`/var/log/journal/…`) instead of reporting success on faith. If it ever fails again, that
check is the thing to look at.

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
- **Opening the S3's USB port resets it.** The USB-Serial/JTAG reports `rst:0x15
  (USB_UART_CHIP_RESET)` when DTR/RTS change, so attaching a console (e.g. `scripts/kcon.py`)
  reboots the brain. Drive functional tests over the WebSocket/app protocol, not the console;
  the hub reconnects its relay tunnel by itself after such a reset.

## Where to look

- **Design & rationale:** [SPEC.md](SPEC.md) (the source of truth).
- **Current phase status & what's verified:** [CLAUDE.md](CLAUDE.md) § Status.
- **What we are building towards & the order of the work:** [docs/roadmap.md](docs/roadmap.md).
- **The demo procedure, box to working home:** [docs/golden-path.md](docs/golden-path.md).
- **Per-phase wiring/output notes:** `docs/`.
- **Client app design system:** [docs/design/keystone-spec.html](docs/design/keystone-spec.html)
  (open it in a browser) and the brief it implements, [docs/design-brief.md](docs/design-brief.md).
