# Brain backlog — what the brain still needs to become a real hub

State today (`main`): the brain (S3) runs `EntityRegistry` +
`RadioCoordinator` + `StateMachine` + `CommandDispatcher` + `RuleEngine` +
`ChipMonitor` + `Reconciler`/`DesiredState` on top of a `ChipRouter` that binds
UART1↔Zigbee C6 (chip 0x01) and UART2↔Thread C6 (chip 0x02), and adds Wi-Fi + SNTP,
timers, recurring schedules, scenes, entity names, and the app-protocol server (wss + token,
mDNS, [app-protocol.md](app-protocol.md)). Interactive console over USB-Serial-JTAG: `home`,
`get`, `on|off|toggle`, `in`, `at`, `timers`, `schedules`, `unschedule`, `cancel`, `scene`,
`rule`, `name`, `desired`, `chips`, `log`, `pair|unpair|identify`, `time`, `tz`, `wifi_*`,
`webtoken`, `webplain`, `heap`, `ota`. Brain cross-forwards DECLARE+STATE+COMMAND between
chips (Zigbee↔Matter), persists rules, schedules, timers, scenes, names and desired state in
NVS, and can OTA both C6s ([ota.md](ota.md)). Host test count: run `ctest --test-dir build`.
Resources on the live brain (`heap` console command, Wi-Fi up, both chips ready, app server
off): ~215 KB free internal heap (lowest since boot 212 KB, largest block 172 KB; ~153 KB with
three TLS clients); app 1.24 MB of a 1.9 MB OTA slot; 2 MB PSRAM present but not enabled.

That's a working single-user, single-radio-family dev hub. To be a real home
hub, the brain needs the following, ordered roughly by how much home wiring
they unlock.

## 1. Persistent, richer rule engine

Today: user-editable rules stored in NVS, each a single if-then (one trigger,
one predicate, one action). To make rules richer:

- ~~**Rule storage**~~ ✅ done: rules are data (`CmpOp` + operand), stored in NVS
  (`rules` blob), edited via the `rule` console command (`rule`, `rule add
  <entity> <attr> <op> <value> then <entity> <attr> <value>`, `rule del <n>`).
- ~~**Time window + extra condition + cool-down**~~ ✅ done (rules v2 in NVS, v1
  still loads): `rule add … then … [between HH:MM-HH:MM] [and <entity> <attr> <op>
  <value>] [cooldown <sec>]`. A constraint that needs the clock or a state that is
  not known yet (clock not synced, never reported) keeps the rule quiet. Still
  open: OR, more than one extra condition, several actions per rule.
- ~~**Recurring time-of-day actions**~~ ✅ done (`core::ScheduleTable`, NVS key
  `schedules`): `at 22:00 off plug1 [daily|weekdays|weekends|mon,tue,..]`,
  `schedules`, `unschedule <n>`; local time (see `tz`), once per day, caught up
  within 10 min of its time, last-fired day persisted. Open: conditions that
  combine a time window with a state (`after sunset && motion → light`), sunrise /
  sunset (needs a location), one-shot absolute times.
- ~~**Scenes**~~ ✅ done (`core::SceneTable`, NVS key `scenes`, up to 8 scenes of 8 actions):
  console `scene`, `scene add <name> <id> <attr> <value> …`, `scene <name>`, `scene del <name>`;
  app ops `scene.put/del/apply` (protocol doc §5). Applied as ordinary commands, so
  desired state sees them. Schedules, timers and rules can apply a scene (`at 22:00 scene night`,
  `in 60 scene night`, `rule add … then scene night`; the reference is the scene's stable id and a
  scene in use cannot be deleted). Still open: Zigbee Groups & Scenes (one radio frame for the
  whole group), editing a scene in place.
- Consider a small DSL (Berry/Lua) later, but not before we hit a wall with
  the in-code table (SPEC §7 already flags this as a later option).

## 2. Time

Done (`91de2c2`): SNTP over WiFi (`pool.ntp.org`), console `time` (shows time,
`synced: yes/no`, tz, uptime) and `tz`; checked on the live brain (`synced: yes`,
tz UTC). Timers fall back to a monotonic tick before the first sync. Open: use
the wall clock inside rule conditions (§1), sunrise/sunset (needs a location).

## 3. WiFi

Needed for SNTP, for Matter's mDNS discovery, later for any web UI or push
notifications:

Done (`91de2c2`): STA connect with automatic reconnect; credentials come from
the gitignored `main/wifi_secrets.h` (compile-time). Open:
- Provisioning (SoftAP + captive portal, or BLE, or a config file on SD) so
  credentials are not baked into the image.
- Exponential backoff on reconnect.
- Chip status in `chips`: also show WiFi RSSI, IP.

## 4. Persistent state on the brain

The registry is rebuilt from co-processor DECLAREs on every boot, that's fine.
Rules, desired state, timers and entity names are persisted in NVS.

- ~~**Persistent timers**~~ ✅ done (`core::TimerTable` v2 stores the remaining
  delay, wall-clock deadlines supported; overdue timers older than 10 min are
  dropped on boot). Still open: a per-action `after_deadline` policy (see
  [../backlog.md](backlog.md#recovery-after-a-crash-or-reboot--desired-vs-actual-reconciliation)).
- ~~**Desired-state per attribute** + reconciler~~ ✅ done (`core::DesiredState`
  + `core::Reconciler`, NVS key `desired`, console `desired`).
- ~~**Entity names**~~ ✅ done (`core::EntityNames`, NVS key `ent_names`, console
  `name`, every command accepts `<id_or_name>`). Still open: room tag, favorite
  flag, icon.
- ~~**Persistent rule table**~~ ✅ done (§1).
- One combined NVS namespace `brain_state`, versioned, migrated on schema
  bumps.

## 5. Console v2

The current console is good enough for debug. For daily use it wants:

- ~~**Named entities**~~ ✅ done (see §4).
- **Tab completion** for entity names and rule ids.
- **`watch` command** — subscribe to STATE changes for one entity and print
  them until Ctrl-C, without turning on the global log firehose.
- **`log <tag> on|off`** — per-component log toggle (`log core`, `log
  dispatch`), not just the whole brain.
- **Non-destructive history** — the current REPL loses history across
  reboots; save last-N commands to NVS.

## 6. Chip health as first-class

We started this with `chips`. To finish:

- ~~**Heartbeat watchdog**~~ ✅ done (`core::ChipMonitor`): silent after 20 s, shown
  in `chips`, COMMANDs to a dead chip fail at once, silent chips are probed with
  QUERY, reboots and flapping (≥3 in 5 min) are logged.
- ~~**`self` entity per chip**~~ ✅ done, hardware-verified (commit `30f9da2`):
  device_type=chip (0x0A), entity_id = chip_id, attrs 0x70–0x75 (uptime, heap, boot_id, rx/tx frames, CRC errors); `chips` prints
  the telemetry. Still open: Matter diagnostic clusters, radio quality
  (RSSI/LQI). (Detail: [../backlog.md](backlog.md#chip-health-timeouts-and-recovery)).
- **Hardware EN-reset lines** — plan the extra two wires (S3 GPIO → each C6
  EN via a MOSFET) before the soldered rev freezes, so the brain can
  hard-reset a wedged co-processor.

## 7. Recorder / event log (Phase 7 groundwork)

We already know Phase 7 puts a passive listener on the CAM writing STATE
history to SD. Brain-side hooks that make that easy:

- **Structured event emission** — instead of `ESP_LOGI` prose, emit
  typed events (`type=STATE, entity=..., value=..., ts=...`) on an
  internal channel that the recorder subscribes to (and the console can
  optionally render).
- **Gap markers** on reboot — see recovery item in `../backlog.md`.

## 8. Human-facing surface (beyond console)

Once the internals are honest, the outer surface:

- ~~**Small web UI** on the brain~~ replaced by a separate client: the hub serves
  no UI/static files, only the WebSocket+JSON protocol in
  [app-protocol.md](app-protocol.md); the app (Expo, iOS/Android/web) lives in
  `app/`.
- **Notifications** — Telegram / MQTT / Home Assistant webhook on
  interesting events ("front door opened at 03:12").
- **A tiny status LED** driven by the on-board WS2812 (GPIO21) — green
  hub healthy, amber degraded, red offline.

## 9. Multi-user / auth (much later)

The app protocol now has a shared token (constant-time compare, lockout) over TLS with a
pinned certificate. What is still missing is per-user identity and per-source ACLs. Matter's own fabric ACLs cover that half of the
world, so scope this carefully.

## 10. Testing

Host tests cover `libhubproto` and `core` in isolation (including the
reconciler, chip monitor, OTA and persistent rules). What's missing:

- **Multi-bus tests** — `ChipRouter` in `core` (once it earns its move) with
  two `InMemoryLink` legs, routing/fan-out.
- **Rules with a real clock injection**, sunset conditions.
- **Console command tests** — a fake serial in/out that drives commands
  end-to-end.

## Order-of-work suggestion

Done: Wi-Fi + SNTP, persistent timers, recurring schedules, rule constraints, entity names, desired-state + reconciler,
persistent rules + `rule` console command, chip health watchdog, `self` entities, scenes (and schedules/timers/rules
that apply them), the app protocol + client ([app-protocol.md](app-protocol.md)): wss + token + pinning, mDNS,
management requests, pair/unpair/identify.

1. **Structured event stream** — precondition for the recorder (Phase 7).
2. **Wi-Fi provisioning** — credentials are still baked into the image (§3).
3. **Sunrise/sunset** for schedules and rule windows (needs a location).
4. **Phone build** of the app's native module (not verified on a device) — see app-protocol §10.

Related backlogs: [Zigbee](zigbee-backlog.md), [Matter](matter-backlog.md).
Cross-cutting design questions: [../backlog.md](backlog.md).
