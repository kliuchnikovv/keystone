# Matter backlog — what a full Matter bridge needs

State today (branch `phase6-zigbee`, commit `f4b23cf`): the second C6 exists,
is soldered in, runs a bring-up scaffold (chip 0x02, HELLO + heartbeat only,
no Thread, no Matter). The brain talks to it over UART2 through the
ChipRouter. That's the whole Matter-side story so far — roughly 1% of what
we need.

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

1. OpenThread up (border router forms a Thread network, visible on mDNS).
2. esp-matter integrated, one Matter On/Off plug endpoint on C6#2, static
   value — Apple Home commissions it, toggles it, but toggle doesn't do
   anything real yet.
3. Bridge down-path: Matter On/Off write → COMMAND over UART → real IKEA
   plug switches → CmdAck.
4. Bridge up-path: STATE from brain → Matter attribute update → Apple Home
   reflects the new state.
5. Multi-fabric + polish.
6. Additional Matter device types matching the Zigbee ones as they come
   online.

Realistic: **3–5 focused sessions** from zero to "Apple Home turns on and off
our real IKEA plug via Thread". First one is the biggest step; each next
one is smaller.

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
