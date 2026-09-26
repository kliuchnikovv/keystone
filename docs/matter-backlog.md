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
