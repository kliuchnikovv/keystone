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

Next: on-hardware UART bring-up (Phase 2), and wiring `RadioCoordinator`+registry into `brain-s3` with fake DECLAREs from `zigbee-c6` (Phase 3 on-device); then **Phase 4 — STATE + COMMAND/CMD_ACK + minimal RuleEngine** (host-testable core).

### ESP-IDF component layout

Firmware apps pick up shared code via `EXTRA_COMPONENT_DIRS=../components`. The wrappers in `firmware/components/{hubproto,core}` reference the same sources as the host build (no duplication); `firmware/components/esp_uart_link` holds the only ESP-IDF-specific transport code. App `main` is C++ (`app_main` is `extern "C"`).
