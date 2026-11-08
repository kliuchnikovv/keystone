# SPEC.md — Multi-Chip ESP32 Smart-Home Hub

> Source of truth for the project. Written to be implemented incrementally by
> Claude Code. Read this fully before writing code. Do not violate the
> **Design Invariants** (section 3) — they are the reason the architecture
> survives future hardware changes.

общаемся на русском
---

## 1. What we are building

A local-first smart-home hub, in C++ on ESP-IDF, running across several ESP32
chips inside one enclosure. It reimplements the *core* of Home Assistant — an
event bus, an entity registry with a state machine, and a rule engine — but
**not** HA's integration ecosystem, history database, template engine, or web UI. The hub
serves no UI of its own: a separate phone/web app (`app/`) talks to it over a small
authenticated WebSocket+JSON protocol ([docs/app-protocol.md](docs/app-protocol.md)).

It is a **centralized hub**, not a distributed mesh. All "brain" chips live in
one box, wired together. What is distributed is the *end devices* (Zigbee lamps,
BLE sensors, DIY nodes), reached over radio. This is a deliberate trade: the box
is a single point of failure, in exchange for a much simpler system (no
distributed consensus, no lossy-radio sync protocol, rules run in one place).

### Non-goals (explicitly out of scope for now)
- Distributed / no-SPOF core. Rejected in favor of the simpler hub model.
- ESP-NOW between our own boards. Not needed — boards are wired.
- Matter **controller** at scale (see section 9). Prototype-tier only on ESP.
- A Thread border router / Thread controller. (Matter-over-Thread *accessory bridging* exists: chip 0x02, Phase 8.)
- History DB beyond a simple event log on SD.

---

## 2. Architecture

### 2.1 Topology: brain + radio co-processors

One **brain** chip runs the core (registry, state machine, rules). Every other
chip is a **radio co-processor** that owns one wireless protocol and translates
its devices to/from a common wire protocol spoken to the brain over a physical
link (UART now, SPI/USB later).

```
                        +------------------+
                        |   BRAIN (S3)     |
                        |  - entity registry
                        |  - state machine |
                        |  - rule engine   |
                        |  - WiFi + Matter-over-WiFi (later)
                        +---+----+-----+---+
                 UART/wire |    |     | UART/wire
              +------------+    |     +------------+
              |                 |                  |
        +-----+-----+   +-------+------+    +-------+------+
        | C6        |   | CAM          |    | (spare C6 /  |
        | Zigbee    |   | recorder(SD) |    |  BLE / etc.) |
        | coordinator   | + camera     |    |              |
        +-----+-----+   +--------------+    +--------------+
              | 802.15.4                         
        Zigbee devices around the home
```

### 2.2 Roles per chip (current inventory)

| Chip                | Role                                                        | Radio it owns        |
|---------------------|-------------------------------------------------------------|----------------------|
| ESP32-S3-Zero #1    | **Brain**: registry + state machine + rules; WiFi; Matter-over-WiFi bridge (later) | WiFi + BLE |
| ESP32-C6 #1         | **Zigbee coordinator** (`esp-zigbee-sdk`)                   | 802.15.4 (Zigbee)    |
| ESP32-CAM           | **Recorder**: passive bus listener → event log on SD; camera/motion → events | — (wired) |
| ESP32-S3-Zero #2    | Spare: BLE bridge, or time/NTP border node                  | WiFi + BLE           |
| ESP32-C6 #2         | **Matter Bridge over on-chip Thread** (chip 0x02, `esp-matter`): mirrors registry entities as Matter accessories | 802.15.4 (Thread) + BLE |
| ESP32-C6 #3..#5     | Spare radio (replacements)                                  | WiFi/BLE/802.15.4    |
| nRF24L01+           | Transport for **DIY nodes only** (proprietary, not a standard SH protocol). Attaches via SPI to a chip that bridges its packets to the brain as normal DECLARE/STATE. | 2.4 GHz proprietary |

The recorder is **read-only for the bus**: it only receives, never commands. If
it dies, home logic keeps working; only history is lost. Keep it that way.

---

## 3. Design Invariants (do not break these)

1. **Transport is an abstraction.** Core logic talks to a `Bus` interface
   (`publish(msg)` / `subscribe(...)`), never directly to UART/SPI/USB. Swapping
   the physical link must not touch the registry, state machine, or rule engine.
2. **The protocol is topology-independent.** Do not bake "one wire per chip" into
   any layer above the frame layer. `chip_id` stays in the header even on
   point-to-point links, so moving to a shared bus later is a transport change,
   not a rewrite.
3. **The brain is separable from radio stacks.** The core must be portable so the
   brain can migrate S3 → ESP32-P4 → Linux SBC later, with radio chips becoming
   co-processors of the new brain. Keep zero radio-stack dependencies in core code.
4. **One capability descriptor feeds both the internal registry and the outward
   Matter bridge.** Model entities in Matter-cluster shape from day one (section 6)
   so the bridge is near-free and nothing is described twice.
5. **The core has no radio.** All wireless lives on co-processors. The brain only
   computes and orchestrates.

---

## 4. Inter-chip protocol

Three layers: **frame** (transport-level), **message** (envelope), **model**
(capabilities). This is the most valuable and most portable part of the whole
project — it outlives every brain/transport change. Implement it as a shared
library (`libhubproto`) used identically on brain and co-processors.

### 4.1 Layer 1 — framing

Byte streams (UART/SPI) have no message boundaries and can corrupt. Use **COBS**
(Consistent Overhead Byte Stuffing): it removes `0x00` from the payload so a
single `0x00` byte unambiguously delimits frames and the parser self-resynchronizes
after any glitch.

```
Frame on the wire:
  [ COBS( HEADER || PAYLOAD || CRC16 ) ]  0x00

  - CRC16 = CRC-16/CCITT-FALSE over (HEADER || PAYLOAD), computed BEFORE COBS.
  - COBS encodes (HEADER || PAYLOAD || CRC16); the trailing 0x00 is the delimiter.
  - Receiver: read until 0x00, COBS-decode, verify CRC16, drop frame on mismatch.
```

CRC is mandatory on UART; keep it on SPI too (cheap insurance against edge glitches).

### 4.2 Layer 2 — message header (8 bytes, little-endian)

```
offset size field       meaning
0      1    version     protocol version, starts at 0x01
1      1    type        message type (section 4.3)
2      1    chip_id     identity of the OTHER endpoint (0x00 = brain,
                        0x01..0xFE = radio chips, 0xFF = broadcast/unassigned)
3      1    boot_id     sender's reboot counter (persisted in NVS, ++ each boot)
4      2    seq         request/response matching (LE)
6      2    entity_id   subject entity (LE; 0x0000 = about the chip itself)
```

**Role of these fields on a reliable wire (different from the ESP-NOW design):**
- The wire is not lossy — a frame either arrives whole with valid CRC or not at
  all. So `seq` is **not** for loss detection; it matches a `COMMAND` to its
  `CMD_ACK`.
- `boot_id` is the resync primitive. A co-processor increments its NVS-stored
  `boot_id` on every boot. When the brain sees a changed `boot_id` from a chip,
  it treats that chip's prior entity declarations as invalid and re-queries
  (see 4.4). Self-healing across crashes / watchdog resets / OTA.

### 4.3 Message types

```
0x01 HELLO        chip→brain   "I booted; here is my chip_id and boot_id"
0x02 DECLARE      chip→brain   one entity + its capability descriptor
0x03 DECLARE_END  chip→brain   "I have declared all my entities"
0x10 STATE        chip→brain   an entity's attribute value(s) changed
0x20 COMMAND      brain→chip   set attribute Y on entity X
0x21 CMD_ACK      chip→brain   result of COMMAND seq=K (ok / fail / no-response)
0x30 QUERY        brain→chip   "(re)declare all your entities"
0x40 HEARTBEAT    both         liveness ping; absence over N intervals = chip down
0x50..0x58 OTA_*  both         firmware update of a co-processor over the link
                               (BEGIN/CHUNK/END + acks, ABORT, INFO_REQ/INFO);
                               see docs/ota.md and libhubproto payload.hpp
0x59..0x5C DEV_OTA_* both      firmware update of a Zigbee *device*: the brain
                               pushes the vendor .ota file to the coordinator,
                               which serves it over the OTA Upgrade cluster
                               (BEGIN/CHUNK/END + STATUS); see docs/ota.md
0x60 MATTER_PAIR brain→chip   commission a Matter device: kind(u8) + bytes[]
0x61 MATTER_RESULT chip→brain the result of a commissioning attempt
0x62 THREAD_DATASET_REQ brain→chip "give me your active Thread dataset"
0x63 THREAD_DATASET chip→brain the dataset, as Thread operational TLVs
                               (the brain has no 802.15.4 radio, so the
                               commissioner borrows the co-processor's
                               network); see docs/matter-backlog.md §3
```

### 4.4 Lifecycle

```
Boot / link-up:
  chip → HELLO(chip_id, boot_id)
  brain → QUERY
  chip → DECLARE (xN)
  chip → DECLARE_END
Runtime:
  chip → STATE            (device changed)
  brain → COMMAND ; chip → CMD_ACK   (brain drives a device)
  both → HEARTBEAT        (background liveness)
Resync:
  chip reboots → new HELLO with incremented boot_id
  brain drops that chip's entities, re-QUERYs   (self-healing)
```

Critical commands (locks, boilers) rely on `CMD_ACK`: no ACK within timeout ⇒
chip is stuck ⇒ brain escalates (retry, then alert). Non-critical `STATE` updates
need no ACK — the next report supersedes.

---

## 5. Payload formats

All multi-byte integers little-endian.

```
HELLO payload:
  (none beyond header; chip_id and boot_id are in the header)

DECLARE payload:
  uint16 entity_id          # canonical here; also mirrored in header
  uint8  device_type        # section 6
  uint8  attr_count
  Attribute[attr_count]:
    uint8 attr_id           # semantics scoped to device_type
    uint8 value_type        # section 6.2
    uint8 flags             # bit0 readable, bit1 writable, bit2 reportable

DECLARE_END payload:
  (none)

STATE payload:
  uint16 entity_id
  uint8  attr_count
  ValueEntry[attr_count]:
    uint8 attr_id
    <value bytes per value_type>

COMMAND payload:
  uint16 entity_id
  uint8  attr_id
  <value bytes per value_type>

CMD_ACK payload:
  uint8  status             # 0=ok 1=fail 2=unreachable 3=unsupported

HEARTBEAT payload:
  (none)
```

---

## 6. Capability model (the shared language)

The model must be **Matter-cluster-shaped** so a single descriptor serves the
internal registry, the rule engine, AND the outward Matter-over-WiFi bridge.
`device_type` maps 1:1 to a Matter Device Type; each `attr` maps to a Matter
cluster/attribute. Describe an entity once → it exists internally and can be
exposed as a Matter accessory with no second modeling pass.

### 6.1 device_type ↔ Matter mapping (initial set)

| device_type | value | Matter Device Type | Typical attributes (attr_id) |
|-------------|-------|--------------------|------------------------------|
| light       | 0x01  | On/Off, Dimmable, Color-Temperature or Extended Color Light | on_off(0x01), brightness(0x02), color_xy(0x03), color_temp(0x04) |
| sensor      | 0x02  | Temperature / Humidity / Light Sensor | temperature(0x10), humidity(0x11), illuminance(0x12), battery(0x1F) |
| switch      | 0x03  | On/Off Light (a plug-in unit today) | on_off(0x01), power_w(0x13), energy_wh(0x14) |
| cover       | 0x04  | Window Covering     | position(0x20), state(0x21) — not implemented yet |
| lock        | 0x05  | Door Lock          | locked(0x30) — not implemented yet |
| climate     | 0x06  | Thermostat (heating) | temperature(0x10), setpoint(0x40), mode(0x41) |
| occupancy   | 0x07  | Occupancy Sensor   | occupied(0x50) |
| contact     | 0x08  | Contact Sensor     | contact(0x51) |
| button      | 0x09  | Generic Switch     | event(0x60) |
| chip        | 0x0A  | — (not bridged; may later map to Matter diagnostics clusters) | uptime_s(0x70, u32), heap_free(0x71, u32), boot_id(0x72, u8), rx_frames(0x73, u32), tx_frames(0x74, u32), crc_errors(0x75, u16), thread_role(0x76, u8), thread_children(0x77, u8) |

**How a Zigbee endpoint becomes one of these** (`core/zcl_profile.hpp`): the clusters decide,
and so does the metering. An endpoint that reports its **own** power draw (Electrical
Measurement / Metering) is a socket — `switch`, with on_off, power_w and energy_wh and no
brightness — whatever else it exposes. IKEA's INSPELNING control outlet also reports a Level
cluster (it shares its firmware with the bulbs), so "On/Off + Level ⇒ a dimmable light" used
to turn a plug into a lamp. A lamp never measures its own consumption: the socket does.
The rule is applied from the stored capabilities too, so a device paired with an older
firmware is corrected on the next coordinator boot instead of needing a re-pair.

**Attribute units** (`libhubproto/include/hubproto/attrs.hpp`; Zigbee and Matter agree on most of
them, the rest are converted by `hubproto/units.hpp`):

| attr | value_type | meaning |
|------|------------|---------|
| on_off | bool | |
| brightness | u8 | 0..254 (ZCL/Matter CurrentLevel) |
| color_temp | u16 | mireds |
| color_xy | xy | CIE 1931, Zigbee ColorX/ColorY (0..65279) |
| temperature | i16 | 0.1 °C (Zigbee/Matter use 0.01 °C) |
| humidity | u16 | 0.1 % (Zigbee/Matter 0.01 %) |
| illuminance | u16 | lux, saturating (Zigbee/Matter use 10000·log10(lux)+1) |
| battery | u8 | percent 0..100 (Zigbee/Matter half-percent) |
| power_w / energy_wh | u16 / u32 | W / Wh |
| setpoint | i16 | 0.1 °C, occupied heating setpoint, writable |
| mode | enum8 | thermostat SystemMode: 0 off, 1 auto, 3 cool, 4 heat (same numbers in Zigbee and Matter) |
| occupied | bool | true = someone is there |
| contact | bool | true = **closed** (Matter BooleanState StateValue); an IAS "alarm" is "opened" |
| event | enum8 | button: 0 idle, 1 press, 2 double, 3 hold, 4 release, 5 on, 6 off, 7 up, 8 down |

A button press is a *pulse*: STATE(event=N), then STATE(event=0) 0.4 s later. The brain's state
machine is edge-triggered, so a rule `event == 1` fires on every press although two presses carry
the same code.

**One entity per Zigbee application endpoint** (identity: IEEE address + endpoint). The primary
cluster picks the type (thermostat > light > occupancy > IAS > sensor > button > plug); every other
cluster of the endpoint adds attributes, so a motion sensor that also reports temperature is one
`occupancy` entity with a `temperature` attribute. `battery` is only declared on devices that are
not plugs or lamps (those carry Power Configuration for the mains voltage). The mapping is
`core/zcl_profile.hpp`, host-tested. On the Matter side `core/matter_plan.hpp` turns an entity into one
endpoint: the device type from its primary attributes plus extra clusters (temperature, humidity,
illuminance, battery as Power Source, power and energy metering) on the same endpoint.

The `chip` device_type is the co-processor itself, declared as a self entity
whose `entity_id` equals the chip's own `chip_id` (0x01, 0x02, … — the registry
is global, so ids must be unique; header `entity_id 0x0000` stays "about the
chip" for OTA/meta messages); its health reaches the brain as ordinary STATE (no extra
message type). The Matter bridge skips it.

`thread_role` (0x76) and `thread_children` (0x77) describe the Thread network
of the co-processor that runs one: the role follows OpenThread's `otDeviceRole`
(0 disabled, 1 detached, 2 child, 3 router, 4 leader) and the children are the
devices attached to us in that network. A chip without a Thread stack reports
`0xff` ("no role"). They are what makes the Thread network visible from the
brain — the brain has no radio of its own.

> Keep this table in sync with Matter Device Types. When adding a device_type or
> attr_id, check the Matter cluster it maps to first — this is the most tedious
> but highest-payoff consistency work; getting it right means the bridge never
> needs reworking.

### 6.2 value_type enum

```
0x01 bool      (1 byte: 0/1)
0x02 uint8     (1 byte)
0x03 int16     (2 bytes, LE)      # e.g. temperature in 0.1°C
0x04 uint16    (2 bytes, LE)
0x05 float32   (4 bytes, IEEE754) # avoid on hot paths; prefer scaled ints
0x06 xy_color  (4 bytes: uint16 x, uint16 y)
0x07 enum8     (1 byte)
0x08 uint32    (4 bytes, LE)      # e.g. cumulative energy in Wh (up to ~4.29 GWh)
```

A Zigbee lamp and a DIY relay declared with the same descriptor are
indistinguishable to the brain — both are `device_type=light`. A rule like
"turn on the light" works with any source; both are exposed identically as Matter
lights.

---

## 7. Core (brain) components

Portable C++17, **no radio-stack / ESP-IDF-radio dependencies** — so it compiles
on host for tests and later on P4/Linux.

- **Bus** — the abstraction from Invariant 1. Transport implementations
  (`UartTransport`, later `SpiTransport`, `UsbTransport`) sit behind it.
- **EntityRegistry** — `entity_id → { chip_id, device_type, attributes[], state }`.
  Populated by DECLARE; cleared per-chip on `boot_id` change. Target scale: tens
  to low hundreds of entities (bounded by RAM on the brain).
- **StateMachine** — applies STATE updates; notifies subscribers (rules,
  recorder, Matter bridge) on change.
- **RuleEngine** — trigger → condition → action. Because all entities from all
  radio chips converge on one brain, cross-device rules ("motion in hall → light
  in kitchen") run locally in one place, no radio races. Start with a simple
  in-code rule table; a small embedded scripting language (Berry/Lua) is a later
  option, not part of MVP.
- **CommandDispatcher** — routes COMMAND to the owning chip by `chip_id`; tracks
  `seq` for CMD_ACK; timeouts + retry policy for critical commands.

Added since (all portable and host-tested; the status log in CLAUDE.md has the detail):

- **RuleEngine** is data, not code: comparison + typed operand, optional local-time
  window, one extra state condition and a cool-down, persisted in NVS; an injected
  action hook lets the brain run a *scene* as a rule's action.
- **TimerTable**, **ScheduleTable** (recurring local-time actions), **SceneTable**
  (named groups of settings, stable ids) — persisted; schedules, timers and rules may
  target a scene.
- **EntityNames**, **DesiredState** + **Reconciler** (what the hub last asked for is
  re-applied after a gap), **ChipMonitor** (liveness, reboot/flapping detection).
- **OtaSender / OtaReceiver / OtaVerifier** + portable SHA-256: firmware updates of
  the C6 co-processors over the UART link.
- **App protocol** (`json.hpp`, `app_protocol.hpp`, `app_requests.hpp`): the
  allocation-free JSON layer, snapshot/event encoding and request validation behind the
  WebSocket server that lives in the brain firmware.

---

## 8. Repository layout

```
/CLAUDE.md              # short: build/flash commands, conventions, pointer to this file
/SPEC.md                # this document
/libhubproto/           # shared: framing (COBS+CRC), header codec, message/payload structs
  /include/
  /src/
  /test/                # host unit tests (COBS round-trip, CRC, codec) — no hardware
/core/                  # brain: Bus, EntityRegistry, StateMachine, RuleEngine, Dispatcher
  /test/                # host unit tests — no radio deps
/firmware/
  /brain-s3/            # ESP-IDF app: core + UART transport, Wi-Fi/SNTP, app-protocol server (TLS), mDNS
  /zigbee-c6/           # ESP-IDF app: esp-zigbee-sdk coordinator + Zigbee→DECLARE translator
  /thread-c6/           # ESP-IDF app: esp-matter Bridge over on-chip Thread (chip 0x02)
  /recorder-cam/        # ESP-IDF app: passive bus listener → SD event log (stub)
  /components/          # shared ESP-IDF wrappers: hubproto, core, esp_uart_link, nvs_boot_id
/app/                   # Expo (React Native + web) client, mock hub, native pinning module
/scripts/               # matter-build.sh, gen-hub-cert.sh
/docs/                  # per-phase notes, backlogs, ota.md, app-protocol.md
```

`libhubproto` and `core` must build and unit-test on the host (no hardware) so
protocol and logic are testable before any board is involved.

---

## 9. Build phases (implement in order)

Each phase is independently testable. Do not start a phase before the previous
one passes its tests. Do not combine Zigbee bring-up with protocol bring-up —
debug one new thing at a time.

- **Phase 0 — Scaffold.** Repo layout, ESP-IDF v5.1+ toolchain, host build for
  `libhubproto`/`core`, CI running host unit tests. RGB-LED blink on C6 and S3 to
  confirm flashing works.
- **Phase 1 — Framing (host-only).** COBS encode/decode, CRC16-CCITT, header
  codec, message/payload (de)serialization. Full unit tests. No hardware.
- **Phase 2 — UART link-up.** `UartTransport` behind `Bus` on both S3 and C6
  (TX↔RX crossed + common GND). Exchange HELLO + HEARTBEAT; observe link on both
  sides.
- **Phase 3 — Registry + declaration.** C6 declares a couple of **fake** entities
  (`light`, `switch`); brain runs QUERY → DECLARE → DECLARE_END and populates
  `EntityRegistry`. C6 here is a stand-in for a real radio chip.
- **Phase 4 — State + commands + rules.** STATE upward; COMMAND/CMD_ACK downward;
  minimal RuleEngine driving a fake entity end-to-end.
- **Phase 5 — Resync.** `boot_id` in NVS; reboot C6, confirm the brain detects
  the change and re-queries.
- **Phase 6 — Zigbee.** `esp-zigbee-sdk` coordinator on C6; commission a real
  Zigbee device; write the Zigbee-attribute → DECLARE/STATE translator using the
  capability model. Fake entities from Phase 3 are now real.
- **Phase 7 — Recorder.** CAM passively logs STATE events to SD with timestamps;
  also logs `boot_id` changes and any detected gaps as history annotations. The brain now emits typed
  events through an `EventSink` (core/event_log.hpp) and keeps the last 256 in RAM for the app's History
  tab; the CAM is one possible further sink, not a prerequisite (docs/brain-backlog.md §7).
- **Phase 8 — Matter-over-WiFi bridge (outbound).** `esp-matter` on the brain:
  expose registry entities as Matter accessories, generated from the capability
  descriptors. Dev/test certificates only.

MVP = Phases 0–6. Phase 8 exists as a Matter *bridge* on its own chip (thread-c6, esp-matter
on the C6, not on the brain). Phase 7 (recorder): the event stream exists (RAM ring, app History tab); the SD
sink on the CAM is not started.

- **Phase 9 — App protocol and client (added later).** WebSocket+JSON over TLS with a
  token and a pinned certificate, mDNS discovery, snapshot + live events, commands and
  management requests (names, rules, schedules, timers, scenes, pair/unpair/identify);
  an Expo app (devices, automations, scenes, hub health) and a mock hub. Design and
  status: [docs/app-protocol.md](docs/app-protocol.md). Not verified on a phone yet.
- **Phase 10 — Remote access (relay).** The brain connects out to a relay server for
  access from outside the LAN — no extra hardware in the house. A cloud relay exposes
  HTTP+SSE for the app; a tunnel agent is the alternative for LAN-only deployments. The hub's
  token-based auth works end-to-end. An interactive console tool speaks the app protocol.
  Design: [docs/app-protocol.md §13](docs/app-protocol.md); deployment:
  [docs/relay-deploy.md](docs/relay-deploy.md). The ESP relay client is verified on hardware
  (against a LAN relay, `ws://`); a public relay over `wss://` is not deployed yet.

---

## 10. Technical constraints & gotchas

- **ESP-IDF v5.1+** required for ESP32-C6 (use a current 5.x).
- **Memory on the brain is the scaling limit**, not CPU. The entity registry and
  (later) Matter session state live in RAM. On S3, expect tens of entities /
  low tens of Matter sessions before heap/fragmentation bites. If this becomes
  the wall, the answer is a bigger-RAM brain (ESP32-P4 has 16/32 MB PSRAM), not
  a custom scheduler — FreeRTOS already provides preemptive multitasking, and a
  different scheduler does not create RAM.
- **PSRAM (when on P4/CAM) is slower than internal SRAM and DMA-limited.** Keep
  hot structures (queues, crypto handshakes) in internal SRAM; put cold objects
  (subscription state, cold entities) in PSRAM.
- **Do not connect two GPIO outputs directly.** Inter-board signals go through a
  protocol (UART/SPI), always with common GND. Avoid strapping pins (GPIO0 etc.)
  for inter-chip signals.
- **2.4 GHz coexistence.** WiFi, Zigbee, Thread, BLE, nRF24 all share 2.4 GHz.
  Keep Zigbee and (future) Thread on different channels; separate antennas where
  possible.
- **One 802.15.4 radio = one stack at a time.** A C6 is *either* a Zigbee
  coordinator *or* a Thread border router per flash, not both. Simultaneous
  Zigbee + Thread needs two separate 802.15.4 radios (two C6, or C6 + H2).
- **CAM flashes via external FTDI** and is GPIO-poor; fine as a wired recorder.
  C6 and S3-Zero flash over their own USB-C (no external adapter).

---

## 11. Future / migration notes (context, not tasks)

- **Brain migration.** S3 → P4 (more RAM, no radio → needs a WiFi companion like
  C6) → Linux SBC (Radxa Rock 5B+, Orange Pi 5, or N100 mini-PC). Because the
  protocol is transport-independent and the core has no radio deps, radio chips
  become USB/UART radio co-processors of the new brain; core and protocol move
  unchanged.
- **Matter controller (inbound) at scale** — receiving/managing many purchased
  Matter devices — is RAM-bound and not a well-trodden path on ESP; it is the
  first thing to move to a Linux brain when scaling beyond a prototype.
- **Thread / Matter-over-Thread** — add a second 802.15.4 radio (C6 self-contained
  BR, or S3-host + H2-RCP per Espressif's reference). Note a C6 border router
  hard-depends on WiFi (loses WiFi → Thread devices drop).
- **Certification** — CSA membership + per-product DAC (and secure-element
  storage for keys) is required to ship as a real product. Not needed for
  personal/dev use, but choose hardware that keeps the door open (secure boot /
  key storage) rather than foreclosing it.
