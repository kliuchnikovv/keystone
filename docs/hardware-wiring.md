# Hardware wiring — three-board hub

The full hub is three ESP32s soldered together: one brain and two 802.15.4
radio co-processors. Each radio is a separate chip because one 802.15.4 radio
can only run one stack at a time (SPEC §10) — so Zigbee and Thread each get
their own C6.

```
                    ┌──────────────┐
                    │  S3 (brain)  │
                    │  UART0=USB   │
                    │  console     │
                    └───┬──────┬───┘
             UART1  ┌───┘      └───┐  UART2
             GPIO7/6│              │  GPIO8/9
                    ▼              ▼
             ┌──────────┐   ┌──────────┐
             │  C6 #1   │   │  C6 #2   │
             │  Zigbee  │   │  Thread  │
             │ coordinator│  │ br. router│
             │ chip 0x01│   │ chip 0x02│
             └──────────┘   └──────────┘
```

Everyone shares one 5V bus (from S3's USB) and one GND.

## Pin map

### Power (one bus, S3 is the source)

| Signal | S3 pin  | C6 #1 pin | C6 #2 pin |
|--------|---------|-----------|-----------|
| 5V     | 5V (VBUS)| 5V       | 5V        |
| GND    | GND     | GND       | GND       |

Each C6 has its own 3.3V LDO on-board; do **not** feed 3V3 across boards.

### UART 1 — brain ↔ Zigbee C6 (chip 0x01)

| S3         | direction | C6 #1      |
|------------|-----------|------------|
| GPIO7 (TX1)| →         | GPIO4 (RX) |
| GPIO6 (RX1)| ←         | GPIO5 (TX) |

### UART 2 — brain ↔ Thread C6 (chip 0x02)

| S3         | direction | C6 #2      |
|------------|-----------|------------|
| GPIO8 (TX2)| →         | GPIO4 (RX) |
| GPIO9 (RX2)| ←         | GPIO5 (TX) |

C6-side pins are identical on both boards (GPIO4/5), so the firmware constants
`kUartTxGpio=5, kUartRxGpio=4` are the same in `firmware/zigbee-c6` and
`firmware/thread-c6`. The two UARTs differ only on the S3 side.

## Power budget

Peak current, worst case:
- S3 (WiFi TX active): ~150 mA
- C6 #1 (Zigbee TX): ~120 mA
- C6 #2 (Thread + WiFi coexist): ~350 mA

Sum ≈ **~620 mA peak**, average lower. A plain USB-A 500 mA host may sag under
peaks and reboot the S3. Prefer:
- a powered USB hub, **or**
- a USB-C wall adapter (900 mA+), **or**
- external 5V PSU into S3's 5V pin (then don't plug S3 into USB at the same time).

Recommended local decoupling on each C6's 5V input: 100 µF electrolytic +
100 nF ceramic.

## Flashing after soldering

Each C6 keeps its own USB and can still be flashed individually. But when a C6
is powered from S3's 5V bus, plugging in its USB adds a second 5V source — the
DevKitC-1 usually has diodes but not always. Safe recipe:

- **To flash a C6:** unplug S3's USB (so its 5V bus goes down), plug USB into
  the target C6, run `idf.py -p <port> flash`. Then reverse.
- **Alternative** (nicer): put all three boards on a powered USB hub with
  independent USB lines and flash any of them without re-plugging.

## 2.4 GHz coexistence (SPEC §10)

Three 2.4 GHz radios in one box demand deliberate channel and antenna
placement:

- **Zigbee** (C6 #1): pick one and pin it in the coordinator config
  (currently channel 17).
- **Thread** (C6 #2): pick a channel **far from Zigbee** — good candidates are
  15 or 25 (each is 3+ channels away from 17).
- **WiFi** (S3, for Matter): fix the AP channel to 1, 6, or 11. WiFi channels
  overlap 802.15.4 channels; the standard pairing that avoids the worst
  collisions is WiFi ch 1 + Zigbee ch 25/26, or WiFi ch 11 + Zigbee ch 15.
  Adjust once you pick a Thread channel.

Antennas should be separated by at least 5–10 cm, and the two C6 antennas
should not be parallel-close (co-planar) if possible.
