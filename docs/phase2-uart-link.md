# Phase 2 — UART link-up (S3 ↔ C6)

Brings up the physical UART link between the brain (ESP32-S3-Zero) and the
Zigbee co-processor (ESP32-C6) using the portable `core::UartTransport` over an
`EspUartLink`. The C6 sends `HELLO` on boot then `HEARTBEAT` every second; the
brain logs inbound messages and sends its own `HEARTBEAT`. No Zigbee, registry,
or rules yet.

## Wiring (crossed TX/RX + common GND)

| Signal        | S3-Zero | C6-DevKitC |
|---------------|---------|------------|
| S3 TX → C6 RX | GPIO7   | GPIO4      |
| C6 TX → S3 RX | GPIO6   | GPIO5      |
| GND ↔ GND     | GND     | GND        |

- UART1 is used for the link; UART0/USB stays the console.
- Do **not** use strapping pins for the link (SPEC §10). Adjust the GPIO
  constants at the top of each `main.cpp` if your boards expose different pads.
- Onboard RGB LED blinks once per heartbeat: green on S3, blue on C6.

## Build & flash

From each app directory:

```bash
# brain
cd firmware/brain-s3
idf.py set-target esp32s3
idf.py -p <S3_PORT> flash monitor
```

```bash
# co-processor
cd firmware/zigbee-c6
idf.py set-target esp32c6
idf.py -p <C6_PORT> flash monitor
```

`led_strip` is pulled in as a managed component on first build (needs network).

## Expected output

On the S3 monitor, once both are running and wired:

```
I (…) brain-s3: brain up; heartbeating to chip 0x01
I (…) brain-s3: rx type=0x01 chip=1 boot=1 seq=0 plen=0   # HELLO from C6
I (…) brain-s3: rx type=0x40 chip=1 boot=1 seq=0 plen=0   # HEARTBEAT from C6
...
```

On the C6 monitor you should likewise see `rx type=0x40 ...` (the brain's
HEARTBEAT). If you see nothing inbound: check the TX/RX crossing and common GND
first, then confirm both are on UART1 at 115200.

## What is host-tested vs. on-hardware

`UartTransport` (framing + dispatch) is fully covered by host tests via
`InMemoryLink` (`core/test/test_uart_transport.cpp`). Only `EspUartLink` (the
ESP-IDF UART byte I/O) and the app wiring are hardware-specific and verified by
flashing.
