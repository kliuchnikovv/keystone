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

**Verdict of that run: option A did not cross even the PASE step on this
hardware** — the BLE radio never came up. The brain reverted cleanly to its
PlatformIO image (1.37 MB, no Matter) and runs normally; nothing in NVS or
Zigbee state was affected.

**But the radio does come up — three fixes (05.11.2026, working tree of
`experiment/commissioner-on-s3`).** All three causes above are gone, and the
BLE stack is provably alive on the real board:

| fix | what | why |
|---|---|---|
| the controller singleton → PSRAM | `matter_controller_client::s_instance` tagged `EXT_RAM_BSS_ATTR` (patched in the container, `scripts/matter-build.sh`) | the singleton embeds a whole `CHIPDeviceController` — ~130 KB of internal `.bss`, more than half of what the radio needs in one block |
| the brain's own tables → PSRAM | `bus`, `reg`, `coord`, `sm`, `disp`, `monitor`, `rules`, the NVS/scene/timer buffers, the relay RX buffer … all `EXT_RAM_BSS_ATTR` | the brain's own `.bss` sat on top of the three small internal pools |
| BLE slimming + a different `app_main` order | one connection, no ISO / privacy / extended / periodic advertising, minimum buffer counts, and the Matter bring-up **before** the Wi-Fi driver starts | the BLE controller wants one contiguous ~30 KB block of internal RAM, and the Wi-Fi driver, started first, was fragmenting the big pool |

The last one is the subtle one: the S3-Zero hands out its internal RAM as
**three non-contiguous pools** (60 + 21 + 32 KB), the radio asks for its
HCI/LLCP buffers as *one* contiguous block, and the Wi-Fi driver split what
was left. Bringing Matter up first — and only then starting the Wi-Fi driver
— gives the radio its block. The boot now reaches:

```
matter: app_main entry: heap 269391 B internal, largest block 204800 B
matter: heap before bringup: heap 253903 B internal, largest block 196608 B
matter: after esp_matter::start: heap 141583 B internal, largest block 81920 B, psram free 1838396 B
matter: controller up: heap 115847 B (-138056 B), largest block 55296 B, psram free 1773896 B
chip[FP]: Fabric index 0x2 was retrieved from storage … NodeId 0x000000000001B669
chip[IN]: CASE Server enabling CASE session setups
NimBLE: GAP procedure initiated: discovery
NimBLE: duration=60000ms
BLE Scanner: 02 01 1a 02 0a 0c 0b ff 4c 00 10 06 …
```

Compare that with the first attempt, where the controller came up with **1939 B**
of internal heap and the radio had **no** block at all: the radio now gets a
55 KB contiguous block and **115 KB** of internal heap is still live afterwards
(the case session's own peak is still unmeasured — that needs a device).

**The scan ran — and the device was in range the whole time.** The 60 s
window catches live advertisements from the neighbourhood, and among them the
**TIMMERFLOTTE itself**: Matter commissioning service data (0xFFF6) with
vendor 0x117C (IKEA) and product 0x8005 — which *is* the TIMMERFLOTTE
(matter-survey: "IKEA of Sweden TIMMERFLOTTE temp/hmd sensor", vendor
0x117C, product 0x8005). Its advertising discriminator is **0x0239**.

Why CHIP never connected to it: the discriminator and the PIN were read off
the manual pairing code **as if it were "discriminator then PIN"** — it is
not. The Matter manual code is bit-packed (CHIP `ManualSetupPayloadParser`:
chunk1 = 1 digit, chunk2 = 5, chunk3 = 4, the last character is a Verhoeff
check digit), and it carries only a **4-bit short discriminator** (the top
nibble of the 12-bit one) plus a 27-bit PIN. Decoding the device's code the
way CHIP does gives short discriminator 2 — and the sensor advertises
0x0239, whose top nibble is 2. The device matched all along; our parameters
were wrong, and CHIP only ever logged `chip[BLE]: Discriminator did not
match`.

**The earlier "not a single Matter service-data AD" was our own parsing bug.**
`ESP_LOG_BUFFER_HEX` prints a multi-line hex dump, so concatenating the dump
line by line and re-syncing the AD structures desynchronised them. A
self-synchronising AD walker over the same log finds 15 (resp. 9) Matter
commissioning advertisements — exactly the number of CHIP's own
"Discriminator did not match" messages in the same log. The radio, NimBLE
and CHIP's scanner all work.

So the option-A verdict is **"the stack is alive, our parameters were
wrong"**, not "the BLE radio never comes up". The proof line to wait for is
`pairing_command: PASE session establishment success` — the controller prints
it when SPAKE2+ has finished against a real device. Until the sensor
answers, the CASE-session heap is still unmeasured.

**Four traps worth writing down.**

1. **The GCC/NOLOAD trap.** `EXT_RAM_BSS_ATTR` puts an object into
   `.ext_ram.bss`, which is a **NOBITS** section: zeroed at boot, never
   loaded from flash. A namespace-scope object with a constant-initialisable
   constructor is **constant-initialised** by GCC, and its vtable pointer is a
   *constant* emitted into the object's own section — which NOLOAD never
   loads. The vptr stays 0 and the first virtual call faults
   (`LoadProhibited`, EXCVADDR 0x8). `core::EventRing` (`g_ev_ring`) hit
   exactly that. The rule: **only plain data arrays and objects whose
   constructor runs at runtime may carry the attribute** — a function-local
   `static` with a non-trivial constructor is initialised at run time and is
   fine.
2. **A failed scan leaves the commissioner "busy".** CHIP reports
   `OnPairingComplete` only once a device has actually connected
   (`RendezvousCleanup` returns early while `mDeviceInPASEEstablishment` is
   null), so after a scan that found nothing the pairing delegate stays
   registered and the next attempt is refused with "There is already a
   pairing process" (esp_err 259). The firmware now calls `StopPairing()`
   (resets the pairer) and `RegisterPairingDelegate(nullptr)` before every new
   attempt, and has a `matter_pair_cancel` console command to abort a running
   attempt — both verified on the board: after a cancel the next
   `matter_pair_ble` starts again instead of answering 259.
3. **A cancel does not stop the radio.** CHIP's NimBLE layer implements
   `BLEManagerImpl::CancelConnection()` as `CHIP_ERROR_NOT_IMPLEMENTED`, so
   `StopPairing()` resets the pairer but leaves the GAP scan running until its
   60 s window expires. An attempt started right after a cancel therefore
   fails with `Failed to start a BLE scan: 3` →
   `PASE session establishment failure: Matter-Error CHIP:0x00000003` — the
   error is CHIP_ERROR_INVALID_STATE, not a device problem. Either let the
   old scan expire or reboot the brain (`DTR=False` on the console port gives
   `rst:0x15`, and the console stays alive through the boot).
4. **A Matter manual pairing code is bit-packed, not "discriminator then
   PIN".** The 11-digit code (`DDDD-DDD-DDDD`) packs a version bit, a
   vid/pid-present bit, a **4-bit** discriminator, the PIN split 14 + 13
   bits, and a Verhoeff check digit — so the digits you can read carry the
   discriminator and the PIN in an unreadable form. Decode the code with
   CHIP's `ManualSetupPayloadParser` (the python reference `SetupPayload.py`
   does the same) and check the Verhoeff digit before touching hardware. The
   control case is the esp-matter test DAC code `3497-0112-332` → it must
   decode to discriminator 3840 and PIN 20202021.

**A hard limit of the S3's Bluetooth controller: no LE Extended Advertising.**
The ESP32-S3 uses the original ESP32 controller (`lib_esp32`), which has no
extended-advertising support at all — no ext-adv symbols in the controller
archive, no `ble_ext_adv_max_size` in the SoC capability struct, and
`CONFIG_BT_NIMBLE_EXT_ADV` off by default (it depends on
`BT_NIMBLE_50_FEATURE_SUPPORT`). CHIP's ESP32 commissioner scans with the
legacy `ble_gap_disc` only, so it can never see a device that advertises
*exclusively* via extended advertising (Matter 1.4 enhanced commissioning,
service data 0xFFF1). The TIMMERFLOTTE uses legacy 0xFFF6 and is therefore
commissionable from the brain — but a commissioner on this silicon has that
ceiling: for R3 it means "the brain's radio only ever sees legacy
advertisers".

**The flash gate needed -Os.** The controller does not fit the 1.875 MB app
partition at IDF's default optimisation (`-Og`): the image was 0x1e0650 —
1616 B over the slot, so the board did not boot at all.
`CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (the vendor's own release setting)
brings it to **0x1de350** — 7344 B free. The switch lives in the Matter-only
overlay (`sdkconfig.defaults.matter`), so the normal PlatformIO build of the
brain stays untouched.

**PASE reached — two more traps in CHIP's ESP32 NimBLE backend (05.11.2026,
hardware-verified).** The radio was alive and the parameters were right, yet
every attempt still died before PASE — and the brain rebooted on the way out.
Both causes are in CHIP's ESP32 BLE backend, and both are fatal only because
CHIP treats a *recoverable* condition as fatal:

| trap | what actually happens | fix (`scripts/matter-patches.py`) |
|---|---|---|
| the ATT MTU exchange "fails" | `HandleGAPConnect()` calls `ble_gattc_exchange_mtu()`; on the S3 it returns **2** = `BLE_HS_EALREADY` ("already in progress" — *not* a link failure), and CHIP maps that to `CHIP_ERROR_INTERNAL` → *"Disabling CHIPoBLE service due to error: ac"* → the BLE layer is deinitialised → the freshly established connection is **torn down by CHIP itself**, before the commissioner ever gets to PASE | keep the connection and log the reason: a smaller MTU is survivable, a dead connection is not |
| the connect timeout cancels a dead link | 30 s later `HandleConnectTimeout()` → `CancelConnect()` → `ble_gap_conn_cancel()` → `ble_hs_is_enabled()` reads the **deinitialised** NimBLE host context → `LoadProhibited` → the brain **panic-reboots** (reproducible every round) | skip `ble_gap_conn_cancel()` — the connection we would cancel is gone anyway |

The patches live in `scripts/matter-patches.py` and are applied inside the
container by `scripts/matter-build.sh` (each patch verifies the exact source
text it replaces and hard-errors if the upstream text is gone, so a future
esp-matter release cannot silently skip one). With them the brain talks to the
real TIMMERFLOTTE:

```
I (67421) chip[CTL]: Commission called for node ID 0x000000000001B66A
I (67541) chip[BLE]: Device Discriminator match. Attempting to connect
W (67741) CHIP[DL]: ble_gattc_exchange_mtu failed: 2 (keeping the connection)
I (67741) chip[DL]: BLE GAP connection established (con 1)
I (69191) chip[BLE]: GATT discovery complete status:0 conn_handle:1
I (70081) chip[EM]: <<< … Type 0000:22 (SecureChannel:PASE_Pake1)
I (70191) chip[EM]: >>> … Type 0000:23 (SecureChannel:PASE_Pake2)
I (70881) chip[EM]: <<< … Type 0000:24 (SecureChannel:PASE_Pake3)
I (71051) pairing_command: PASE session establishment success
```

That is the proof line of option A, reached twice in a row. The BLE stack, the
CHIP commissioner and the sensor's GATT server all work on the S3-Zero; the hub
itself keeps running on that image (Wi-Fi up, SNTP synced, 48 KB internal heap
+ 1.75 MB PSRAM free after the attempt). With the trust store of the next
paragraph the run goes much further — through attestation and the NOC, all the
way to the Thread step, which is where R3 picks up.

**Attestation is the next wall (05.11.2026 — passed).** The commissioning ran
past PASE and died in the certificate step:

```
I chip[CTL]: Received certificate chain from the device
I chip[CTL]: Successfully finished commissioning step 'SendAttestationRequest'
E chip[-]: Unable to find PAA, err: 4a, PAI's AKID: 6B:31:8C:FC:E9:99:…
E chip[CTL]: Error on commissioning step 'AttestationVerification': 'Error CHIP:0x00000020'
I chip[CTL]: Failsafe disarmed
I chip[CTL]: Commissioning complete for node ID 0x000000000001B66A: Error CHIP:0x00000020
```

The device's DAC is signed by a real PAA, and the commissioner's trust store
holds only the **two hardcoded Chip-Test PAA certificates**
(`CONFIG_ESP_MATTER_COMMISSIONER_ATTESTATION_TRUST_STORE` defaults to
`TEST_ATTESTATION_TRUST_STORE`). esp-matter offers three more:
`DCL_ATTESTATION_TRUST_STORE` (fetch the PAA from the CSA's Device
Certification List over HTTPS — `…/dcl/pki/certificates?subjectKeyId=…`),
`SPIFFS_ATTESTATION_TRUST_STORE` (the PAA in a `paa_cert` partition) and
`CUSTOM_ATTESTATION_TRUST_STORE` (our own implementation). The sensor is left
clean — the failsafe is disarmed, so it stays in commissioning mode and the
attempt can be repeated.

**Our own PAA trust store (05.11.2026, hardware-verified).** We took the custom
route: `firmware/brain-s3/main/matter_paa.{hpp,cpp}` implements
`chip::Credentials::AttestationTrustStore` with the **IKEA production PAA**
("IKEA of Sweden Matter PAA G1", 487 bytes of DER, taken from the CSA
certification list) compiled in and looked up by the key id the PAI's AKID names
— that is the PAA's own SKID. Three details cost a build each:

- the store has to be installed **before** `setup_commissioner()` — esp-matter
  reads `get_attestation_trust_store()` inside it
  (`firmware/brain-s3/main/main.cpp::matter_bringup`);
- `CONFIG_CUSTOM_ATTESTATION_TRUST_STORE=y` in `sdkconfig.defaults.matter` is
  **not enough**: kconfgen does not override a value that already exists in
  `sdkconfig`, and the choice had already been resolved to the test store — the
  resolved `sdkconfig` has to be switched as well;
- the declaration of `set_custom_attestation_trust_store()` sits behind
  `#ifdef CONFIG_CUSTOM_ATTESTATION_TRUST_STORE`, so with the wrong choice the
  call does not even compile ("is not a member of 'chip::Credentials'").

With the store in place the commissioning runs the **whole chain** — from PASE
through the certificate exchange to an operational credential — and stops only
where it has to, at the Thread step:

```
I chip[CTL]: Successfully finished commissioning step 'SendAttestationRequest'
I chip[CTL]: Performing next commissioning step 'AttestationVerification'
I chip[CTL]: Successfully finished commissioning step 'AttestationVerification'
I chip[CTL]: Successfully validated 'Attestation Information' command received from the device.
I chip[CTL]: Successfully finished commissioning step 'SendNOC'
I chip[CTL]: Performing next commissioning step 'ThreadNetworkSetup'
I chip[DMG]: Received NetworkConfig response, networkingStatus=1
E chip[CTL]: Error on commissioning step 'ThreadNetworkSetup': 'Error CHIP:0x000000AC'
I chip[CTL]: Successfully finished commissioning step 'Cleanup'
```

`ThreadNetworkSetup` fails because we deliberately hand over an **empty
dataset**: the brain has no 802.15.4 radio and no Thread network of its own, and
`networkingStatus=1` is the device refusing that empty dataset. This is the R3
wall, not a bug — it needs the dataset from the C6 (as a pure OTBR) over a new
hubproto message, and `pairing_ble_thread` with a real dataset in hand.

The device stays usable: the failsafe was armed for 60 s
(`ArmFailSafe response errorCode=0`) and the aborted run left it to expire on
its own, so the sensor reboots itself, forgets the operational credential it
was given and returns to commissioning mode.

**Flash is the constraint for the DCL route.** The image now ends at
`0x1deac0` — **0x1540 (5.4 KB) free** in the 1.875 MB slot — while the DCL client
pulls in the HTTP client, TLS and cJSON. The 4 MB flash has 128 KB unused
after `ota_1` (`0x3E0000…0x400000`), but that is not an app slot. So the DCL
route means F1' (shrink the image) or F5' (an 8 MB S3). The custom trust store
costs ~2 KB and fits today — but it is **vendor-specific**: every vendor we
want to commission needs its PAA added (or the DCL route, or the SPIFFS
partition with a folder of PAAs, once there is flash for it).

**A third trap: the console REPL's stack is too small for a commissioning call
(05.11.2026).** With the trust store in place the *next* attempt died
differently — the brain panic-rebooted with

```
***ERROR*** A stack overflow in task console_repl has been detected.
```

The console command called `esp_matter::controller::pairing_ble_thread()`
directly, so the whole CHIP call chain — the PAKE setup, the BLE bring-up, the
crypto — ran on the console REPL task's 8 KB stack, on top of the REPL's own
frames. The fix: the console command only checks the arguments and hands the
request to a dedicated task (`matter_cmd`, 12 KB of stack), which is created
while the internal heap is still one big block — the CHIP stack wants a large
contiguous chunk later on. The call itself needs **~3.7 KB** (the worker task
reports its own high-water mark), so the stack is comfortable now:

```
I matter: commissioning task up (12288 B of stack)
I matter: worker done (ESP_OK), 8588 B of its stack left
```

Verified on the board: with the task in place the commissioning runs the same
chain as before (PASE → attestation → NOC → `ThreadNetworkSetup`) and the hub
survives it — no stack overflow, no reboot, 34.5 KB of internal heap left
afterwards.

The price is 12 KB of internal RAM: the hub has **35.7 KB free** (was 48.8 KB)
and its largest free block is 25.6 KB (was 31.7 KB).

**R3: the Thread dataset comes over the link (05.11.2026 — written, not yet on
hardware).** `ThreadNetworkSetup` wants the **operational dataset TLVs** — the
bytes of the Thread network the device is supposed to join. The brain has no
802.15.4 radio and therefore no dataset of its own; the co-processor does (it
runs OpenThread and forms the network). So the dataset travels over the link:

| step | where | what |
|---|---|---|
| `THREAD_DATASET_REQ` (0x62) | brain → chip 0x02 | "give me your active dataset" |
| `THREAD_DATASET` (0x63) | chip 0x02 → brain | `len(u8) bytes[]` — the Thread TLVs as they are |

The chip side hands them over straight out of OpenThread
(`otDatasetGetActiveTlvs()`; 254 bytes — the Thread maximum *and* the size of
CHIP's own buffer, so one frame carries it). The brain checks them before
trusting them (`core::ThreadDataset`, portable and host-tested): the TLVs have
to parse exactly, and the three things a device cannot do without — the
channel, the PAN id and the network key — have to be in there. A truncated or
key-less dataset would otherwise make the device attach to nothing and fail
with a message that says nothing about the cause.

The commissioning then runs the same BLE path as before, but with a real
dataset in hand: `pairing_ble_thread(node_id, pin, disc, dataset, len)`. The
console command is `matter_pair_thread <node_id> <pincode> <discriminator>`: it
asks the co-processor, waits up to 15 s for the answer, prints the channel and
the network name, and only then hands the request to the `matter_cmd` worker.
The dataset lives in a static object, not on a stack: CHIP keeps the pointer
for the whole multi-minute commissioning.

**Two build fixes and one move, on the way to the image.** The thread-c6 image
did not build for two reasons, both in the container's CHIP sources: the ESP32
platform event header uses `chip::System::PacketBuffer` without including the
header that declares it (the file does not compile at all once esp-matter's BLE
controller is enabled — the fifth patch in `scripts/matter-patches.py`, now
applied for both apps), and CHIP's own sources are not portable enough for this
toolchain's format warnings (a `uint32_t` printed with `%u`) while the build
runs with `-Werror=all` — so the thread-c6 turns the format family off, like
the brain-s3 does.

And the console moved: in ESP-IDF 5.5 the C6's console *is* the USB-Serial-JTAG
peripheral by default, and `esp_console_new_repl_uart()` is declared only under
`CONFIG_ESP_CONSOLE_UART_DEFAULT` — the old REPL call did not compile at all.
The REPL (`thread>` with `factoryreset` and `reboot`) and the log output now go
to the same USB connector we flash through, which is exactly what the R3 test
needs: the thread-c6's answer has to be readable.

**Not yet on hardware.** The thread-c6 has to be connected and flashed with
this firmware — the board plugged in here is the zigbee-c6, and the C6s are
untouched under the standing rule «Пока не трогать C6». The brain's Matter
image ends at **0x1df2c0** — 0xd40 (3.3 KB) free in the 1.875 MB slot, so the
dataset path cost ~2 KB of the 5.4 KB that were free. The thread-c6 image now
builds again and fits with room to spare: **0x1b6110** (1.75 MB) in the 1.92 MB
slot, **0x29ef0 (168 KB, 9 %) free** — 175 KB smaller than the November build,
because the CHIP *controller* is not linked into the bridge image (the
commissioner lives in the brain; the bridge is a server). What the answer will
look like depends on the co-processor's own Thread network: if OpenThread runs
there without an active dataset, the chip says so and the dataset cannot be
lent out yet.

**R3, second attempt: the co-processor becomes a pure Thread host (06.11.2026).**
The bridge code is out of the thread-c6 — the Apple Home side of it is what
took ~190 KB of static DRAM (SPEC §11, «the bridge is heavy»). What is left on
that chip is what it was always good for: an OpenThread instance with a real
802.15.4 radio. So the chip is now **the hub's Thread network host**:

- it forms (or keeps) the network `keystone-home` on channel 15 and stays the
  leader — its own log says `role=leader network=keystone-home`;
- the operational dataset lives in NVS (`thread_net`/`ds`), so a reflash of the
  chip does not change the network the devices were commissioned into (a fresh
  key would leave every device behind);
- it lends the dataset to the brain over the link (0x62/0x63, the table above),
  which is exactly what a Matter commissioner needs to hand a device;
- it answers `MatterPair` with `unsupported`: the CHIP stack is gone from this
  chip, the commissioner lives in the brain.

Two things about the OpenThread port on the C6 are worth writing down, because
both cost an afternoon:

1. **`esp_vfs_eventfd_register()` must come before `esp_openthread_init()`**
   (`max_fds = 3`). Without it the port does not come up at all, and the
   failure is silent — no OpenThread, no error.
2. **Every call into OpenThread has to hold the port lock**
   (`esp_openthread_lock_acquire()` / `_release()`), because the instance belongs
   to the OpenThread main-loop task: reading the dataset while the main loop is
   halfway through a dataset update gives you half a dataset.

**The dataset frame does not arrive (open).** The chip answers the request and
logs it — `sent the Thread dataset (109 bytes) to the brain` — but the brain
never sees it: `matter_pair_thread` waits out its 15 s and prints «no dataset in
15000 ms». Everything around it is proven:

- the **protocol** is proven by host tests — a 109-byte dataset through
  `encode_frame`/`FrameReader` (byte by byte and in one piece) and through the
  real `UartTransport` over `InMemoryLink` in the chip→brain direction
  (`libhubproto/test/test_payload_thread_dataset.cpp`,
  `core/test/test_uart_transport.cpp`; 411/411 host tests green);
- the **link** is alive in that direction — the brain sees the chip's telemetry
  (heap, uptime, tx_frames, crc_errors) and the chip's own log shows it
  answering the request, so the request reached the chip;
- the **frame** is the only big frame in the system (122 bytes on the wire; the
  telemetry frames are ~40), which is why it gets its own test.

The frame layer is silent when it drops a block, so "the frame was lost" and
"the frame never left the chip" look the same from the outside. Two
diagnostics were added for the next run:

- the chip says whether the frame was really written
  (`sent the Thread dataset (…) to the brain: written` / `WRITE FAILED`);
- the brain counts what its own link dropped (`UartTransport::bad_frames()`),
  logs it when the counter grows («link dropped frames: zigbee N, thread N») and
  prints it per chip in the `chips` console output (`crc_errors …, dropped …`).

With those two numbers the next run splits the case in two: the counter grows
→ the frame arrived and was rejected (COBS/CRC); the counter stays → the
frame never reached the brain's UART.

**One false lead, closed (06.11.2026).** The brain's console showed
`ROUTE_HOOK: Received RIO` / `prefix FD56:BFEE:8601:: lifetime 1800` every
minute or so, and the prefix looks exactly like an OpenThread mesh-local
prefix — as if the brain had joined the hub's Thread network. It has not: the
string is **lwIP's IPv6 NDP route hook** on the Wi-Fi interface (the tag
`ROUTE_HOOK` and the neighbouring format strings `Received RIO`,
`prefix %s lifetime %lu` sit in the brain's own image next to the Wi-Fi
strings), and the prefix is the ULA prefix the *home router* advertises. The
S3 has no 802.15.4 radio and no OpenThread at all (`CONFIG_OPENTHREAD_ENABLED`
is off; only `CONFIG_ESP_MATTER_ENABLE_OPENTHREAD` is on, which is the CHIP
stack's own knob).

**The probe (06.11.2026).** `scripts/thread-dataset-probe.sh` does the whole
round in one run: it finds the two boards by their USB MAC (the ports swap
when a board is re-plugged!), checks each board's console prompt (`home>` /
`thread>`) so the wrong one cannot be flashed, flashes the two rebuilt images
(without `erase_flash` — the Thread network and the chip's dataset survive),
asks the brain for the dataset and prints the lines that answer the question.
The images are built (thread-c6 623 KB, brain-s3 0x1df3f0 with 3 KB of flash
to spare) but **not yet flashed** — writing firmware to a board needs the
person's go-ahead.

**R3, third attempt: the dataset arrives, the device joins the Thread network,
the operational channel does not (07.11.2026).** Both boards were flashed with
the rebuilt images (the brain — ESP32-S3 on `/dev/ttyACM0`, MAC
`10:51:DB:5E:F8:94`; the co-processor — ESP32-C6, MAC `AC:EB:E6:DA:FC:08`;
the tty numbers swap when a board is re-plugged, so find them by MAC). Both
came up:

- the brain: `Wi-Fi IPv6 link-local address created` → `controller up (node
  112233, fabric 1, UDP 5580): heap 93779 B (-12096 B), largest block 31744 B`
  → **`bus fan-out: 9 subscribers (room for 12)`**. The ninth subscriber is
  the cross-chip forwarder — which is also what unpacks the Thread dataset —
  and with the old eight-slot table it was silently dropped. That was the real
  reason the dataset "never arrived": not the link, not the frame, a full
  subscriber table (`dropped 0` on both links the whole time);
- the chip: OpenThread up, `role=leader network=keystone-home`.

`matter_pair_thread 4660 39441506 569` on the brain:

```
matter: asked chip 0x02 for its Thread dataset, waiting 15000 ms...
matter: got the Thread dataset (109 bytes, channel 26, name 'keystone-home')
```

— **the frame from the co-processor reached the commissioner.** The open
question of 06.11 is closed.

The commissioning then went all the way, further than it ever has:

```
chip[BLE]: Device Discriminator match. Attempting to connect
CHIP[DL]: ble_gattc_exchange_mtu failed: 2 (keeping the connection)   ← the NimBLE patch
pairing_command: PASE session establishment success
AttestationVerification ✓  AttestationRevocationCheck ✓  ValidateCSR ✓
GenerateNOCChain ✓  SendNOC ✓
chip[CTL]: Successfully finished commissioning step 'ThreadNetworkSetup'
chip[CTL]: Successfully finished commissioning step 'ThreadNetworkEnable'
```

and the device **joined the Thread network**: the chip reports `0x76=4`
(leader) and `0x77=1` (one child — the sensor).

**The wall is now the operational channel (CASE).** The commissioner sends the
first CASE message to the device's Thread address:

```
chip[EM]: >>> Msg TX ... [UDP:[fd7c:568:d52b:1:ff4d:c458:f9f4:f3c8]:5540]
         --- Type 0000:30 (SecureChannel:CASE_Sigma1)
```

and gets nothing: four retransmissions die, `Session establishment failed for
<0000000000001234, 2>, error: 32` (timeout), CHIP extends the fail-safe and
retries. Why: the hub is on Wi-Fi, the device on Thread, and the only path
between them is the co-processor as a Border Router. The host→Thread direction
needs the BR to translate the host's address into a Thread address (the
MeshCoP Address Table — the "NAT" of a Border Router); without that
translation the Sigma1 has nowhere to go. The hub *does* learn the route for
the Thread prefix (`ROUTE_HOOK: prefix FD7C:568:D52B:1::`), so the prefix
advertisement works — what is missing is the address translation on the BR
side. Open for the next run:

- does this build's Border Router run the TREL/Address Table at all?
  `CONFIG_OPENTHREAD_BORDER_ROUTER=y` only arms the BR path; the translation
  is a separate mechanism in the border-router example;
- the chip's own `ot` CLI answers the question (trel/nat state) — but opening
  the C6's USB console resets it and the sensor has to rejoin, so read that
  state *before* a commissioning run, not after.

Side observations from the same run: the Zigbee co-processor is flapping (four
reboots in five minutes) and the IKEA plug (entity 0x0100) is offline —
`zigbee-c6: NWK addr lookup failed for entity 0x0100 (offline?)` — which
matches the plug being unplugged.

**The address translation is *not* the wall (07.11.2026, later the same day).**
Read in the OpenThread sources of the IDF 5.5 image and measured on the chip
(`thread` command on the co-processor):

- the network data publishes exactly **one** on-mesh prefix — the Border
  Router's own OMR prefix `fd7c:568:d52b:1::/64` (`omr prefix:
  fd7c:568:d52b:1::/64`, `omr in netdata: yes`, and the on-mesh table has only
  that one entry). The dataset's Mesh-Local prefix `fdc7:d07f:ab6f:6eef::/64` is
  *not* published as an on-mesh prefix;
- the sensor's operational address in the fabric is therefore the OMR one:
  `fd7c:568:d52b:1:ff4d:c458:f9f4:f3c8` = the OMR prefix + the IID of its
  Mesh-Local address;
- **OpenThread has no NPT/MLAT** (grepping the OT sources finds only the NAT64
  translator): it never swaps the OMR and ML prefixes. But it does not have to.
  `Ip6::IsOnLink()` answers "yes" for any address inside an *on-mesh* prefix,
  and the OMR prefix is exactly that — so `Ip6::DetermineAction()` sets
  `aForwardThread` and the packet goes into the mesh. The BR forwards to the
  OMR address, and the mesh resolves the device's RLOC16 from the IID (address
  query) as usual;
- the hub's side of the path works too: the co-processor advertises the OMR
  prefix upstream as a RIO, and the brain's Matter stack keeps it in its route
  hook (`ROUTE_HOOK: prefix FD7C:568:D52B:1::`) with the gateway = the
  co-processor's backbone link-local `fe80::aeb:e6ff:feda:fc08`.

So the "NAT" the previous entry looked for is not needed for the host→Thread
direction. The missing piece is one hop earlier: **the Sigma1 never reached the
co-processor's OT stack at all** — `br counters: in_unicast=0 pkt/0 B
out_unicast=0 pkt/0 B` (the OT border-routing counters: *in* = from the mesh,
*out* = into the mesh).

Two more measurements from the same day:

- the co-processor's **reset reason** after the previous run is `interrupt
  watchdog` — a hang, not a brownout. It did **not** repeat when only the
  dataset was requested (`sent the Thread dataset (109 bytes, 110 byte
  payload) to the brain: written`, and the chip kept running afterwards), so
  whatever hung it needs the commissioning traffic, not the dataset;
- the **partition ID no longer churns**: `0x851f9b0` is the same before and
  after a reboot (the earlier churn — `0x6076210b → 0x707e2214 → 0x751bb2fe` —
  was from before the dataset was kept in NVS);
- the sensor is **detached** (`children=0`) and **no longer advertises BLE** —
  the commissioning scan of 07.11 sees only other devices' advertisements. A
  new commissioning therefore needs a **factory reset of the TIMMERFLOTTE**
  (the physical button), after which it advertises again and can be
  commissioned into our fabric once more.

**A diagnostic for the hop that failed** (new console command on the brain,
Matter build only):

```
ip6                 the Wi-Fi interface's IPv6 addresses
ip6 <address>       the route lwIP would take (netif + gateway), from the Matter stack's route hook
ip6 probe <address> [port]
                    send one UDP datagram and report what lwIP made of it
```

`ip6 probe` is the cheap way to answer "does a packet reach the mesh?": the
co-processor counts what enters the mesh (`out_unicast`), so a probe plus a
`thread` command on the chip settles the question without a device — and
without touching the sensor.

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

**The wall was the missing interface, not the address translation (07.11.2026,
evening).** `br counters: in_unicast=0` means the packet never got into the
OpenThread stack — and the reason is now read out of the IDF 5.5 sources and
confirmed on the chip:

- our co-processor started the stack with the **legacy** `esp_openthread_init()`.
  That call brings up the platform, the instance and the radio — and **no lwIP
  interface**. The "ot" netif is made by the modern `esp_openthread_start()`:
  its worker task does `esp_netif_new(&config->netif_config)` and
  `esp_netif_attach(netif, esp_openthread_netif_glue_init(...))`
  (`components/openthread/src/esp_openthread.cpp`, `ot_task_worker`). The IDF's
  own Border Router example (`examples/openthread/ot_br`) uses exactly that
  call, with `.netif_config = ESP_NETIF_DEFAULT_OPENTHREAD()`;
- without that netif lwIP has **no interface into the mesh at all**: the next
  hop for a mesh address cannot be resolved (`nd6_get_next_hop_entry()` →
  `ERR_RTE`), so every backbone packet to a mesh address dies in lwIP before
  the OpenThread stack sees it. The chip's own `thread` command said it
  plainly: `ot netif: none`;
- the netif also needs an **address** of the prefix the mesh lives in, or lwIP
  has no on-link route. That address is what the SLAAC module forms out of the
  OMR prefix the BR publishes (the BR flags it `mSlaac`). The module is **not
  in the IDF build of the OT core**: `OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE`
  defaults to 0 in `openthread/src/core/config/ip6.h`, so neither
  `slaac_address.cpp` nor its notifier hook is compiled in. Upstream's own
  Border Router config (`openthread/examples/config/ot-core-config-check-size-br.h`)
  turns it on. Ours does too, through a custom OT config header:
  `main/esp_ot_custom_config.h` (`#define OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE 1`)
  plus `CONFIG_OPENTHREAD_HEADER_CUSTOM=y` and the path/file name in
  `sdkconfig.defaults.esp32c6`.

So the change on the co-processor is: start the stack with
`esp_openthread_start()` (and let it run the main loop in its own task — our
own `ot_mainloop_task` is gone), and keep the SLAAC module in. The first boot
of that image confirms the interface exists: `OPENTHREAD: OpenThread attached
to netif`.

**Two more traps of that same boot (both fixed).** The image with the netif
froze right after `the Thread role is enabled`, with the console dead (the
`thread` command waits on the OT lock) and the Wi-Fi gone at the 27th second
(`bcn_timeout`):

- the freeze was a call to `esp_openthread_set_backbone_netif()` made **under
  the OT lock** — the IDF's example calls it in the same place, but there it is
  a no-op for us: the co-processor had already been given its backbone netif
  (right after the stack came up), and the second call did nothing good. The
  backbone netif is now handed over once, outside the lock, right after
  `esp_openthread_start()`;
- the Wi-Fi loss was the **coexistence arbiter**: on the C6, Wi-Fi and 802.15.4
  share one radio, and the 802.15.4 side goes active the moment the Thread
  role is enabled. The arbiter was armed only *after* the Border Router
  started — long after the radio had begun to starve the Wi-Fi. It is now
  armed **before** the role is enabled (in `thread_network_task`, right before
  `thread_bring_up()`), so the Wi-Fi keeps its access point.

The log lines around mDNS and the BR start were kept (`starting mDNS`,
`setting the mDNS hostname`, `starting the Border Router`) — the freeze above
was found by them, and the next freeze should be too.

**The second freeze was a deadlock between the OT lock and mDNS (07.11.2026,
night).** The image with the netif in it froze again — this time right after
`starting mDNS` — and the Wi-Fi died 13 s later (`bcn_timeout`,
`wifi:state: run -> init`). The cause is in the IDF sources, and it is a real
deadlock, not a slow call:

- `esp_openthread_lock_acquire()` takes **two** recursive mutexes
  (`components/openthread/src/esp_openthread_lock.c`): `s_openthread_mutex` and
  `s_openthread_task_mutex` (the task-switching lock). The OpenThread main
  loop takes the same lock around every iteration
  (`esp_openthread.cpp`: `esp_openthread_platform_process()` +
  `otTaskletsProcess()`), so while we hold it, **the OT stack does not run at
  all**;
- the tx path of the "ot" netif takes the task-switching lock too
  (`esp_openthread_netif_glue.c`: `openthread_netif_transmit()`), so with the
  lock held, any lwIP packet that goes out through that netif blocks;
- `mdns_init()` is an lwIP thing: it waits for the lwIP thread
  (`mdns_priv_if_init()` → `tcpip_api_call(pcb_if_init_lwip, ...)`, which joins
  the ff02::fb multicast group and sends the MLD report).

So: our task holds the OT lock and waits for the lwIP thread; the lwIP thread
can end up in the "ot" netif's tx path and waits for the task-switching lock we
are holding. Neither ever gets there — and the Wi-Fi driver, starved of the
lwIP lock, stops seeing beacons.

The fix is to keep the OpenThread lock away from anything lwIP-ish: the mDNS
start is now a separate step (`thread_bring_up_mdns()`) that runs **outside**
the lock, and only the Border Router call itself takes the lock, for its own
duration. Verified on the chip: `mDNS is up` → `mDNS hostname is 'keystone-br'`
→ `Border Router is up (SRP server + DNS-SD + border routing)`, the Thread
role comes up as **leader** on `keystone-home`, the Wi-Fi keeps its access
point (no `bcn_timeout` any more) and the netif reports `OT_STATE: netif up`.

**And the mDNS step has to come BEFORE the Thread role, not after.** The Border
Router publishes its Thread network as `_meshcop` the moment the OT state
changes (`esp_openthread_state.c` → `esp_openthread_publish_meshcop_mdns()`),
and that publish needs the mDNS **hostname** to be there already — the mDNS
component keeps every service under the hostname, and a service published
without one is dropped without a word (`Failed to publish meshcop mdns
service`). In the boot order above the hostname was set *after* the role was
enabled, so the publish was lost: a raw mDNS query on the LAN
(`_meshcop._udp.local`) gets an answer from Apple's Border Router and from
other Matter devices, but **nothing from our co-processor (192.168.0.201)**.
The order is now `mDNS + hostname` → Thread role → Border Router.
**Not yet verified on the hardware** (the image with this order is built; the
flash needs the go-ahead).

**Not yet re-verified on the hardware**: with the netif in place, the
Sigma1 has to reach the mesh at all (the BR counters must start moving), and
the CASE session has to come up. That needs a **factory reset of the
TIMMERFLOTTE** (the physical button — the sensor is detached and no longer
advertises BLE) and a fresh `matter_pair_thread <node_id> <pincode>
<discriminator>` on the brain.

**The CASE destination was the device's OMR address all along (08.11.2026).**
The raw log of the failing run (not a summary of it) says:

```
159:I (741173) chip[DIS]: UDP:[fd7c:568:d52b:1:ff4d:c458:f9f4:f3c8%st1]:5540: new best score: 3 (for B9208C6B9B867EED-0000000000001234)
162:I (741353) chip[EM]: <<< ... [UDP:[fd7c:568:d52b:1:ff4d:c458:f9f4:f3c8]:5540] --- Type 0000:30 (SecureChannel:CASE_Sigma1)
```

and the co-processor's console for *that* network said:

```
mesh-local prefix: fdc7:d07f:ab6f:6eef::/64
omr prefix:        fd7c:568:d52b:1::/64
on-link prefix:    fd5e:986a:bb23:be7e::/64
```

So `fd7c:568:d52b:1::/64` was that network's **OMR (routable, domain) prefix** —
its mesh-local prefix was `fdc7:…`. The address only *looks* mesh-local:
`fd7c:568:d52b:1::/64` is Thread's **default** mesh-local prefix (RFC 6603),
which is why two readings of the same line disagreed for a week.

⇒ **No address translation is needed — and none could help.** The device was
addressed correctly; the packet had nowhere to go. Two things were missing that
day, and both had to be fixed:

- the brain had **no route** to that network's OMR prefix. The BR advertises
  its own OMR prefix upstream as a RIO, and the brain picks it up with CHIP's
  route hook (`ROUTE_HOOK: Received RIO / prefix FD3C:1C91:4A38:1:: lifetime
  1800`) — but only while that network is the current one. The network was
  reformed in between (old OMR `fd7c:568:d52b:1::/64`, current
  `fd3c:1c91:4a38:1::/64`), so the RIO the brain held pointed at a prefix the
  device was not in, and the packet died in the home router;
- the co-processor had **no lwIP interface into the mesh** (the legacy
  `esp_openthread_init()`), so even a routed packet would have died on its side
  with `ERR_RTE` before OpenThread saw it — hence `br counters: in_unicast=0`.

Both are in place now: the "ot" netif exists and holds an address *in* the
OMR prefix (`fd3c:1c91:4a38:1:788:e3e8:31d2:e757`), which is what lets lwIP
route into the mesh, and the brain's route to the OMR prefix is the BR's
link-local (`FE80::AEEB:E6FF:FEDA:FC08`).

**What is left is one physical action.** The sensor is detached
(`children=0`) and does not advertise BLE any more (the scan of 07.11 saw only
other people's advertisements), so it has to be **factory-reset** — the button
on the TIMMERFLOTTE — before it can be commissioned into the current network.
Then one `matter_pair_thread 4660 39441506 569` should put the Sigma1 out to
`fd3c:1c91:4a38:1:…`, and the co-processor's `br counters: in_unicast` is the
check that the packet really entered the mesh (and `out_unicast` on the way
back). If the counters move but CASE still fails, the next thing to look at is
the brain's **source** address: the packet has to leave with an address the
mesh can answer to (the on-link prefix the BR advertises), not with the Wi-Fi
link-local — that is what the brain's `ip6` command prints.

**The crash-loop recovery was re-forming the network behind our backs (08.11.2026).**
The co-processor keeps the Thread dataset in NVS, and NVS is also what the
crash-loop rule erases (`boot_id > 50` ⇒ `nvs_flash_erase()`): a wiped dataset
means a **new network** — a new PAN id, new keys, a new mesh-local prefix and a
new **OMR prefix** — and every device commissioned into the old one is orphaned
for good. It stays in a network that no longer exists, and the brain's route to
that network's OMR prefix points nowhere. The network was re-formed three times
in two days, each time with a different Ext PAN ID.

So the dataset is now read out **before** the wipe and written back **after** it
— only the crash-looper's leftovers are lost — and our own copy of the dataset
(`thread_net`/`ds`) is kept in step with the stack's on every boot, so there is
always something to restore. The **identity** of the network (its Extended PAN
ID) is in the boot log now (`the Thread network is e22c46678647f540 (channel 26,
109 bytes of dataset)`) and in the commissioning log on the brain: a network
that changed from boot to boot is visible at once instead of turning up as a
mystery a week later. Built, not yet flashed.

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

### 10. One hub, one interface: every device as a bridged endpoint

The goal: when the hub joins Apple Home / Google Home / anything else, the
outside world sees **one accessory — the hub** and every device behind it as
an endpoint on it. Not one Matter accessory per device, not a mix of
"this one is Zigbee, this one is Matter, this one is this hub" — one hub
whose UX is the same whichever radio a device runs on.

**Half of this already exists.** `brain-s3`'s core bus is unified:
`core::EntityRegistry` holds every entity the same way, `Bus` carries the same
DECLARE/STATE/COMMAND/CMD_ACK whether a message came from the Zigbee
co-processor (chip 0x01), the Thread co-processor (chip 0x02) or the brain's
own Matter commissioner (pseudo-chip 0x03). The hub's own app already reads
everything through one protocol (`docs/app-protocol.md`). Internal
unification is done.

**What is missing is the external side.** `brain-s3` today runs Matter only
as a **controller** (it commissions others); it has no **accessory/server**
stack, so Apple Home has nothing to add. The thread-c6 used to serve that
role (aggregator endpoint 0x000E + dynamic bridged endpoints per Zigbee
entity), but that implementation came with ~190 KB static DRAM and we traded
it away in commit `b9e8897` to make thread-c6 a clean Thread host.

**The subtlety that worried the user: can a native Matter device become a
bridged endpoint on our bridge?** From the **same** fabric — no (it already
is a node of that fabric; a bridge endpoint would be a second representation
of the same thing). From **another** fabric — yes. Matter 1.3 § 9.13 says a
Bridge may expose "Matter nodes that are not part of the current Fabric
(e.g. Matter devices from another ecosystem's Fabric)." That is exactly our
shape: the hub owns one fabric, Apple Home owns another; TIMMERFLOTTE is a
node in ours; we re-publish it to Home's fabric as a bridged endpoint on the
hub's aggregator. Home Assistant's Matter Server does this today, which is
the practical confirmation that esp-matter-class stacks can run it.

Shape of the plan:

- Enable the server/accessory side of esp-matter on brain-s3 **in addition
  to** the controller. Two fabrics, both live:
  - **Fabric A (hub's)** — brain-s3 is the commissioner; real Matter devices
    (TIMMERFLOTTE) are nodes here.
  - **Fabric B (Apple Home's)** — brain-s3 is a commissioned accessory, and
    the only one it needs to know about.
- Build the data model in Fabric B: root endpoint + aggregator (device type
  0x000E) + one bridged endpoint per entity in `EntityRegistry`.
- Pick the device type per entity from the same table the app uses
  (`core::zcl_profile` for Zigbee, `core::matter::device_type` for Matter
  nodes we own). On/Off plug → Device Type 0x010A; temperature+humidity
  sensor → compose 0x0302 + 0x0307; etc.
- Route reads/writes/subscriptions on a bridged endpoint to `Bus` — the
  same Bus the console and the app already drive. STATE on Bus → attribute
  update on Fabric B; command on Fabric B → `Dispatcher::send` → Bus →
  CMD_ACK → attribute mirror.
- Setup code / QR for the hub itself (printed on a label or shown by the
  app). Open commissioning window for Fabric B on first boot when no admin
  has joined yet.

What makes this costly: **RAM**. After `esp_matter::start` + controller
up, brain-s3 has ~115 KB internal RAM free and 1.7 MB PSRAM. The server
stack, ACL, SRP, fabric-B CASE sessions and N bridged-endpoint cluster
servers all land on top. Before writing any of it, measure: build the
minimum server+aggregator+one bridged endpoint, flash, read the symbol map
+ `heap_caps_get_info`, decide whether it fits with room for growth
(~32 KB headroom, per the controller's rule of thumb). If it does not:

- F1 — split clusters: keep a **slim** per-bridged-endpoint cluster set
  (Identify + the one device-type cluster + Descriptor), drop every
  diagnostic cluster.
- F2 — **host the server on thread-c6 instead of brain-s3** — the setup
  that Phase 8 Stage D already proved. brain-s3 keeps the controller,
  thread-c6 adds back the accessory + bridge + aggregator; the hub bus
  already carries the entities and the Phase 8 wire to the bridge
  (`on_bus_forward`) is in git history, removed in `b9e8897`.
- F3 — move the brain to Linux (SPEC § 11); the RAM problem disappears,
  the Matter server runs as a normal process, and the hub keeps the same
  Bus. This is already the stated long-term direction, but it is a big
  step.

**RAM measurement (13.11.2026, hardware), thread-c6 as a pure Thread host
in steady state** (OT leader + Border Router + SRP + mDNS + Wi-Fi
coexistence + one child in the mesh). Uptime ~40 s.

| | bytes | KiB |
|---|---:|---:|
| sram_seg (chip total) | 452 112 | 441.5 |
| static (.iram0.text + .dram0.data + .dram0.bss) | 199 236 | 194.6 |
| heap free now | 224 488 | 219.2 |
| largest free block | 204 800 | 200.0 |
| minimum free since boot | 221 220 | 216.0 |

Two things about the shape of this heap: the **largest free block is
200 KB**, so the heap is not fragmented — CHIP's one-chunk allocation and
NimBLE's own ~30 KB chunk both fit as a single piece. And **peak usage
is 3 KB above the stationary** (minimum free vs current), so the
network's active life does not spike the floor — the 224 KB is honest.

Adding `esp_matter` accessory + bridge **without** the controller (which
stays on brain-s3) grows static DRAM by ~60–100 KB (CHIP server + NimBLE
backend + bridge data model) and runtime heap by ~30–50 KB (CASE sessions
in fabric B, SRP registrations, ACL, N bridged-endpoint cluster servers).
Worst-case post-addition free heap: ~75 KB — well above the 32 KB
headroom target. Phase 8 Stage D hit 181 KB free with a heavier config
(controller + bridge together), which is the independent confirmation
that F2 is not a tight fit but a comfortable one.

**Verdict:** F2 is the recommended fork. The next moves are concrete:

1. Cherry-pick the Matter accessory + bridge code that lived on
   thread-c6 before `b9e8827` (search git for `app_bridge_*`,
   `BridgedDevice`, `create_bridged_clusters`); restore it next to the
   current Thread host + Border Router instead of replacing it.
2. Restore `on_bus_forward` on brain-s3 so DECLARE/STATE/COMMAND for
   Matter-owned entities (chip 0x03) is mirrored to the thread-c6 bridge
   over UART, the same way Zigbee entities (chip 0x01) are mirrored today.
3. Setup code + QR for the hub itself. Print on the brain console; later
   the app shows it on first-run.
4. First-run flow: no admin has joined fabric B → open commissioning
   window on thread-c6 for N minutes; afterwards `matter_share`-style op
   from the app to re-open for a second admin.

The RAM measurement above lives in thread-c6 as the `heap` console
command — kept around so the budget can be checked against the next
build, not just this one.

Non-goals / known limits:

- The hub's fabric is not visible to Apple Home even then — fabrics are
  intentionally isolated. Home sees the hub and its bridged endpoints,
  not the nodes behind them.
- OTA of bridged devices stays invisible to Home (§ 9 above).
- `matter_share` from the previous task is still useful: an administrator
  who wants a Matter device directly in Home (not through the hub) can
  skip the bridge and multi-admin it. The bridge is the default path, not
  the only one.

Status: **planned, not started**. The RAM measurement is the first move;
it decides between F1 and the harder forks.

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
