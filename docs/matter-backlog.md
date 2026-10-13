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
