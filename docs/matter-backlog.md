# Matter backlog — what a full Matter bridge needs

State today (`main`, commit `6ad8bd2`): `thread-c6` is a full Matter Bridge
(aggregator endpoint 0x000E + dynamic bridged endpoints via `app_bridge_*`,
persisted NVS). Apple Home commissioned it over BLE + Thread; every toggle in
Home flips the real IKEA plug end-to-end. Bridged endpoints carry On/Off
Light + EPM (ActivePower W) + EEM (CumulativeEnergyImported Wh) clusters when
declared. Build path: Docker image `espressif/esp-matter:release-v1.6_idf_v5.5.5`
(Route 3 below); flash from host esptool.

Removing a Zigbee device (`unpair`, console or app) removes its Matter mirror too: the brain remembers the
unpair request, and once the Zigbee chip has declared again without that entity it sends the Matter chip
`Undeclare` (hubproto 0x04, entity in the header); thread-c6 removes the bridged endpoint (and its NVS record)
under the Matter stack lock. Deliberately tied to the explicit request, not to "an entity is missing" — one lost
DECLARE frame must never delete an endpoint people have put in rooms and automations. Not covered: a device
that leaves by itself (factory reset) and an unpair pending across a brain reboot.

## Device types on the bridge

Every hub entity becomes one bridged endpoint; `core/matter_plan.hpp` (host-tested) picks the Matter
device type from its attributes and the extra clusters to add, and the choice rides in the endpoint's
persisted "device type id" (low 16 bits the Matter device type, above them the extras), so a restored
endpoint has exactly the clusters it was created with:

| hub entity | Matter device type | extra clusters |
|------------|--------------------|----------------|
| plug / on-off only | On/Off Light | Electrical Power / Energy Measurement |
| brightness | Dimmable Light | power, energy |
| + colour temperature / + xy | Color-Temperature / Extended Color Light | power, energy |
| temperature, humidity or illuminance | Temperature / Humidity / Light Sensor (first one present) | the other readings, Power Source |
| occupancy | Occupancy Sensor (PIR) | temperature, humidity, illuminance, Power Source |
| contact | Contact Sensor | temperature, Power Source |
| thermostat (setpoint, mode) | Thermostat (heating) | Power Source |
| button | Generic Switch (momentary, release, long press) | Power Source |

Things learnt the hard way (all found by `inject`, the brain console tool that declares a made-up device to
the Matter chip only: `inject dimmer|ctlight|colorlight|sensor|motion|contact|thermostat|button`, `inject set
<entity> <attr> <value>`, `inject clear`):

- `app_bridge_create_new_device`'s last argument is the device's *private data*, not an out-parameter.
- The occupancy cluster aborts the chip unless a sensing feature (PIR) is set.
- BooleanState and OccupancySensing are code-driven clusters: `attribute::update` is refused, the cluster
  object (`registry().Get(path)`, then `SetStateValue` / `SetOccupancy`) has to be told.
- Switching a lamp on makes the Matter stack move CurrentLevel to 1 by itself, and it initialises attributes
  while an endpoint is created or restored. The bridge ignores level/colour/thermostat writes during endpoint
  construction, for 8 s after boot and for 0.6 s after mirroring an "on", otherwise the real lamp would be
  dimmed to 1 by its own mirror.
- The cluster set of an endpoint cannot change after creation, so an entity whose attributes changed (a lamp
  whose colour modes were read) gets a new endpoint; a bare legacy on/off-light id counts as matching so the
  plug keeps its accessory (and, as before, its power/energy clusters are absent after a restore).
- Clusters must be enabled in `sdkconfig.defaults` (`CONFIG_SUPPORT_THERMOSTAT_CLUSTER`, `..._SWITCH_...`,
  `..._OCCUPANCY_SENSING_...`, `..._POWER_SOURCE_...`) or the link fails. 20 bridged devices fit
  (`CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=22`).
- A controller dragging a slider produces many writes: level, colour and thermostat commands are debounced
  (150 ms, latest wins) before they go to the brain; on/off is immediate.

Not verified: any of this against a real Zigbee device of that type, and with a controller (Apple Home) —
only with the injected devices and the plug.

Matter is a stack of independent pieces. Each has its own effort and its own
failure mode; put together they let Apple Home / Google Home / Alexa see the
home's devices as first-class Matter accessories.

## The big pieces, bottom-up

### 1. OpenThread on chip 0x02 (Thread border router)

Bring up the Thread stack on the second C6:

- Managed component `espressif/esp-openthread` + `esp-thread-br` (BR
  = border router; the flavor that also does IPv6 routing to the LAN).
- sdkconfig: enable `CONFIG_OPENTHREAD_ENABLED`, pick a role
  (`CONFIG_OPENTHREAD_BORDER_ROUTER`), enable radio native (`OPENTHREAD_RADIO_NATIVE`).
- Dataset (network key, PAN ID, channel, xPAN). Persisted in `ot_storage`
  (already reserved in `partitions.csv`).
- Backbone network: how does Thread reach the LAN? Options:
  - Bring WiFi up on **the brain** (S3) and let the C6 border-route through
    it — needs a small routing bridge on the S3 side too (BR over UART).
    Extra work; keeps the C6 backbone-agnostic.
  - Or plug the C6 into WiFi itself — simpler code, but now the C6 juggles
    Thread + WiFi + Matter, tight on RAM.
- Verification: `ot cli` over the UART, form a network, see the border
  agent announced on the LAN via mDNS (`_meshcop._udp`).

Estimate: **1 focused session** just for this, longer if the backbone routing
choice above turns out fiddly.

### 2. esp-matter SDK integration

Matter itself. Add `espressif/esp-matter` as a component (large — many MB).
Depends on: OpenThread (above), the CHIP subtree, mbedTLS, and its own
partition layout (factory + KVS namespaces for fabrics, ACLs, groups,
attribute persistence).

- Partition rework: add `matter_kv` + `factory` (DAC/PAI live here) to the
  C6#2 table.
- App main gains `esp_matter::start()` and a data-model registration step.

Estimate: **1 session** for the plumbing, before any endpoint exists.

### 3. BLE for commissioning

Matter's first-time pairing runs over BLE (a Matter Commissioner talks to
the accessory over BLE, sets the fabric certificates, then Thread takes
over). The C6 has BLE, but running BLE + 802.15.4 + Matter on one chip
tightens memory. Coexistence config in sdkconfig.

If BLE gets crowded, alternate: **the brain S3 hosts SoftAP-based
commissioning** and hands the fabric off to the C6. More code, but leaves
the C6 free.

**What esp-matter can actually do on the C6** (read in the sources of
`release/v1.6` on 01.11.2026 — no hardware needed). The controller component
builds for the C6 (no SoC restriction in its `CMakeLists.txt`), and the C6
does have a Bluetooth LE radio (`SOC_BLE_SUPPORTED` in
`components/soc/esp32c6/include/soc/soc_caps.h`). But the *commissioning
transports* are not the same on every chip:

| command | what it needs | where esp-matter offers it |
|---|---|---|
| `pairing_ble_wifi`, `pairing_ble_thread` | the BLE transport, behind `CONFIG_ENABLE_ESP32_BLE_CONTROLLER` | **ESP32-S3 only** — the vendor docs say exactly that for both BLE pairing methods |
| `pairing_on_network` | the device is already on the same IP network, plus the setup PIN | any chip |
| `pairing_code` | a Matter pairing payload (the onboarding payload, or the manual code) | any chip |

So the Matter chip cannot run the BLE handshake the phone imagines. Two ways
out, and the app protocol already carries both:

- **payload pairing** (`pairing_code`): the phone hands over the payload it
  read from the air (our `kind 1`) or from the QR (`kind 2`), and the chip
  pairs with it. The device has to be reachable — on the home Wi-Fi, or over
  SoftAP.
- **on-network pairing**: the device is already on the home Wi-Fi, the phone
  sends the PIN and the discriminator (both of which it can read: the
  discriminator from the BLE advertisement, the PIN from the label or the QR),
  and the chip finds the device by mDNS.

The phone's Bluetooth read therefore stays useful — it *identifies* the
device and carries the payload — but it is not the transport of the
commissioning itself. The app must not tell the person that the phone does
the pairing.

**What the chip side will have to do with each kind** (the wire form is
already in place — `hubproto::MsgType::MatterPair`, `kind(u8) len(u8) bytes`):

| kind | what the app sends | what the chip has to make of it |
|---|---|---|
| 0 | the commissioning service data, 7 bytes | only a discriminator and a setup hash — enough to recognise the device, not to commission it |
| 1 | the bytes of the enhanced commissioning advertisement | the advertising form of the onboarding payload; the CHIP ESP32 code that parses it lives in the **S3-only** BLE commissioning path, so on the C6 it has to be written by hand |
| 2 | the payload of a QR code (the 11 packed bytes) | the CHIP setup payload is parsed from the **`MT:…` text** (`SetupPayload::FromStringRepresentation`, the QR parser), so the chip either re-encodes these bytes to base38 or the wire form carries the text itself — a decision for the implementation |

For the on-network path the chip needs the **setup PIN** and the **discriminator**
instead of a payload. The app has both already (`decodeMatterQr` reads the PIN
and the discriminator out of the QR payload; the BLE service data carries the
discriminator), so if that path is the one that survives the RAM gate, the
protocol grows one more kind — a small change, better made when the chip side
exists.

**The RAM gate — the budget is measured, the controller's own footprint is
not.** What can be measured without hardware was measured on this machine, from
the linker map of the thread-c6 build (`firmware/thread-c6/build/keystone_thread_c6.map`,
build of 03.10.2026):

| what | bytes | note |
|---|---|---|
| SRAM the app may use (`sram_seg`) | 452 112 (441.5 KB) | the C6's internal SRAM window, minus what the ROM and the bootloader keep |
| static D/IRAM of the bridge as it is | 194 184 (189.6 KB) | `.dram0.data` 12 552 + `.dram0.bss` 87 824 + `.iram0.text` 93 804 + `.iram0.data` 4 |
| what is left for the runtime heap | 257 928 (251.9 KB) | before the FreeRTOS stacks, the heap bookkeeping and the driver's buffers |
| flash (text + rodata) | 1 564 576 (1 528 KB) | of the two 1.92 MB slots |

So a CHIP controller on this chip inherits a budget of **~250 KB of heap** — and
that budget has to hold the Matter stack, a live CASE session, and the ≥32 KB
margin of the stop criterion at the same time.

The vendor's own configuration points the same way. In esp-matter `release/v1.6`,
`examples/controller` ships `sdkconfig.defaults.esp32s3` and
`sdkconfig.defaults.esp32s31` — **there is no C6 configuration** — and its
RAM-optimised build turns on `CONFIG_SPIRAM` together with
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, i.e. it moves the Matter
library's BSS into external RAM, which the C6 cannot do (no PSRAM). The docs'
"RAM and Flash Optimizations" page says the same for controller and
Thread-Border-Router applications with PSRAM.

**Not measured, and not measurable on this machine:** the static DRAM of a
controller build, and the free heap during a live CASE session. The first needs
the esp-matter image — 5.76 GB compressed, ~13 GB unpacked — while this Mac has
3.1 GB free on a full disk; the second needs a board and a real Matter device
(nothing is connected here). Until both are measured the verdict is **no-go for
the commissioner on the C6 as it is**: the vendor ships the controller for
S3 + PSRAM, and ~250 KB would have to hold the whole CHIP controller stack next
to a bridge that already takes 190 KB. Two ways to change that verdict:

- **F1 — slim the bridge**: every bridged endpoint costs RAM and flash;
  `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` is 22 today.
- **F2 — a second C6 as a controller-only node**: the ~250 KB is then not
  shared with the bridge, but that node needs its own Thread path.

**Measured on the Linux dev box 04.11.2026 (branch `experiment/commissioner-on-s3`
commit `a11c223`).** The static DRAM was measurable on this machine because
Docker + the esp-matter image are here, so the same gate that could not be run
on the Mac was run and gives real numbers for the **S3** path (F5 — commissioner
on brain-s3, not on the C6):

| what | bytes | note |
|---|---|---|
| S3 internal DRAM (`dram0_0_seg`) | 341 760 (334 KB) | after the boot ROM reservations |
| brain (pre-Matter) static DRAM | 165 723 | `.bss` 57 312 + `.text` 78 519 + `.data` 29 892 (commit `63bf9c1`) |
| with the controller called | 336 451 | `.bss` 209 584 + `.text` 101 991 + `.data` 24 876 |
| controller's own static cost | **+170 728** | almost all of it `.bss`: fabric table, session manager, DNS-SD caches, ACL |
| DRAM left after the call | 5 309 B | the slack for DMA / ISR / things that must be internal |
| flash image | 1 945 880 (1.86 MB) | 20 KB free in the 1.92 MB OTA slot |
| flash cost of the controller | +685 KB | `.text` 856 → 1407 KB (+551), `.rodata` 279 → 395 KB (+115) |

What this means for F5 (commissioner on brain-s3 — the one the F-list above
does not have a name for):

- **RAM gate: cleared, barely.** The controller's ~170 KB static BSS does
  fit, but only after `EXT_RAM_BSS_ATTR` on `g_ev_ring` moved 6 KB of our own
  BSS to PSRAM — that is what turned a 992-byte overflow into 5 309 B of
  slack. Heap during a live CASE session is still not measured (no device
  connected). The ~250 KB "controller heap budget" the F-list mentioned now
  lives in PSRAM via `CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL` +
  `MBEDTLS_EXTERNAL_MEM_ALLOC` + `BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`; the 5 KB
  internal slack is only for allocations that refuse PSRAM.
- **Flash gate: tight.** 20 KB free in a 1.92 MB OTA slot. Adding the real
  pairing path (QR decode, the Rendezvous state machine, UDC, Zigbee→Matter
  mirroring on this chip) will not fit the current slot. Choices then are
  **F1'** — shrink the brain image (some application state can move to PSRAM
  the same way `g_ev_ring` did, and the status LED / relay client / websocket
  console paths each carry their own tables) — or **F5' — go to an 8 MB flash
  variant of the S3-Zero** (the Waveshare S3FH4R2 is 4 MB; a plain S3 module
  with 8 MB keeps the same pinout and PSRAM).

**Flashed and booted on the real brain-s3 04.11.2026 (commit `2ca3ab7`, with
the console command `matter_pair`).** The *runtime* RAM picture turned out to
be worse than the linker map said — the static gate had slack, but the live
heap did not:

```
I (660) heap_init: At 3FCDA330 len 0000F3E0 (60 KiB): RAM
I (661) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM
I (661) heap_init: At 3FCF0000 len 00008000 (32 KiB): DRAM
I (799) matter: heap before bringup: 39135 B internal
I (829) BLE_INIT: Malloc failed
E (829) BLE_INIT: esp_bt_controller_init -4
E (829) chip[DL]: Disabling CHIPoBLE service due to error: b
I (1949) matter: controller up: heap 1939 B (-37196 B), psram free 2045172 B
Guru Meditation Error: Core 0 panic'ed (LoadProhibited).  → crash-loop
```

What that says:

- internal RAM is handed out as **three non-contiguous pools** (60 + 21 + 32 KB),
  not one. The BLE controller (`libbtdm_app.a`) asks for its ~30 KB of HCI/LLCP
  buffers as a **single contiguous block from internal RAM**, and no pool is
  big enough once the brain's own startup has run. `BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`
  only moves the NimBLE **host**; there is a separate knob for the controller
  (`CONFIG_BT_CTRL_HCI_...`, undocumented in esp-matter) that we did not set.
- `esp_matter::start()` itself consumes 37 KB of what little internal heap
  remained (nvs opens, mDNS tables, Operational-Credentials storage, test DAC,
  CASE server setup) and leaves **1939 B of internal RAM live** before
  anything else wakes up. Matter does bring up (fabric 0x2 is retrieved, CASE
  server enables, operational node is advertised on mDNS), but there is
  nothing left for the first scheduled task that wants internal memory, so
  the brain panics in under a second.

**Verdict: option A does not cross even the PASE step on this hardware** —
the BLE radio never comes up. The brain reverted cleanly to its PlatformIO
image (1.37 MB, no Matter) and runs normally; nothing in NVS or Zigbee state
was affected. Options to try before giving up on F5:

- **Move the BLE controller allocations to PSRAM** (`CONFIG_BT_CTRL_HCI_TL_MODE`,
  `CONFIG_BT_CTRL_HCI_SERIAL_MODE`, or the newer `BT_CONTROLLER_HOST_HEAP`
  knobs; esp-matter examples/controller does not use them, so some digging in
  IDF Bluetooth config is required). If any of those takes the 30 KB demand
  out of internal RAM, the gate reopens.
- **Shrink the brain's own startup footprint.** The relay client's 1-KB
  outbound queue, the websocket server's per-client buffers (3 clients × a few
  KB each), the status-LED LED-strip RMT buffers all sit in internal RAM.
  Tagging them with `EXT_RAM_BSS_ATTR` or allocating them via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
  should give back more than 30 KB.
- **F5' (8 MB S3)** does not help this one — internal RAM is the chip's, not
  the module's. PSRAM capacity would need to grow (it is already enough).

The CASE-session heap number is still unmeasured, but that is now behind the
BLE and panic gates and depends on their fix first.

**F3 (the Linux brain, SPEC §11) stays the fallback that needs no new
hardware**, and F4 (the app shows the device and its QR payload; the person
adds it to Apple Home with the Home app) needs no chip work at all.

**Where a Matter device ends up: a fabric, not a bridged endpoint.** A Matter
bridge aggregates *non-Matter* devices as its own endpoints — that is exactly
what our C6 does today with the Zigbee devices. A native Matter device cannot
be aggregated that way: it joins a fabric as a node in its own right, and a
device belongs to a fabric through its administrator. So the plan's step "make
a bridged endpoint for a Matter device in the Apple Home fabric" is not
something a bridge can do. What is possible:

- **Apple Home's fabric** is administered by the phone — the Home app reads the
  QR, runs the BLE handshake and issues the credentials. Our hub's operator
  credentials cannot add a node to it; commissioning into a fabric is the
  administrator's act, and the administrator here is the phone. (An operator may
  control the nodes its ACL allows, but that is not a right to add new ones,
  and relying on Apple granting one is not a plan.)
- **A fabric of our own** — if the hub runs a Matter commissioner with its own
  fabric admin, it can commission the device into *that* fabric. Then the
  device is controllable from the Keystone app (it becomes an entity in the
  registry, like a Zigbee device), but it is a different fabric from Apple's,
  so Home does not see it.

A device can sit in both fabrics (Matter allows multi-admin), but each fabric
needs its own commissioning pass: the Home app for Apple's, the hub for ours.
That is why the app's "Add to the hub" must not promise that the device will
appear in Apple Home — and why the phone's Bluetooth read stays useful even in
the F4 variant: it identifies the device and carries the payload, and the
person adds it to Home with the Home app exactly as they would without us.

### 4. Data model — Matter endpoints on the C6

Matter controllers talk to endpoints, not to us. Two shapes:

- **Simple mirror**: for every registry entity on the brain, create a
  matching Matter endpoint on the C6 (light, on/off plug, occupancy sensor,
  ...). Attribute values live in Matter's local storage; the C6 pushes
  updates through cluster attribute writes when the brain says STATE
  changed. Downside: every attribute stored twice (brain + C6).
- **Pull bridge**: the C6 declares Matter endpoints but resolves reads by
  asking the brain over UART on demand. Slower per read, half the memory.
  Matter tolerates ~seconds of latency on attribute reads.

Realistic choice: **simple mirror**, cache-only, C6 is authoritative for
Matter, brain remains authoritative for the home's own state; a small sync
message (STATE_MIRROR or reuse STATE with `chip_id=0x02`) keeps them
consistent.

Every device_type in [zigbee-backlog](zigbee-backlog.md) needs a matching
Matter Device Type; since SPEC §6.1 was written Matter-cluster-shaped, this
is mostly a lookup table.

### 5. Bridge: brain registry ↔ Matter data model

Two directions:

- **Up**: STATE change in registry → C6#2 updates the corresponding Matter
  attribute → active Matter subscriptions push the change to controllers.
- **Down**: Matter command from a controller (e.g. "turn on light X") → C6#2
  receives it → sends `COMMAND(entity_id, on)` over UART2 to the brain → brain
  routes to C6#1 (Zigbee) → device → CmdAck path back → STATE mirror updates
  the Matter attribute → controller sees Success.

Failure surface here is huge (timeouts, retries, race conditions between the
Matter command handler and the brain's own CommandDispatcher). This is where
the [recovery](backlog.md) invariants matter: desired-state on the brain,
reconciler that closes the loop even if a mirror update is lost.

### 6. Commissioning UX

- **DAC / PAI / Certification Declaration** — Matter security roots.
  - Dev / test: esp-matter ships test roots that work out of the box; Apple
    Home will onboard the device with a "Uncertified Accessory" warning.
    Good enough for personal use and demos.
  - Production: CSA membership ($7000/year) + Certified Provisioning
    (signed CD, per-device DAC, factory provisioning line). Out of scope
    until, if ever, this becomes a product.
- **Setup code + QR** — hardcode a setup passcode + discriminator + generate
  the QR at build time. Printed on paper / shown on the brain console.
- **First-fabric flow** — user opens Apple Home → "Add Accessory" → scans QR
  → BLE pairing → Matter over Thread takes over.

### 7. Multi-fabric support

Matter's headline feature: one accessory in Apple Home + Google Home + Alexa
at once. Costs ~30 KB RAM per fabric on the C6. esp-matter supports up to
5 fabrics by default; may need to raise / trim in sdkconfig.

### 8. Matter OTA (updating the C6#2 itself)

Separate from Zigbee OTA. Matter has its own OTA Provider / Requestor. Not
critical for MVP, worth planning before shipping.

### 9. Firmware updates of *bridged* devices

Matter has no standard way to update the firmware of a **bridged** device (a
Zigbee bulb behind the bridge): the Software Update clusters belong to the
node itself, not to the endpoints it bridges. So the Zigbee OTA the hub now
does (docs/ota.md) has no Matter-side representation — the progress is visible
in the app and the console only. Nothing to implement, but worth remembering
before someone looks for the missing Matter cluster.

## Grand order-of-work (rough)

1. ✅ OpenThread up (Thread network formed on C6#2, FTD).
2. ✅ esp-matter integrated, one Matter On/Off endpoint on C6#2, Apple Home
   commissions it over BLE + Thread.
3. ✅ Bridge down-path: Matter On/Off write → COMMAND over UART → real IKEA
   plug switches → CmdAck. (`brain-s3::on_bus_forward` at the ChipRouter.)
4. ✅ Bridge up-path: STATE from Zigbee → mirrored on the Matter attribute →
   Apple Home reflects the new state (same forward).
5. ✅ **Matter Bridge** (aggregator endpoint 0x000E + dynamic bridged
   endpoints via `app_bridge_*`) — one endpoint per registry entity,
   persisted in NVS so Apple Home never loses devices across reboots.
   Trigger: brain re-plays every known DECLARE to chip 0x02 the moment
   `RadioCoordinator` marks Matter ready, so Bridge can build endpoints
   for entities it wasn't around for the first time.
5a. ✅ **EPM cluster** (0x0090, ActivePower) on bridged endpoints that declare
    `power_w(0x13)` — `attribute::update` with nullable int64 milliwatts
    (Stage 2c). Hardware-verified: INSPELNING → STATE(power_w) → Matter
    ActivePower attribute.
5b. ✅ **EEM cluster** (0x0091, CumulativeEnergyImported) on bridged endpoints
    that declare `energy_wh(0x14)` — `NotifyCumulativeEnergyMeasured()` with
    int64 milliwatt-hours (Stage 2e). Cluster confirmed on hardware; full
    STATE path triggers after ≥1 Wh accumulation.
6. Additional Matter device types matching the Zigbee ones as they come
   online. Today only Switch → On/Off Light is wired
   (`map_device_type()` in `thread-c6/main/main.cpp`); adding
   dimmable_light, color_control, occupancy_sensor, etc. is a lookup
   table + a few extra clusters in `create_bridged_clusters()`.
7. NVS-restore limitation: EEM/EPM clusters are only added on fresh endpoint
   creation (`g_pending_has_metering/energy` flags set before
   `app_bridge_create_new_device()`). NVS-restored endpoints get only
   `on_off_light` clusters — workaround: erase NVS to force fresh creation.
   Fix: pass cluster flags through NVS or reconstruct from DECLARE at
   bridge init.
8. Multi-fabric + polish (removing test-DAC warning, DAC certification,
   Matter OTA).

Steps 1–5b done on `main`, hardware-verified end-to-end with an IKEA
INSPELNING plug: Apple Home talks to a bridged endpoint via Thread → C6#2 →
UART → brain → Zigbee → plug flips; EPM ActivePower confirmed; EEM cluster
created and confirmed (energy_wh flow active after ≥1 Wh).

## Known blockers

Both routes we tried on 2026-09-28 dead-ended in different upstream places,
not in our code. Recording the details so the next attempt starts from what
we've already learned instead of the same rakes.

### Route 1: keep IDF 5.5.5 + esp-matter release/v1.5–v1.6

Fails at **the CHIP subtree vs GCC 14**. IDF 5.5.5 ships the riscv toolchain
`esp-14.2.0_20260121` (GCC 14). Compiling CHIP's `src/app/data-model/Nullable.h`
line 116 fails with `no match for 'operator=='` on
`std::optional<chip::app::Clusters::ClosureControl::GenericOverallCurrentState>`
(and sibling `GenericOverallTargetState`). GCC 14 tightened `operator==` name
lookup and requires `T` to be EqualityComparable at template instantiation,
which the ClusterAdvertisement-generated `GenericOverall*State` structs are
not. `esp-matter/main` HEAD has the same code; the upstream connectedhomeip
`master` also has it — so this needs a fix at the CHIP level. Options:
- Wait for CHIP to add SFINAE / concept guards on `Nullable::operator==`, or
  for the generated `GenericOverallState` structs to declare their own
  `operator==`.
- Patch the local managed component after every install — brittle.
- Downgrade the toolchain (see route 2).

### Route 2: downgrade to IDF 5.2.3 + esp-matter release/v1.3

Fails elsewhere, worse. The 5.2.3 install itself works (esp-13.2.0 riscv,
GCC 13.2 — no CHIP `operator==` issue), but three layered problems show up
in sequence:
1. **`check_python_dependencies.py` in IDF 5.2 mis-handles modern PyPI
   metadata (Metadata-Version 2.4).** Fresh `install.sh` populates a Python
   venv where the packages themselves are correct (they satisfy their own
   `Requires-Dist`), but the checker walks all installed packages, mis-reads
   `ruamel.yaml`'s 2.4 metadata and repeatedly reports a different unmet
   requirement each `export.sh` run (`cryptography`, `pyparsing`,
   `setuptools>=64`, `esp-idf-kconfig`, `construct`, …). Workaround
   discovered: `sys.exit(0)` at the top of `tools/check_python_dependencies.py`
   — makes `export.sh` succeed. Still needed on every fresh install.
2. **The managed component `espressif/esp_matter ~1.3.1` doesn't compile**
   against IDF 5.2.3's headers: `src/app/ClusterStateCache.cpp` has
   forward-declaration mismatches and undeclared identifiers
   (`ClusterStateCache`, `mRequestPathSet`, `mLastReportDataPath`, …). Pin
   to `==1.3.0` gets past this.
3. **`espressif/esp_insights` (dep of esp_matter 1.3.0)** then fails to
   build against IDF 5.2.3 — `esp_insights_cbor_encoder.c` refers to
   `SHA_SIZE` (undeclared) and `rtc_store_non_critical_data_hdr_t::dg` (no
   such member). Fixing this would need pinning esp_insights, patching it,
   or disabling insights via sdkconfig — worth trying next time.

### Route 3: Docker build image (chosen)

Verified working on 2026-09-28. Image
`espressif/esp-matter:release-v1.6_idf_v5.5.5` (5.29 GB compressed) has the
whole toolchain preassembled: IDF at `/opt/espressif/esp-idf`, esp-matter at
`/opt/espressif/esp-matter`, the tested Python venv, and — crucially — the
riscv toolchain version esp-matter was validated against, so the CHIP
`Nullable.h`/GCC-14 breakage from route 1 does not reproduce. Reference build
inside the container:

```
docker run --rm espressif/esp-matter:release-v1.6_idf_v5.5.5 bash -lc \
  'cd $ESP_MATTER_PATH/examples/light && idf.py set-target esp32c6 && idf.py build'
```

produces `light.elf` + `light.bin` (1.85 MB, 6% headroom in the 1.92 MB OTA
slot). Working recipe going forward: keep source in this repo, invoke the
container with the project bind-mounted (`-v $PWD:/project`) — code stays under
version control, only the toolchain is containerized. Local flashing still uses
host esptool over USB.

Cost: ~5.3 GB image + ~2-3 GB per build workspace. Requires ~15 GB free disk
before pulling.

### Practical read

Route 3 (Docker) is now the primary Matter build path for this repo. Route 1
(local IDF 5.5.5) remains a "watch this space" — once esp-matter publishes a
release that fixes the CHIP `Nullable.h` operator== issue against GCC 14, we
can drop the container. Route 2 (IDF 5.2.3 downgrade) is a dead end and worth
keeping only as a warning.

### Flash size

Independent of the toolchain, **4 MB flash is tight for Matter + OpenThread +
BLE + our UART bridge**. `managed_component_light`'s partition table is two
1.92 MB OTA slots + `esp_secure_cert` + `fctry` — right at 4 MB. To fit we'd
have to drop `ota_1` (lose Matter OTA rollback) and possibly ratchet the
factory partition. Something to size once one of the above routes is
unblocked.

## Thread RCP mode (idea)

Running the full Matter + Thread + BLE stack on the C6 leaves ~181 KB heap with
9 bridged endpoints and a fabric — tight enough to cause watchdog reboots during
init. The alternative: **switch the C6 to Radio Co-Processor (RCP) mode** and
run the Matter/Thread host stack on the brain S3, which has 320 KB SRAM + 2 MB
PSRAM (present, not yet enabled). The C6 becomes a thin 802.15.4 radio driven
over UART/SPI by the host; Espressif's `esp-thread-br` supports this pattern.

Pros: memory is no longer the bottleneck, the number of bridged endpoints is
limited only by the S3's resources, and the C6 firmware becomes trivial (just
the RCP image). Cons: major architectural change — the UART link between brain
and C6 would carry the Thread/spinel protocol instead of hubproto, and the
Matter data model would live on the brain rather than on a separate chip. The
brain's Wi-Fi could serve as the Thread backbone directly (Border Router on the
S3). Evaluate if the current approach hits a wall beyond ~12 endpoints.

## Traps to watch

- Matter certs + BLE + Thread + WiFi + our UART link — the runtime is busy.
  Watchdog-friendly, but not much headroom on one C6.
- Test roots are OK for personal use but Apple Home will occasionally flag
  the accessory as "uncertified" — not a bug, just the UX.
- Thread's own commissioning (`ot ba start` etc.) is separate from Matter's —
  they cooperate but the invariants are subtle.
- Matter's spec is _large_; we implement only the clusters we bridge, not
  everything. Keep the SPEC §6.1 mapping table as the single source of truth
  for what maps to what.
