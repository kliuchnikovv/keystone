# Backlog — open questions and future work

Ideas we haven't shaped into a phase yet. Not TODOs on a phase — those live in
[CLAUDE.md § Status](../CLAUDE.md). This file is looser: things worth deciding
before they become urgent.

Topic-specific backlogs live next to this one:
- [zigbee-backlog.md](zigbee-backlog.md) — what a full Zigbee coordinator needs (device types, metering, network mgmt, OTA).
- [matter-backlog.md](matter-backlog.md) — what a full Matter bridge needs (OpenThread, esp-matter, commissioning, brain↔Matter bridge).
- [app-protocol.md](app-protocol.md) — the WebSocket+JSON protocol between the hub and the phone/web app (draft).
- [brain-backlog.md](brain-backlog.md) — what the brain still needs to become a real hub (persistent rules, wall time, WiFi, chip health, human-facing surface).

Cross-cutting design questions are collected here.

## Use idle memory on the radio co-processors

Each C6 has ~512 KB SRAM + 4 MB flash. Zigbee/Thread stacks + our translator
use only a fraction (thread-c6 Stage A: 217 KB image, 83% partition free).
The remaining RAM/flash on the two C6s is idle and could carry work that
today sits on the brain — or is missing altogether. Question: **which work
belongs there, without turning the co-processors into second brains?**

Design invariant to preserve (SPEC §3): the *brain* owns entity state and
rules; the co-processors own their radio and a translation layer only. So the
right kind of work to push down is **local-to-that-radio, non-policy** work.
Candidates (roughly in order of value):

- **Local event buffer / send queue.** If the UART link to the brain drops
  (brain reboot, cable), currently STATE/CmdAck are lost. A small ring buffer
  on the C6 (say 128–256 events) would replay on link resume. Rescues short
  outages without any brain change.
- **Device-side attribute cache.** Cache last-known values per Zigbee/Thread
  device so `get` doesn't need a fresh ZCL read every time; the brain already
  has its own cache, but this lets the co-processor answer local diagnostics
  and short-circuit its own periodic reads.
- **Debounce / de-duplication of noisy sensor reports.** Occupancy, contact
  and metering clusters can spam; the C6 can coalesce reports before they
  reach the wire (e.g. no STATE if value unchanged, or rate-limited).
- **Local scaling / unit conversion.** Electrical Measurement returns raw
  int16 × ACPowerDivisor/Multiplier. Reading those divisors is one-shot per
  device; the C6 can pre-scale into the SPEC §6.1 unit before sending STATE,
  so the brain's registry gets clean values.
- **Persistent device metadata cache.** Manufacturer/model, cluster list,
  attribute reporting config — cache in NVS so re-discovery after reboot is
  faster and (partly) works offline from the Zigbee network.
- **Time-series for metering.** Keep the last N power/energy samples in a
  compact circular buffer, so a `power 0x100 last 5m` query gets answered
  without spamming the plug at request time.
- **Compression on the UART wire.** hubproto is COBS+CRC; adding lightweight
  delta encoding for STATE bursts (very small on our scale) could help under
  storms, but only worth doing if we see them.

Non-candidates (belong on the brain, not the co-processor):
- Cross-device rules ("motion in hall → light in kitchen") — brain is the
  single place these converge (SPEC §7 RuleEngine).
- Matter fabric state — Matter itself lives on chip 0x02 (Thread), but the
  bridge model that ties Matter accessories to registry entities is brain.
- Any policy / user-facing configuration.

Decision needed later: pick 1–2 of the above once a concrete pain point shows
up (link outage log, sensor spam, metering scaling), not now on speculation.

## Recovery after a crash or reboot — desired vs. actual reconciliation

Today the brain is purely **reactive**: it stores the last-reported value of
each attribute and runs rules on state changes. There is no notion of
*desired* state. So when the brain (or a co-processor, or the whole hub) is
down and comes back, we don't know what *should* have happened during the gap
and can't correct for it. Examples:

- A `in 3600 off 0x100` timer was set at 21:00, brain rebooted at 21:30 —
  the timer is lost; the plug stays on forever.
- A rule "at sunset, close the covers" fires at 20:47; brain was down; at
  boot we don't know the covers should be closed now.
- The plug was physically toggled by the button while the brain was down; on
  return the brain thinks it's still off.

Question: **give each entity a desired-state alongside actual-state, and
reconcile at boot / on link recovery?**

Shape of the answer (rough):

- **Desired state per attribute**, stored in the registry beside actual. Set
  by rules or by explicit commands. Persisted (NVS on brain; or on the
  co-processor for its own entities). Missing = "don't care", so we don't
  fight physical toggles unless something set a desired.
- **Reconciler loop** on the brain: for each entity/attribute with a desired,
  if actual != desired (and actual is known), send a corrective COMMAND. This
  is the CommandDispatcher's retry logic generalized — today it retries a
  single send; a reconciler retries the *goal*.
- **Boot audit**: on start, read actual for every entity (already partly done
  in the C6 restore path via read_device_state), compare to persisted desired,
  emit corrective COMMANDs. Log a `reconcile` event so it's visible what was
  fixed.
- **Persistent scheduled actions**: today `in <sec> …` timers live in RAM;
  move them to NVS with an absolute wall-clock deadline. On boot, drop the
  expired ones by policy (some make sense to still apply — "turn off at 22:00"
  fired at 22:03 is still fine; "briefly flash the light" is stale). A per-
  action `after_deadline: apply | drop | ask` decides.
- **Gap markers** on actual state: after a reboot / long link outage, mark
  attributes as *stale until confirmed*. Rules that care about freshness
  (edge-triggered on a toggle) can then skip; rules that just want current
  value wait for the next read/report.
- **What we *cannot* recover**: transient events with no lasting state (a
  button press, a motion pulse that already ended). Those we drop honestly
  and note the gap in the recorder log (Phase 7) rather than fake them.

The invariant to keep: desired state is authoritative *only if something set
it*. We never fabricate a desired from an actual — otherwise a physical human
toggle would be silently reverted, which is the classic worst behavior of
smart-home hubs.

> **Status:** desired state + reconciler are implemented (see CLAUDE.md § Status,
> "Desired-state reconciliation"). Still open from this section: per-action
> `after_deadline` policy for timers and gap/stale markers.

Depends on: persistent timer table (small); a `desired` field on AttrState
(small); a reconciler tick in the brain loop (small); Phase 7 recorder for the
gap-annotation part (later).

## Sharing load across the three chips

Today the split is by *radio*: brain (S3) does registry / rules / dispatch;
C6 #1 does Zigbee; C6 #2 does Thread/Matter. Both C6s sit ~mostly idle when
their radio isn't peaking. Question: **which brain-side work is worth pushing
to a specific co-processor, and by what rule?**

Framework to decide (before individual moves):

- **Move by locality, not by load.** Push down work whose inputs and outputs
  are on that co-processor's radio: e.g. an "if occupancy → light" rule where
  both devices are Zigbee could run on C6 #1, cutting a UART round-trip.
  Don't push down cross-radio rules — those belong on the brain by design
  (single meeting point, SPEC §7).
- **Move stateless, keep stateful.** Debounce, unit conversion, dedup — good
  candidates. Multi-step schedules and user-visible rule state — brain (the
  console lives there anyway, one place to debug).
- **Don't shard the entity registry.** The registry is the shared model that
  Matter, the console, and rules all read. Splitting it across chips buys
  little and adds a distributed-systems problem we don't have yet.

Concrete candidates (open, not ranked):

- **Local rule fast-path**: a rule tagged as *pure Zigbee* (all triggers +
  actions on chip 0x01) executes on C6 #1 in ~1 ms instead of round-tripping.
  Brain keeps the source of truth and ships the compiled rule down at startup.
- **Chip-local diagnostics**: each C6 answers `chip 0x01 stats` locally
  (heap free, uptime, radio counters, RSSI to root device) so the brain
  doesn't have to keep a mirror.
- **Radio-side rate limiting**: C6 caps how many COMMANDs/sec it lets through
  to a given device to protect it (some IKEA bulbs mis-handle bursts). No
  brain change needed.
- **Distributed timers for local-only actions**: `in 300 off 0x100` where
  entity is on chip 0x01 — could live on that C6 in NVS. Survives a brain
  reboot ([[recovery]] item above). Downside: harder to introspect.
- **Between the two C6s**: today they only talk through the brain. If we ever
  want a Zigbee→Thread bridge (unlikely) or shared time — decide separately.

Rule of thumb we'd want to formalize: **the brain must be able to reason
about the whole home even if a co-processor is silent** — so anything moved
down must be *cache-visible* or *reproducible*, not authoritative.

## Chip health, timeouts, and recovery

We already have the ingredients — HELLO (`chip_id + boot_id`), the
RadioCoordinator's boot_id resync, and MsgType::Heartbeat (0x40) that
thread-c6 already emits every 5s — but the brain doesn't currently reason
about them. Question: **make every chip's liveness a first-class thing the
brain observes and reacts to.**

What "health" should include:

- **Reachable / silent**: last heartbeat older than N × interval → mark
  silent, stop routing COMMANDs there, surface in `chips` console.
- **Reboot count** via boot_id transitions: how often does this chip reset?
  Alert on flapping.
- **Radio counters**: TX/RX frame count, CRC errors, dropped frames on the
  UART link + on the radio side (parents/children lost, join failures).
- **Resource use**: free heap, largest free block, uptime, main-loop lag if
  we can measure it.
- **Radio quality**: RSSI/LQI for each end-device (Zigbee) or neighbor
  (Thread). Lets us spot a device that's about to drop out.

How to wire it (rough plan):

- **A `self` entity per chip** (entity_id 0x00XX reserved), device_type = a
  new `chip` type in SPEC §6.1, with attributes for the above. Same
  DECLARE/STATE plumbing — no new wire message type. Brain gets health as
  regular STATE updates; console `chip 0x01` just does `get 0x00XX`.
- **Brain heartbeat watchdog**: subscribe to Heartbeat in RadioCoordinator;
  if none seen for T (say 20s = 4 missed heartbeats at 5s), mark chip
  Unreachable, fail in-flight COMMANDs fast (skip retries into the void),
  and log a `chip_lost` event.
- **Recovery policy** when a chip comes back:
  - boot_id changed → we already clear+re-QUERY, nothing new to add.
  - Same boot_id after silence → treat as *link resumed*, not restart:
    replay whatever was in the co-processor's outbound buffer (see the
    "local event buffer" idea in the memory section above), no DECLARE
    churn.
- **Automated hardware recovery**: we can't reset a C6 remotely today (no
  reset line from S3). Options for later: (a) drive the C6 EN pin from a
  spare S3 GPIO via a MOSFET, so the brain can hard-reset a wedged
  co-processor; (b) rely on the C6's task-watchdog to self-reboot on its
  own hangs. (a) is small extra wiring and worth planning before the
  soldered rev freezes.
- **User-facing surface**: `chips` command in the console listing each chip
  with color/status; the future Matter bridge can also expose "hub health"
  as a diagnostic cluster.

> **Status:** the heartbeat watchdog (`core::ChipMonitor`) is done and
> hardware-verified; the `self` entity is done and hardware-verified
> (device_type `chip` 0x0A, entity_id = chip_id, attrs 0x70–0x75, SPEC §6.1).
> Still open: radio quality (RSSI/LQI), the EN/BOOT reset lines, replay of a
> co-processor's outbound buffer after a link resume.

Invariant: chip health belongs to the *brain*. A chip reporting "I'm fine"
tells the brain nothing useful about the *link* — the brain must judge
liveness from its own inbound stream.

## Investigate the C6 random restarts

The two C6s reboot far more than they should: the last `chips` snapshot (hub
uptime ~46 min) showed **203 reboots seen for zigbee-c6 and 169 for thread-c6**.
Something — a mix of things — restarts them every few minutes. Known causes so
far: the Pi5's 600 mA per-USB-port current cap trips VBUS when both chips burst
at once (see the `pi5-usb-power-cap` memory, fix = `usb_max_current_enable=1` +
reboot), and the brownout detector tripped on radio bursts at level 1
(~2.58 V), now disabled in both C6 images (`CONFIG_ESP_BROWNOUT_DET` not set).
After both of those the count is still high, so **there is a second class of
restart we have not pinned down yet**. Question: what is actually resetting
the chips, and how much of it is one root cause vs. several?

What to collect, before changing anything:

- Classify every reboot by `esp_reset_reason()` (we already print it at boot;
  turn it into a persistent counter per reason in NVS so a session that wipes
  the UART log does not lose it). The histogram per chip is what tells us
  whether we see a panic storm, a brownout storm, a watchdog storm or USB
  cycling.
- Correlate with the Pi's dmesg (`USB disconnect, device number N`): a pair
  of disconnects within 1–2 s on different buses is power, a lone one is
  software (chip reset → USB re-enumerated without the Pi touching VBUS).
- Capture the UART tail across a reset: when it is a panic/abort the chip
  prints backtrace before the restart; when the port disappears with nothing,
  it is power or hard reset. The `serial-tap.py` we already have teaches both
  to the same log and is what to run during a soak.
- Note what else the chip was doing at the moment: Zigbee TX burst, Thread
  routing burst, OTA flash write, NVS commit. The `zigbee_reset_reason_name`
  helper already prints it; add a one-line "last activity before reset" to
  the chip's self-entity telemetry (`attr 0x78`?).

Only then it is worth a fix — the fix will be different for brownout, panic,
and silent power loss, and we do not know the mix yet.

Status: **open, no plan yet** — the data is the deliverable, not a patch.

## Auto-recovery when a chip restarts

Related but separate: the user reports "часто один из чипов не виден" — after
a C6 reboot the hub sometimes does not notice the chip came back, or the UART
link stays wedged until the brain is also reset. RadioCoordinator already
does the right thing when a frame arrives with a new boot_id (clear + re-QUERY),
and ChipMonitor marks a chip SILENT after 20 s of no frames. **What is missing
is the active step that turns SILENT back into READY by itself.**

What we have today:

- `ChipMonitor` marks SILENT, drops in-flight commands as Unreachable, probes
  the chip with a QUERY every 10 s.
- `RadioCoordinator` resyncs on any frame with an unknown `boot_id`.
- No RX recovery on the brain's UART: if the C6 reboot left garbage in the
  line, `FrameReader` resyncs on the next 0x00 — but a line that is stuck
  high (floating RX during the other side's reset) emits no bytes to resync
  from. We already pulled the RX pin up in `EspUartLink::begin()`, which cut
  the phantom CRC errors; whether a wedged link is the same bug or another
  one is unproven.

What auto-recovery should cover:

- **Link wedged, chip alive**: the brain sees heartbeats stop but nothing
  coming in at all. Today the QUERY from `ChipMonitor` cannot wake it because
  our side's RX ignores the (possibly valid) answer. Likely fix: on
  `kSilent → still silent after one probe`, re-init our own UART driver
  (`uart_driver_delete` + `uart_driver_install`) and resend QUERY once. One
  shot, logged — never in a loop.
- **Chip restarting fast (flapping)**: the monitor already logs FLAPPING on
  ≥3 reboots in 5 min. Add a hub-side action: back off commands to that chip
  for 30 s, surface `chip_flapping` as a history event with the reason
  reported by the C6 (see the investigation above), and keep the LED amber.
- **Chip gone for minutes**: once the pattern above is known, decide whether
  the brain should give up and rely on the C6's own task-watchdog to reboot
  it (acceptable today) or whether the soldered-rev reset line (EN pin from
  a spare S3 GPIO, raised in the health section) is the real answer. **This
  is the "automated hardware recovery" the health section mentioned —
  turning it from "options for later" into a decision.**
- **Co-processor outbound buffer**: when the link resumes and the chip did
  not reboot, replay what we missed instead of relying on a fresh DECLARE
  (same note the health section has, cross-reference, not duplicate).

Status: **open**. Blocker: we do not yet know which failure mode is the
common one — the investigation above runs first and chooses the fix.

## The third chip the app shows as "?"

Both the console (`chips`) and the app list a third chip with no name —
`chip 0x03 (?): ready, N frames, ...`. It is not a mystery and not a bug in
the wire: 0x03 is the Matter bridge's **pseudo-chip** on the brain itself
(`firmware/brain-s3/main/matter_bridge.hpp::kChipId = 0x03`), the owner of
every Matter node the hub commissioned. The current TIMMERFLOTTE is here
(`0x0300 sensor chip3: battery / temperature / humidity`). The core routes
DECLARE/STATE/COMMAND/CMD_ACK to it exactly like to a UART co-processor,
which is why it appears in `chips` at all.

The "?" is just a missing label. Fix:

- `firmware/brain-s3/main/main.cpp::cmd_chips` prints "zigbee" for 0x01 and
  "thread" for 0x02; teach it "matter" for 0x03 (any other id stays "?").
- The app snapshot carries the same chip list — give 0x03 a display name
  and a Matter-looking glyph (not a radio icon: it has no radio of its own).
- Nothing on the wire needs to change; the chip id is already stable and
  entity ids 0x0300.. already live there.

Status: cosmetic; the plumbing is already right.

## OTA updates for the co-processor C6s

Today the two C6s (Zigbee coordinator, Thread border router) can only be
updated by physically plugging USB into each one, unplugging the S3 first,
and running `idf.py flash`. That's fine for the dev bench, painful for a
soldered box tucked behind furniture — especially with two co-processors.
Question: **how does the brain push new firmware to a C6 without the human
touching hardware?**

Two main shapes, both worth planning before soldering freezes the wiring:

- **Brain-side flasher over UART.** The brain holds the co-processor image
  (delivered to it any way we already know how — Wi-Fi OTA, SD card, USB
  drop over the console) and drives the C6 into its ROM bootloader by
  toggling the C6's `EN` line, then talks the `esptool` protocol over the
  UART link. Requires the hardware `EN` reset lines already sketched in
  the [chip-health](#chip-health-timeouts-and-recovery) item (a spare S3
  GPIO to each C6's EN via a MOSFET) — worth wiring in the same solder
  session even if we implement the flasher later. Also requires putting
  the C6 into `boot` mode (GPIO9 low on ESP32-C6) at reset, so plan two
  wires per C6: `EN` and `BOOT`. Once wired, the code is a small port of
  esptool's stub protocol.
- **In-application OTA on the C6 itself.** Each C6 exposes an OTA
  endpoint over its own UART protocol (a new hubproto message type, or
  reuse COMMAND with a chunk index). The brain streams the image chunk
  by chunk into a second OTA partition, and on completion sends "reboot
  to new". Doesn't need any extra wiring, but needs OTA partitions in
  each C6's `partitions.csv` (double flash footprint) and a rollback
  strategy for a bad image (esp-idf's rollback + verify).

Recovery is the hard part. If a bad image bricks a C6:
- With EN+BOOT lines: the brain still forces it into ROM and reflashes.
  This is the "always works" path — that's why routing those two extra
  GPIOs now is high leverage.
- Without them: physical USB access is the only recovery. Which means the
  hub can't be sealed / installed permanently.

Ties in:
- Brain-side image delivery = pieces already listed in
  [brain-backlog.md § WiFi](brain-backlog.md#3-wifi) (get images via HTTPS)
  and § web-UI (drop an image from a browser).
- Matter has its own OTA too — that only updates a chip that already runs
  Matter, so it doesn't cover the Zigbee C6 or a bricked chip.
- If we ever add the recorder-CAM as a third co-processor (SPEC Phase 7),
  it needs the same treatment — plan `EN` on it too.

Recommendation: **wire `EN`+`BOOT` from the S3 to each C6 in the same
solder session as the UARTs**, even if the software follows later. Software
without wires: no recovery path. Wires without software: still a working
hub, and we can add the flasher any time.

## Mobile app (iOS + Android)

> **Status (2026-10-11): decided and largely built.** The choice was a separate Expo app
> (iOS, Android, web) talking to the brain over a WebSocket+JSON protocol with a token and
> a pinned self-signed certificate — not a PWA served from the brain. See
> [app-protocol.md](app-protocol.md) and [../app/README.md](../app/README.md). Done: local API
> with auth, mDNS discovery, entity list/toggle/live values, rule/schedule/timer/scene editor,
> hub health, pairing, event history. Not done: remote access, push notifications, Wi-Fi
> provisioning, and the phone build of the native pinning module (never run on a device). The
> text below is the original option analysis, kept for the parts still open.

The user-facing surface for the home. Console-over-USB is fine for a dev
hub; a person on the couch wants a phone. Question: **build our own app, or
lean on someone else's, or both?**

Three shapes, each with a different cost/benefit:

- **Lean on Apple Home / Google Home / Alexa via Matter** — no app of our
  own, no accounts, no push infra. We get free iOS + Android + widgets +
  Siri/Assistant/Alexa voice + geofences + shortcuts. The catch: only the
  entities exposed as Matter accessories are visible, and only what Matter's
  clusters model — energy graphs, our own rule engine, hub diagnostics all
  stay hidden. This is why [matter-backlog.md](matter-backlog.md) matters
  even if we later add our own app.
- **A thin native app talking to the brain over WiFi/HTTPS** — one code
  base per platform (SwiftUI on iOS, Kotlin/Compose on Android; or one
  cross-platform in Flutter / React Native / Expo). Screens: entity list,
  toggle, live power reading, rule editor, event history, hub health,
  provisioning wizard. Talks to a small HTTP/WebSocket server on the brain
  (see [brain-backlog.md § web UI](brain-backlog.md#8-human-facing-surface-beyond-console)).
  Requires: LAN-only first, then some form of remote access.
- **A PWA served from the brain** — the same web UI mounted as an
  installable app. Cheapest to build (single code path), fewest features
  (no true background, limited iOS push). Great stopgap while the native
  app matures.

Cross-cutting bits we'll need whichever we build:

- **Local API on the brain** — REST/JSON + WebSocket for live updates.
  Auth: at first a shared token typed once during onboarding, later per-user
  with Matter-fabric-derived identity so the app inherits the same trust
  model. Depends on WiFi + web UI groundwork already in
  brain-backlog.md.
- **Remote access** — LAN works from home; away needs either mDNS + VPN,
  a relay we host, or reuse of Matter's remote (Apple Home / Google Home
  handle this if the accessory is in their fabric). The cheapest option is
  "away = Apple Home"; the honest one is a relay we run.
- **Push notifications** — APNs for iOS, FCM for Android. Requires an
  outbound path from the brain (webhook to a small relay, or the brain
  itself if it has internet), plus platform provisioning (APNs cert, FCM
  project). The relay is the simplest architecture.
- **Provisioning UX** — the app's first job: find the hub on the LAN
  (mDNS), pair with it (QR on the hub → app scans), commission WiFi if not
  already done, set a friendly hub name.
- **Feature parity with the console** — every `home`/`get`/`on`/`in`/
  `chips`/`log` action should be reachable from the app, but *presented
  differently* (a list of tiles, not a REPL).
- **Offline behavior** — the app must gracefully degrade when the hub is
  unreachable (cached last state + explicit "stale" markers), and must not
  silently retry commands the user thought failed.
- **Design surface**: how much branding / how much just "control panel"?
  Decide before writing pixel-heavy UI.

Order-of-work suggestion:

1. Get Matter working (`matter-backlog.md`) so Apple/Google Home covers
   the 80% case for free.
2. Small **PWA served from the brain** as the desktop UI (a couple hundred
   lines of HTML/JS), reusing the local API. Covers the LAN case, tests
   the API design.
3. When (2) hits its ceiling, native app: Swift + Kotlin or a single
   Flutter/Expo codebase — evaluate then, not now.
4. Push + remote-access relay only when someone actually asks for them.


## Firmware update from the phone app

Today `ota <chip> <url> <sha256>` needs a console and an HTTP server for the
image. Once there is a phone/web app (see above), it could upload the image
itself: the brain exposes `POST /ota/<chip>` (Content-Length + SHA-256 header),
streams the body straight into `OtaSender`, and reports progress via
`GET /ota/status`. Open points: authentication (token at minimum) and image
signing — images are unsigned today, so an open endpoint would let anyone on
the LAN reflash the hub. Decide together with the app; see [ota.md](ota.md).
