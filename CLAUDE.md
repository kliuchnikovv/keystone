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

Next: **Phase 2 — UART link-up** (`UartTransport` behind `Bus` on S3+C6, exchange HELLO + HEARTBEAT).
