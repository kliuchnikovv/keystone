# Brain backlog — what the brain still needs to become a real hub

State today (`main`): the brain (S3) runs `EntityRegistry` +
`RadioCoordinator` + `StateMachine` + `CommandDispatcher` + `RuleEngine` +
`ChipMonitor` + `Reconciler`/`DesiredState` on top of a `ChipRouter` that binds
UART1↔Zigbee C6 (chip 0x01) and UART2↔Thread C6 (chip 0x02). Interactive
console over USB-Serial-JTAG: `home`, `get`, `on|off|toggle`, `in`, `timers`,
`cancel`, `chips`, `log`, `rule`, `desired`, `ota`. Self-heal QUERY +
heartbeats from both C6s make the hub come up on its own. Brain cross-forwards
DECLARE+STATE+COMMAND between chips (Zigbee↔Matter), persists rules and desired
state in NVS, and can OTA both C6s ([ota.md](ota.md)). Host test count: run
`ctest --test-dir build`.

That's a working single-user, single-radio-family dev hub. To be a real home
hub, the brain needs the following, ordered roughly by how much home wiring
they unlock.

## 1. Persistent, richer rule engine

Today: user-editable rules stored in NVS, each a single if-then (one trigger,
one predicate, one action). To make rules richer:

- ~~**Rule storage**~~ ✅ done: rules are data (`CmpOp` + operand), stored in NVS
  (`rules` blob), edited via the `rule` console command (`rule`, `rule add
  <entity> <attr> <op> <value> then <entity> <attr> <value>`, `rule del <n>`).
- **Multi-condition / multi-action** — today one trigger, one predicate,
  one action. Real homes want AND/OR, time windows, cool-downs.
- **Time-of-day / sun** — `after sunset && motion → light`, `at 22:00 → all
  off`. Needs wall-clock time (see next item).
- **Scenes** — a named group of desired attribute values applied at once
  (`scene evening`). Doubly useful with Groups & Scenes on the Zigbee side.
- Consider a small DSL (Berry/Lua) later, but not before we hit a wall with
  the in-code table (SPEC §7 already flags this as a later option).

## 2. Time

Everything above assumes the brain knows what time it is. Today it doesn't
know anything past `esp_timer_get_time()` (uptime).

- **SNTP** over WiFi (once WiFi is up for Matter/OTA).
- Sync `esp_timer` wall clock, cache `time_t`.
- Console: `time` shows current time; `set tz Europe/Prague` sets timezone.
- Fall back to a monotonic tick if no NTP yet — rules that don't need
  calendar time should still work.

## 3. WiFi

Needed for SNTP, for Matter's mDNS discovery, later for any web UI or push
notifications:

- Provisioning (SoftAP + captive portal, or BLE, or a config file on SD).
- Reconnect on drop, exponential backoff.
- Chip status in `chips`: also show WiFi RSSI, IP.
- Not needed for the Zigbee-only slice — deliberately kept out until now.

## 4. Persistent state on the brain

The registry is rebuilt from co-processor DECLAREs on every boot, that's fine.
Rules and desired state are already persisted in NVS; timers and entity names
are still in RAM.

- **Persistent timers** — `in 3600 off 0x100` survives a brain reboot with
  absolute wall-clock deadline (see [../backlog.md](backlog.md#recovery-after-a-crash-or-reboot--desired-vs-actual-reconciliation)).
- ~~**Desired-state per attribute** + reconciler~~ ✅ done (`core::DesiredState`
  + `core::Reconciler`, NVS key `desired`, console `desired`).
- **Entity metadata**: user-set name (`livingroom_plug`), room tag, favorite
  flag, icon. Reference by name in the console: `on livingroom_plug`.
- ~~**Persistent rule table**~~ ✅ done (§1).
- One combined NVS namespace `brain_state`, versioned, migrated on schema
  bumps.

## 5. Console v2

The current console is good enough for debug. For daily use it wants:

- **Named entities**: `on livingroom_plug` in addition to `on 0x100`.
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

- **Small web UI** on the brain (WiFi + `esp_http_server`) — list entities,
  toggle, edit rules, view recent events. Read-only first.
- **Notifications** — Telegram / MQTT / Home Assistant webhook on
  interesting events ("front door opened at 03:12").
- **A tiny status LED** driven by the on-board WS2812 (GPIO21) — green
  hub healthy, amber degraded, red offline.

## 9. Multi-user / auth (much later)

Only relevant once there's a web UI or MQTT — then we need at least a shared
secret and per-source ACLs. Matter's own fabric ACLs cover that half of the
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

Done: desired-state + reconciler, persistent rules + `rule` console command,
chip health watchdog; `self` entities written (await hardware check).

1. **Persistent timers** (small, closes an obvious hole).
2. **Named entities + storage** — `on livingroom_plug` is a huge UX win for
   a couple hundred lines.
3. **Wall time (SNTP)** — unlocks calendar rules and event timestamps.
4. **Structured event stream** — precondition for the recorder (Phase 7).
8. **WiFi + web UI** — external surface.

Related backlogs: [Zigbee](zigbee-backlog.md), [Matter](matter-backlog.md).
Cross-cutting design questions: [../backlog.md](backlog.md).
