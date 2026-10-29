# App protocol — how a phone/web client talks to the hub

Status: **v1, implemented on the hub, in the Node tools and in the web app; verified on the real
brain** (handshake, snapshot, live events, commands, and the management requests for names,
rules, schedules, timers, scenes and pair/unpair/identify; TLS with certificate pinning;
mDNS). **Not verified on a phone:** the native module that pins the certificate and browses
mDNS on iOS/Android (`app/modules/keystone-net`) is written but nothing here can build it.
An actual `unpair` has not been run on a device either (§5). The hub serves no UI and no
static files; it only speaks this protocol. The client is a React Native (Expo) app that
also builds for the web, in [`app/`](../app/).

Decisions (maintainer, 2026-10-10): WebSocket + JSON; Expo (iOS, Android, web);
shared token **and** TLS; code in `app/` of this repo.

This is a *client* protocol. It is unrelated to hubproto (COBS+CRC over UART
between the chips) and does not change it; the hub translates.

## 1. Transport

- `wss://<hub>:8443/ws` (TLS, §10) when the firmware has a TLS identity. `ws://<hub>:8080/ws`
  is a development fallback: the only transport in a firmware without an identity, and
  otherwise enabled by the console command `webplain on`.
- One WebSocket, text frames, one JSON object per frame, UTF-8, max 4096 bytes.
  The hub closes a connection that sends more.
- WebSocket ping/pong keeps it alive (hub pings every 20 s; no pong in 10 s = drop).
- At most **3 simultaneous clients** (heap, see "Resource budget"). A 4th gets
  `error: busy` and is closed.
- Discovery: the hub answers mDNS as `keystone.local` and advertises the service
  `Keystone hub._keystone._tcp.local` on the port it really listens on (8443 for wss,
  8080 when `webplain on`), TXT `v=1`, `name=<hub name>`, `tls=1|0`. The record follows the
  server: no token (server off) means no advertisement, and a mode change re-registers it.
  mDNS is unauthenticated — anyone on the LAN can answer — so it says only *where* to
  look; the trust is the pinned fingerprint (§10), which is deliberately not in the TXT.
  A client can always connect to a typed-in address instead. `npm run discover` (Node) finds
  hubs; a browser cannot do mDNS at all, and on a phone it needs a native module (dev build,
  not tried), so there the address is typed.

## 2. Envelope

Every frame is an object with a `type`. Client→hub requests carry an integer
`id`; the hub echoes it in the reply. Unknown fields are ignored; an unknown
`type` gets `error` (`code: "unsupported"`), so old and new sides can coexist.

```jsonc
{ "type": "...", "id": 7, ... }
```

Values are JSON-native: `bool`, number (`u8/u16/u32/i16/enum8` as integers,
`f32` as a number), `xy_color` as `[x, y]`. The attribute's `value_type` is
given once in the snapshot, so the client knows how to render and edit it.

## 3. Handshake and auth

1. Client opens the socket and must send `hello` within 5 s:
   ```json
   { "type": "hello", "v": 1, "token": "…", "client": "keystone-app/0.1" }
   ```
2. Hub answers `welcome` and then a full `snapshot`:
   ```json
   { "type": "welcome", "v": 1,
     "hub": { "name": "home", "fw": "7843023", "time": 1781000000, "tz": "UTC", "synced": true,
              "wifi": { "ssid": "home", "rssi": -61, "ip": "192.168.0.202" },  // null while not connected
              "location": { "lat": 50.08, "lon": 14.43 },        // null until set
              "sun": { "rise": "06:12", "set": "20:01" } } }       // today, local; null if unknown
   ```
   or `{ "type": "error", "code": "unauthorized" }` and closes. A different major
   `v` gets `error: "version"`.
3. The token is **never** in the URL (URLs end up in logs). It is compared in
   constant time. After 5 failed attempts from one IP the hub refuses that IP
   for 60 s. The token is set on the hub console (`webtoken <value>`, stored in
   NVS) — there is no way to read it over this protocol, and the only way to set it
   is the first-time setup (§11).

No `hello` ⇒ no other message is processed.

## 4. Snapshot and events (hub → client)

`snapshot` is the whole picture right after `welcome`. A frame is at most 4096
bytes, so a big home is sent in **parts**: `part` counts from 0, **part 0 resets
the client's lists**, every part appends the items it carries, and the part with
`"last": true` completes the snapshot (a small home is one frame with `part: 0`,
`last: true`). Sections a part does not carry are simply absent. The hub also
re-sends a snapshot (part 0 again) if it ever had to drop events for a client, so
a client must treat part 0 as "start over" at any time:

```jsonc
{ "type": "snapshot", "part": 0, "last": true,
  "entities": [
    { "id": 256, "name": "plug1", "chip": 1, "device": "switch",
      "attrs": [
        { "id": 1,  "name": "on_off",    "vt": "bool", "flags": ["r","w","rep"], "value": true },
        { "id": 19, "name": "power_w",   "vt": "u16",  "flags": ["r","rep"],     "value": 3 },
        { "id": 20, "name": "energy_wh", "vt": "u32",  "flags": ["r","rep"],     "value": null } ] } ],
  "chips":    [ { "id": 1, "label": "zigbee", "status": "ready", "boot_id": 129, "reboots": 0,
                  "telemetry": { "heap": 377260, "uptime_s": 4480, "tx_frames": 1880, "crc_errors": 0 } } ],
  "rules":     [ … see §6 … ],
  "schedules": [ … ],
  "timers":    [ … ],
  "scenes":    [ { "name": "evening",
                   "actions": [ { "entity": 256, "attr": 1, "value": false },
                                { "entity": 257, "attr": 2, "value": 100 } ] } ] }
```

`value: null` = never reported. Chip self-entities (`device: "chip"`) are **not**
in `entities`; their data is in `chips[].telemetry`.

Incremental events (no `id`, pushed as they happen):

| type | payload |
|---|---|
| `state` | `entity`, `attr`, `value` — an attribute changed |
| `entity` | `op: "add"\|"remove"`; `add` carries the whole entity object and is an **upsert** (new entity, re-declared entity, or a rename); `remove` carries `id` |
| `chip` | `id`, `status` (`ready`/`silent`/`seen`), `boot_id`, `reboots`, `telemetry` |
| `rules` / `schedules` / `timers` | the new full list, after any change (also by another client or the console) |
| `time` | `time`, `tz`, `synced` — on sync or tz change |
| `hub` | `hub`: the whole hub object as in `welcome` — when the location changes or the clock becomes known (today's `sun`) |

**What the real hub does today.** Live attribute changes are pushed as `state`
events and chip health as `chip` events (on change, and every 10 s for telemetry).
Any *structural* change — an entity appearing or disappearing, a rename, a rule,
schedule or timer added/removed/fired — is delivered by **re-sending the snapshot**
(part 0 again) instead of the finer-grained `entity`/`rules`/`schedules`/`timers`
events above, because a list can exceed one frame. Clients must therefore handle
the snapshot at any time (the reducer does); the fine-grained events remain valid
protocol for a hub that wants to use them. Checked at most once a second.

**Dead connections.** The hub sends something at least every 10 s while a client is connected
(chip telemetry), so a client treats 35 s of silence (`staleAfterMs`, 0 = off) as a dead link and
reconnects. This matters when the hub's Wi-Fi blinks or it switches network: the TCP connection can
stay open on the client's side and nothing would ever tell it. (Found by the `--wifi` check below.)

A client that falls behind (its send queue is full) is dropped rather than
buffered without bound; it reconnects and gets a fresh snapshot, so there is no
"missed event" recovery protocol.

## 5. Commands and requests (client → hub)

**Command a device** — async, completes on the chip's CMD_ACK:

```jsonc
{ "type": "cmd", "id": 7, "entity": 256, "attr": 1, "value": false }
// → immediately:      { "type": "accepted", "id": 7, "seq": 12 }
// → on CMD_ACK:       { "type": "ack", "id": 7, "status": "ok" }
```

`status`: `ok` · `fail` · `unreachable` · `unsupported` · `timeout`. A command
to a silent chip is accepted and completes at once with `unreachable` (the
dispatcher fails it fast, no retries). A command that cannot be sent at all
(unknown entity/attr, read-only attribute, wrong value type or range) gets
`{ "type": "error", "id": 7, "code": "…" }` instead of `accepted`.
Commands go through the same `CommandDispatcher` as the console, rules and timers,
so they also update the desired state (reconciler).

**Management requests** — `req` / `res`:

```jsonc
{ "type": "req", "id": 8, "op": "name.set", "args": { "entity": 256, "name": "kitchen_plug" } }
// → { "type": "res", "id": 8, "ok": true }
//   { "type": "res", "id": 8, "ok": false, "code": "bad_args", "message": "…" }
```

| op | args | notes |
|---|---|---|
| `name.set` / `name.clear` | `entity`, `name` | names `[a-z][a-z0-9_]{0,22}` (start with a letter, so never mistaken for a numeric id), unique; chips cannot be named |
| `rule.add` | rule (§6) | `res.result.index`. `then` must be writable (`read_only` otherwise); `when` and `and` may be any attribute, a chip's telemetry included; values are checked against the attribute's type and range |
| `rule.del` | `index` | later rules shift down |
| `schedule.add` | `time` `"HH:MM"` or `"sunrise"`/`"sunset"` with an optional offset `"sunset+30"` (±180 min, see below), `days` (default daily) **or** `date` `"YYYY-MM-DD"` (a one-off, see below), `entity`, `action` — or `scene` instead of the last two | `action`: `on`/`off`/`toggle`; the entity needs a writable on/off. `index` is the slot number, as in the console's `unschedule n` (slots are reused, so indexes can have gaps) |
| `schedule.del` | `index` | |
| `timer.add` | `delay_s` (1..604800), `entity`, `action` — or `scene` instead of the last two | `index` is the slot number |
| `timer.cancel` | `index` | |
| `wifi.scan` | — | the networks the hub sees: `result.networks: [{ssid, rssi, secure}]`, strongest first, one per name, hidden ones left out, at most 16. Blocks the hub for 2–3 s and pauses its own traffic while it listens on other channels |
| `wifi.set` | `ssid` (1..32 bytes), `password` (`""` = open network, 8..63 characters, or 64 hex digits) | move the hub to another network. The reply (`ok`) goes out first, ~0.7 s later the hub leaves its network and **tries the new one for 30 s**: if it gets an address the credentials are saved (NVS), otherwise it goes back to the old network, so a typo cannot strand it. Every connection drops meanwhile and the app must reconnect (the address may change on a different network: rediscover by mDNS). With no working network to go back to, the new credentials are kept at once. The password is never echoed, logged or sent back |
| `location.set` / `location.clear` | `lat`, `lon` (degrees, north/east positive; −90..90, −180..180) / — | the hub's position, for sunrise/sunset; kept in NVS; every client gets a `hub` event with the new `location` and today's `sun` |
| `scene.put` | `name`, `actions: [{entity, attr, value}…]` | creates or **replaces** a scene (name `[a-z][a-z0-9_]{0,22}`, 1..8 actions, up to 8 scenes). Every action must target a writable attribute of a real device (chips excluded) with a value that fits it, and an attribute may appear once: `bad_args` / `read_only` / `not_found` / `table_full`, nothing stored on error |
| `scene.del` | `name` | `not_found` if there is no such scene |
| `scene.apply` | `name` | sends every action as an ordinary command (so desired state and reconciliation see it) and answers `result: {sent, skipped}` once they are queued, not when the devices acked. An action whose entity has since gone, turned read-only or changed type, or that the command queue refused, is `skipped` rather than an error. Scenes change only through these ops, so clients just receive the new snapshot (`scenes` is a snapshot section; there is no scene event) |
| `pair` | `seconds` (optional, 1..254, default 180) | opens the Zigbee network for joining. Fire-and-forget: `res ok` means the command was sent to the Zigbee chip; a device that joins simply appears (the hub re-sends the snapshot). The hub does not report the window, so a client keeps its own countdown |
| `unpair` | `entity` | sends Leave to the device and drops it; the Zigbee chip then re-HELLOs so the brain forgets the entity (the snapshot is re-sent). Only devices on the Zigbee chip: a chip entity or an unknown id is `not_found`, a Matter-side entity `bad_args` |
| `identify` | `entity` | makes the device blink for ~10 s (same rules as `unpair`) |
| `history` | all optional: `before` (a `seq`: only events strictly older), `limit` (1..24, default 24), `entity` (only events about this device, also one that has since been removed) | a newest-first page of the event history (§5.1): `result: {now, latest, oldest, more, events}` |
| `ota.start` | `chip` (1 = Zigbee C6, 2 = Thread C6), `url` (`http://` or `https://`), `sha256` (64 hex characters) | updates that co-processor over the UART link, exactly as the console's `ota <chip> <url> <sha256>`. The hub downloads the image itself, so the URL must be reachable **from the hub**; the client uploads nothing. One update at a time: `busy` while another runs. See §5.2 |
| `ota.status` | — | where the update stands: `result: {state, pct, chip, message}` (§5.2) |
| `hub.reboot` | — | answers `ok`, then reboots the brain ~300 ms later, so the reply gets out first. The connection drops and the client reconnects |
| `hub.factory_reset` | — | answers `ok`, then ~500 ms later erases the hub's own settings (rules, names, schedules, timers, scenes, desired state, the app token, the relay credentials, the location) and reboots. The Wi-Fi credentials stay, and the co-processors keep their own partitions (their networks and images). See §5.2 |
| `access.get` | — | `result: {token, fingerprint, remote, relay_url}` (§5.2) |
| `access.token.set` | `token` (8..64 printable ASCII characters, no spaces) | the same rule as the console's `webtoken`. Answers `ok`, then ~300 ms later saves the token and restarts the app server: **the connection drops and the client must reconnect with the new token** |
| `access.relay.set` | `url` (`ws://`, `wss://`, `http://` or `https://`), `secret` | the remote-access relay (§13), like the console's `relay <url> <secret>`: the credentials are saved and the hub connects out. The secret is never sent back |
| `access.relay.clear` | — | disconnects the tunnel and erases the relay credentials |
| `wifi.forget` | — | answers `ok`, then ~300 ms later erases the saved Wi-Fi credentials and disconnects: the hub comes up on its setup access point (§11) |

Error codes: a missing or ill-typed argument is `bad_args` (the `message` names it);
an entity or attribute that does not exist, or an index with nothing behind it, is
`not_found`; a `then` that targets a read-only attribute is `read_only`; a full
table is `table_full`. Both the hub (`core::app`, `core/app_requests.hpp`) and the
mock hub follow this.

Every mutating op re-broadcasts the affected list (`rules`/`schedules`/…) to all
clients. A client that authenticated with the setup code (§11) may use `wifi.scan`
and `setup.apply` only — everything else, including the ops below, needs the token.

### 5.2 Firmware update, access and hub actions

The ops that update a co-processor, change how the hub can be reached, or change the
hub itself. Two of them answer with a result of their own shape:

```jsonc
// ota.status
{ "type": "res", "id": 9, "ok": true,
  "result": { "state": "downloading", "pct": 42, "chip": 1, "message": "downloading 667 KB" } }

// access.get
{ "type": "res", "id": 10, "ok": true,
  "result": { "token": "s3cret1", "fingerprint": "AA:BB:…", "remote": false, "relay_url": "" } }
```

`ota.status`: `state` is `idle` · `downloading` · `verifying` · `done` · `failed`
(`verifying` covers both the chip writing the image and the chip rebooting into it);
`pct` is the download progress, 0..100 (0 when unknown); `chip` is the chip being
updated (0 when idle); `message` is a short human-readable line for the card.

`access.get`: `token` is the token the client already authenticated with, so handing
it back exposes nothing new (the hub never logs it); `fingerprint` is the SHA-256 of
the hub's certificate — what the app pins (§10) — and is empty when the firmware has
no TLS identity; `remote` says whether a relay is configured; `relay_url` is its URL
(the relay's secret is never sent).

The ops that change the hub itself answer `ok` **first** and do the work a moment
later (300 ms, 500 ms for the factory reset), so the reply reaches the client before
the connection drops or the hub reboots. A client must expect the connection to drop
after `hub.reboot`, `hub.factory_reset` and `access.token.set`, and reconnect.

### 5.1 History

The hub keeps what happened in a fixed ring in RAM (last 256 events, `core/event_log.hpp`; the brain's `events [n]`
console command prints the same list). Every producer makes an `Event` and hands it to an `EventSink`; the ring is
one sink, a flash file or the SD recorder (SPEC Phase 7) would be another, with no change to the producers.

`history` is a request: a client fetches the newest page when it opens the screen, pages back with `before`, and
filters with `entity`. New events also arrive **by themselves** (below), so a client that sees pushes only needs to
ask again when a push does not join what it has; one that has seen none (an older hub) polls every few seconds. The reply:

```json
{"type":"res","id":7,"ok":true,"result":{"now":1791885071,"latest":42,"oldest":3,"more":true,"events":[
  {"seq":42,"age":3,"kind":"state","entity":256,"attr":1,"value":true},
  {"seq":41,"age":3,"kind":"command","entity":256,"attr":1,"value":true}]}}
```

- `seq` counts up from 1 since the hub booted and is never reused; the ring holds `oldest..latest`. **A reboot starts
  the numbers again** (the first event is then `boot`), so a client that sees `latest` go down starts over.
- `age` is seconds before the reply; `now` is the hub's wall clock (0 = not synced). A client computes `now - age`, so
  events from before the clock synced still get a right time.
- `more` is true while events older than the last one returned exist **for the same filter**; page back with
  `before = <seq of the last event>` until it is false.
- A page is at most 24 events so that the worst case fits one frame (tested).

| `kind` | what happened | fields besides `seq`, `age`, `kind` |
|---|---|---|
| `boot` | the hub (re)started — the gap marker | `what`: `power_on` `software` `panic` `watchdog` `brownout` `other` `unknown` |
| `state` | a device's attribute changed (not the continuous measurements: power, energy, light level, chip telemetry; not a button's return to idle) | `entity`, `attr`, `value` |
| `command` | the hub sent a command | `entity`, `attr`, `value`, `what`: who asked — `console` `app` `rule` `schedule` `timer` `scene` `matter` (a Matter controller) `reconcile` (the hub restoring what it last set); absent when unknown (a newer source is shown as it is, or not at all) |
| `command_failed` | …and it was not carried out | `entity`, `attr`, `what`: `fail` `unreachable` `unsupported` |
| `rule` / `schedule` / `timer` | one fired, just before its commands | `ref` (its slot), `entity`, `attr`, `value`; for one that applies a scene no entity, but `scene` |
| `scene` | a scene was applied | `ref` (its id), `scene` (its name, unless it was deleted since), `what`: `console` `app` `rule` `schedule` `timer` |
| `chip` | a co-processor changed health | `ref` (chip id), `what`: `lost` `resumed` `rebooted` `flapping`, `value` (seconds silent, or the reboot count) |
| `reconcile` | the hub found a device differing from what it last set (`corrected`: a corrective command follows) or changed outside the hub (`adopted`) | `entity`, `attr`, `value` (what the device has), `what` |
| `pairing` | the Zigbee network | `what`: `opened` (`ref` = seconds), `unpair` / `identify` (`entity`) |

**Live push.** About 250 ms after events are recorded the hub sends every authenticated client one message with
what is new, newest first, each event in the shape above:

```json
{"type":"history","now":1791885071,"latest":43,"events":[{"seq":43,"age":0,"kind":"state","entity":256,"attr":1,"value":false},
  {"seq":42,"age":0,"kind":"command","what":"app","entity":256,"attr":1,"value":false}]}
```

At most 24 events per message (a burst continues in the next one); nothing is sent while no client is connected, so a
client that arrives later fetches the newest page itself. A client folds a push on top of its list when the events join
what it has (`seq` right after its `latest`); when they do not (it missed one, the hub restarted) it asks with `history`.
A client that does not know the type ignores it. A device filter is the hub's, so a filtered view asks instead of
guessing which pushed events mention the device.

**What is not recorded.** A history wants changes, so a `state` that repeats the last recorded value of that
attribute is dropped (a restarted co-processor re-reports everything it knows; a button's `event` is exempt: two presses
are two events). A co-processor in a crash loop records one `rebooted` and one `flapping` a minute, not one per
restart; the reboot count in `value` still tells how many there were.

The `command` → `state` order is the causal one: a command is recorded when it is sent, the change when the device's
report arrives. A change with **no command, rule, schedule or timer before it** is the device changing by itself
(someone at the wall, a power cut). A new `kind` may appear in a later hub; clients show it as it is.

What this is not: long-term storage (it is RAM, a reboot empties it — the `boot` event says so), nor a log of every
reading. For that, add a sink.

## 6. Rule / schedule objects

Mirror the console grammar and `core::Rule`, with numeric ids:

```jsonc
{ "index": 0,
  "when":  { "entity": 1, "attr": 112, "op": "any" },                 // op: any == != < <= > >=
  "then":  { "entity": 256, "attr": 1, "value": true },               // or { "scene": "evening" }
  "between": { "from": "18:00", "to": "23:00" },                       // optional, local time
  "and":   { "entity": 257, "attr": 1, "op": "==", "value": true },   // optional
  "cooldown_s": 300 }                                                  // optional
```

```jsonc
{ "index": 0, "time": "22:00", "days": "daily", "entity": 256, "attr": 1, "action": "off" }
// days: "daily" | "weekdays" | "weekends" | ["mon","tue",…]
// or, applying a scene:  { "index": 0, "time": "22:00", "days": "daily", "scene": "evening" }
// a one-off:             { "index": 0, "time": "19:00", "days": "once", "date": "2026-10-20", "entity": 256, "attr": 1, "action": "on" }
```

**One-off schedules.** `schedule.add` with `date` (a real date in 2000..2099, the hub's local day) runs once on that
day, within 10 minutes after `time` (like any schedule), and then removes itself; the list is re-sent. `date` and
`days` exclude each other, and a date before the hub's local today is `bad_args` (while the clock is not synced
the hub cannot tell, so it accepts it). A one-off whose day passed without it running (hub off, clock never
synced) is dropped silently the next day. Console: `at 2026-10-20 19:00 on plug1`. Stored as schedule blob v2 (v1 still loads).

```jsonc
{ "index": 0, "entity": 256, "attr": 1, "action": "on", "left_s": 42 }   // a pending timer
{ "index": 1, "scene": "evening", "left_s": 600 }                         // …that applies a scene
```

**Times at the sun.** A schedule's `time` and a rule's `between.from`/`between.to` are *time points*:
`"HH:MM"` (local) or `"sunrise"`/`"sunset"` with an optional signed offset in minutes, `"sunset+30"`,
`"sunrise-15"` (−180..+180; anything else is `bad_args`). The hub computes today's sunrise and
sunset from its location (NOAA algorithm, within about a minute of an independent implementation
on 42 place-and-date combinations worldwide, polar day and night aside) and the clock's zone, with DST.
Without a location, before the clock is synced, or on a day the sun does not rise or set, a sun-relative
schedule does not fire and a rule with a sun-relative window stays quiet (like an unknown clock).
A schedule whose offset time falls outside the day (`sunset+30` when the sun sets at 23:50) is
skipped that day; a rule window that ends past midnight wraps. `between` ends must differ. Times
are echoed in the snapshot exactly as canonical text (`"sunset"`, not `"sunset+0"`).

A schedule, timer or rule may apply a **scene** instead of switching one device: `schedule.add`
and `timer.add` take `scene: "<name>"` in place of `entity`/`action` (giving both is `bad_args`,
an unknown scene `not_found`), and `rule.add` takes `then: { "scene": "<name>" }`. The hub keeps
the reference by the scene's stable id (a scene keeps its id when replaced by `scene.put`), shows
it by name, and refuses to `scene.del` a scene that any schedule, timer or rule uses
(`bad_args`). A rule applies its scene from the main loop, a moment after it matches.

## 7. Errors

`{ "type": "error", "id"?: n, "code": "…", "message"?: "…" }`. Codes:
`unauthorized`, `version`, `busy`, `bad_json`, `too_large`, `unsupported`,
`bad_args`, `not_found`, `read_only`, `table_full`, `internal`.
`message` is for humans/logs only; clients switch on `code`.

## 8. Threading on the hub (not part of the protocol, but a constraint)

The WebSocket handlers run in `esp_http_server`'s task. They **never** touch core
objects directly: like the console, they enqueue a request that the main loop
executes (core access stays single-threaded) and wait for the reply. Events
(`state`, `chip`, …) are produced in the main loop / UART RX context, copied into
a bounded per-client queue, and sent from the server task. A full queue drops the
client (§4).

## 9. Resource budget (measured 2026-10-10, `heap` console command)

Live brain, Wi-Fi up, both chips ready, before the server: ~230 KB free internal
heap; app 1.1 MB of a 1.9 MB slot; PSRAM (2 MB) present, not enabled.
(Latest, 2026-10-11, with TLS, mDNS, scenes and the new ops: the image is 1.24 MB = 63 % of the
slot; the server off: 215 KB free; the TLS figures below still hold, 3 TLS clients ≈ 153 KB.)

Measured with stage 3 (plain `ws://`), free / lowest since boot / largest block:

| state | free | lowest | largest block |
|---|---|---|---|
| server running, no clients | 211 KB | 199 KB | 168 KB |
| 3 authenticated clients | 210 KB | 195 KB | 160 KB |
| after they disconnect | 211 KB | 195 KB | 168 KB |

The server itself costs ~15–20 KB; three clients add ~1 KB (lwIP's pools are
preallocated) and nothing leaks.

**With TLS (stage 5, measured):**

| state | free | lowest since boot | largest block |
|---|---|---|---|
| TLS server, no clients | 208 KB | – | 164 KB |
| 3 TLS clients | 160 KB | 137 KB | 111 KB |
| after they disconnect | 208 KB | 137 KB | 164 KB |

A TLS session costs ~16 KB once established. With mbedTLS's default 16 KB input buffer
it was ~26 KB (3 clients: 130 KB free, a transient low of 97 KB during a run with four
concurrent handshakes), so `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` is 6144 and OUT is 4608.
**Max 3 clients**, and the rule for every stage: with 3 connected clients free heap
must stay above ~100 KB, transients included.

## 10. TLS and trust (the hard part)

A device on a LAN cannot get a public CA certificate, so each hub has its **own
self-signed ECDSA P-256 certificate**. `scripts/gen-hub-cert.sh` creates it (10-year
validity, SAN `keystone-hub.local`, extra names as arguments) as
`firmware/brain-s3/main/hub_{cert,key}.pem` plus a generated `hub_identity.cpp` that
the firmware compiles in; all three are gitignored, so the private key never enters
git. The script refuses to overwrite an existing identity without `--force`, because a
new certificate invalidates every pinned fingerprint. Without `hub_identity.cpp` the
firmware still builds and serves plain `ws://` only.

**What the hub does.** With an identity it serves `wss://<hub>:8443/ws` (TLS 1.2,
ECDHE-ECDSA with AES-GCM; TLS 1.3 is off in the IDF config) and **nothing on 8080**.
Plain `ws://` is a development switch: console `webplain on` serves it instead of TLS
(one server at a time, to spare heap), `webplain off` goes back. The console prints the
certificate's SHA-256 with `webtoken` and when the server starts; that is what a person
compares with what the app shows. No sessions are cached or resumed.

**Pinning.** The app trusts a hub by the SHA-256 fingerprint of its certificate
(`Settings.fingerprint`, any notation, stored as `AA:BB:…`), not by a CA. A socket that
pins verifies the fingerprint **before it reports `open`**, so the token (sent in `hello`)
never reaches a server that does not hold the pinned certificate. A mismatch is final
(`status: untrusted`, `certificate_mismatch`; the client does not retry), and a hub
whose certificate is replaced is caught on the next reconnect. Trust-on-first-use is done
by a person: `npm run probe -- --url wss://…` without `--fingerprint` prints what the
hub presents and stops.

**Through a relay the pin is dropped.** A relay is a different peer: it presents **its
own** certificate (a CA-signed one, checked by the platform the usual way), and the hub's
self-signed certificate never appears on the wire. Pinning the hub's fingerprint there
would refuse every connection, so `Settings.pinnedFingerprintFor()` returns nothing for
a `/client/<hub-id>` address and the platform's own TLS validation takes over. The hub's
identity then rests on its **token** (the hub refuses a wrong one), and the relay is a
trusted party — it sees the traffic (see [relay-security.md](relay-security.md)).

**By platform** (what is implemented and what was actually exercised):

| Platform | How it trusts the hub | Status |
|---|---|---|
| Node (tools, tests) | pinned fingerprint (`tools/pinned-socket.ts`) | implemented and verified against the real hub: 31/31 conformance checks over TLS, wrong pin refused, token never sent |
| Browser (web build) | the browser's own certificate exception: open `https://<hub>:8443/` once and accept it | verified in Chromium against the real hub. Browsers expose nothing about TLS to a page, so a pin **cannot** be checked: with a fingerprint set the app refuses (`pin_unsupported`) instead of pretending. A browser without the exception just fails, and the app shows a hint |
| iOS / Android (React Native) | the local Expo module `app/modules/keystone-net`: a pinned WebSocket (Android: OkHttp with a trust manager that accepts exactly the pinned leaf certificate and no host-name check; iOS: `URLSessionWebSocketTask` whose server-trust delegate compares the leaf's SHA-256) plus an mDNS browser (Android `NsdManager`, iOS `NetServiceBrowser`) | **written, NOT compiled and NOT run on a device.** There is no Android/Apple toolchain here: the Kotlin and Swift pass only a tree-sitter syntax check. Verified: the TS side against a fake module (14 tests), autolinking finds the module on both platforms, `expo prebuild` accepts `app.json` and puts the iOS local-network/Bonjour keys and the Android multicast permission into the generated projects. Stock RN `WebSocket` cannot pin and rejects a self-signed certificate, so the module is the only route; it needs a dev/production build (`npx expo run:ios` / `run:android` or EAS), not Expo Go. Without the module (Expo Go, web) a pinned hub is refused with `pin_unsupported` |

`SocketFactory` (`src/protocol/client.ts`) is the seam: `src/native/platform.ts` picks the
native pinned socket when the build has the module, and otherwise a pinned hub gets
`PinUnsupportedError`; without a pin (ws://, or wss:// with a real certificate) the
platform's own WebSocket is used. Three layers keep a native bug from silently turning
pinning off: the platform reports `open` only after the certificate matched; it also reports
the fingerprint it saw, which JS compares again (an `open` with a missing or different
fingerprint is treated as a mismatch); and the token goes out only on `open`.

**Limits to know.** The hub's private key sits in unencrypted flash (anyone with the
board can read it; flash encryption is a separate project). The 6 KB TLS input buffer
means a client must not send a frame over ~6 KB (the protocol caps frames at 4 KB
anyway). A certificate is valid for 10 years; renewing means regenerating and
re-pinning everywhere.

## 11. First-time setup (a hub with no network, no console)

A hub that was just flashed knows no Wi-Fi network and has no token, so nothing can reach it. The
console fixes that with `wifi_join` and `webtoken`; this section is the way for a person who has no
console (and the way to recover a hub whose network is gone for good).

**The setup code.** `scripts/gen-hub-cert.sh` puts a 12-character *setup code* into the generated
`hub_identity.cpp` next to the TLS identity (`--setup-code` adds one to an existing identity and keeps
its key and certificate, so pinned fingerprints stay valid). The alphabet has no look-alikes (no 0/O,
1/I/L; ~59 bits). It is meant for a label on the hub, shown as `ABCD-EFGH-JKMN`; the hub console prints
it with `setup`. A firmware built from an identity without one has no setup mode at all.

**The access point.** The hub opens a Wi-Fi network `Keystone-XXXX` (last two bytes of its MAC), WPA2,
password = the setup code, at 192.168.4.1 (the app server listens there too, `wss://192.168.4.1:8443/ws`).
`core::SetupPolicy` decides when it is up: at once when the hub has **no** network credentials, after
**10 minutes** without a connection when it has some (the router was replaced), and once open it stays
until the hub has had its network for a minute (long enough for the app to see the result). The console
forces it with `setup on|off` (and shows the state; `wifi_forget` is the way to test the blank-hub case).

**A setup client** signs in with `hello` whose token is the setup code (case, dashes and spaces do not
matter). It gets `welcome` with `"setup": true`, **no snapshot and no events**, and may send exactly
`wifi.scan` and `setup.apply`. Everything else (`cmd`, every other `req`) is refused with
`unsupported` (`"setup only"`), twice: in the server task and again in the main loop. A normal client
cannot use `setup.apply`. Failed codes count towards the same lockout as failed tokens.

| op | args | effect |
|---|---|---|
| `setup.apply` | `ssid`, `password` (as `wifi.set`), `token` (8..64 printable, no spaces) | saves the token and moves the hub to the network, both ~0.7 s after the reply. The app server restarts with the new token (every connection drops), and the network is applied as in `wifi.set`; with no working network the credentials are kept at once, so a **wrong password leaves the hub in setup mode** (the access point stays and the person can try again) |

**Trust.** The hub's certificate is self-signed and not known to the phone yet, so for this one
connection the socket is asked to *learn*: it accepts any certificate and reports its SHA-256 in `open`
(`SocketOptions.learn`; Swift/Kotlin `connectLearning`). Nothing is sent to the hub before that besides the
`hello`, and the only link on which this is done is the access point, which is WPA2 with the setup code as
password: a device that can talk to it already knows the code (which is also the token for this
connection), so a fingerprint learned there is the hub's. The app stores it and pins it from then on, on the
home network, like any other hub. Browsers and Expo Go cannot learn (`unsupported`).

**The app** (`SetupScreen`, "Set up a new hub" on the connect screen): the person joins `Keystone-XXXX` in the
phone's settings, types the code, picks the home network (the scan runs by itself) and its password. The app
invents the token (16 random bytes, 32 hex digits; the person never sees or types it), sends `setup.apply`, then
asks the person to put the phone back on the home Wi-Fi and connects with the learned fingerprint, the new token
and the address found by mDNS (fallback `keystone.local`). `npm run setup-check` is the conformance check.

**Limits.** The setup code sits in unencrypted flash next to the private key (see §10). A phone on a Wi-Fi network
without internet may route traffic elsewhere: iOS sends local addresses over Wi-Fi anyway; Android may need the
network to be kept (untested). The Wi-Fi password travels over TLS inside WPA2 and is never stored by the app.
While the access point is up a second Wi-Fi network is on the air (a visible `Keystone-…`, not a way in without the
code).

## 12. Stages

1. **This document** — agree on the protocol.
2. ✅ **Mock hub + client library** (`app/`, 41 tests): a Node mock hub that speaks this
   protocol with fake entities, and the TypeScript client (connection, auth,
   snapshot/event store, request/ack correlation) with unit tests. The app can be
   built and tested on a dev machine with no hardware.
3. ✅ **Hub, `ws://` + token:** console `webtoken <token>|clear` (stored in NVS,
   never printed; the server runs only while a token is set), handshake with
   constant-time compare, 5 s hello timeout, lockout after 5 failures (60 s), max
   3 clients, snapshot in parts, `state`/`chip` events, `cmd`/`accepted`/`ack`
   (a silent chip completes at once as `unreachable`), backpressure (a client with
   24 queued frames is dropped). JSON layer and message layer host-tested (40
   tests); `app/tools/hub-check.ts` runs 19 conformance checks against a live hub
   (mock or real) and all pass on the brain (36 with `--mutate` over TLS); heap measured (§9).
4. ✅ **App screens** (Expo, `app/`): connect screen, device cards with switches /
   brightness / live values, offline marking when a chip is silent, failure messages, chip
   health, auto-reconnect that keeps the last known state (a **state cache** on the
   device — a file on a phone, localStorage in a browser — so the app still shows the
   devices and their last values while the hub is out of reach; the banner says how old
   they are); **Automations** tab (schedules,
   timers with a live countdown, rules with time window / extra condition / cooldown, each
   readable as a sentence, delete with a second tap), renaming a device. Verified in a real
   browser against the real brain: a click toggles the plug in 280–490 ms, and
   schedules, timers, rules and names created in the browser show up on the hub's
   console and delete cleanly. Added later: **scenes** (a bar of one-tap scenes, a form
   that saves the devices' current state; schedules, timers and rules can apply a scene)
   and **add / blink / remove a Zigbee device**.
5. ✅ **TLS** (hub, tools, web): `scripts/gen-hub-cert.sh`, `wss://:8443`, the console
   shows the fingerprint, `webplain` for development, pinning in the Node client with
   the token withheld until the pin matches, a fingerprint field in the app (refuses
   where pinning is impossible), heap re-measured (§9). 12 pinning tests against a TLS
   mock hub; `hub-check` runs 5 extra TLS checks (31 in all with `--mutate`) and passes on
   the brain. *Open:* pinning on iOS/Android (§10).
6. ✅ **mDNS** (hub + Node tool): `espressif/mdns` (IDF 6 managed component) advertises
   `_keystone._tcp` and `keystone.local`; checked on the brain with an independent raw
   mDNS query (PTR → SRV 8443 → TXT `tls=1,name=home,v=1` → A), with the record switching to
   8080/`tls=0` on `webplain on` and back, and withdrawn on `webtoken clear`. `app/src/protocol/mdns.ts`
   is a small pure codec (compression pointers, bounded against loops/truncation, goodbye
   packets, unknown versions ignored; 8 tests), `npm run discover` prints the hub URL.
   Costs 7 KB of heap (idle 201 KB, 3 TLS clients 153 KB, lowest 129 KB, no leak).
   Discovery inside the phone app: native browser in `keystone-net`, a list on the connect
   screen (tapping a hub fills the address, never the fingerprint: mDNS is unauthenticated).
   Unverified on a device, see §10. *Open:* polish.

7. ✅ **First-time setup** (hub + app + tool): §11. Setup access point, setup-code sign-in, `setup.apply`,
   `SetupScreen`, `npm run setup-check`. Hub verified over the LAN; the access point and the phone are not.

8. ✅ **History** (hub + app + tools): §5.1. Typed events from every producer into a RAM ring, the `history` request,
   a History tab (newest first, grouped by day, tap a row to see one device, "load older"), the brain's `events`
   console command. Verified on the brain: `hub-check --mutate` over wss 41/41 (2 history checks), the tab driven in a
   browser against the real hub (a click on a switch shows as `Sent to …` then the change). The mock hub keeps the
   same ring. Later: `command` events say who asked (`what`), new events are pushed (`type: "history"`), repeated
   `state` and crash-loop `chip` events are not recorded again.

9. **Remote access / relay** (hub + relay server + app + console; hardware-verified):
   §13. The brain connects out to a relay server (`esp_websocket_client`); a cloud relay
   exposes HTTP+SSE for apps; a console tool gives a full REPL. The connect screen has
   Local / Cloud / Relay modes. The ESP relay client was verified on the real hub
   against a public relay (commit `2d5be9e`): it dialed out, registered the tunnel and
   answered a client that came through the public wss endpoint.

10. ✅ **Firmware update, access and hub actions** (hub side; §5.2): `ota.start` /
    `ota.status` (the co-processor update the console's `ota` command drives, with the
    progress the app's card shows), `access.get` / `access.token.set` /
    `access.relay.set` / `access.relay.clear`, `hub.reboot`, `hub.factory_reset` and
    `wifi.forget`. The hub answers first and does the work a moment later (300–500 ms),
    so the reply gets out before the connection drops or the hub reboots. The parsing and
    the validation live in `core/app_requests.hpp` (host-tested), the encoders in
    `core/app_protocol.hpp`.

## 13. Remote access (relay)

Remote access lets a phone or web app reach the hub from outside the LAN through a
relay server on a VPS.  Three deployment options exist; all share the hub's token-based
auth end-to-end.

**The relay is a trusted component.**  Inside the tunnel the hub protocol travels as
plain JSON (the hub's relay client decodes it directly, `firmware/brain-s3/main/relay_client.cpp`),
so the relay sees the hub token and the traffic.  Each hop is protected by TLS — the
hub verifies the relay's certificate against the Mozilla CA bundle — but the relay
itself is not blind to the content.  See [relay-security.md](relay-security.md) for the
threat model, the findings and the target design (per-hub secret, TLS mandatory,
limits).

### 13.1 Architecture options

| Option | How it works | Needs a LAN device? |
|--------|-------------|---------------------|
| **ESP relay client** (built into `brain-s3`) | The brain opens a WebSocket to the relay directly, using the Mozilla CA bundle for TLS.  Remote clients appear in the main loop exactly like local ones (negative `fd`). | No |
| **Tunnel agent** (`npm run relay-tunnel`) | A Node.js process on the hub's LAN (e.g. the Pi) connects to the relay and opens a local WebSocket to the hub for each remote client. Transparent. | Yes |
| **Cloud relay** (`npm run cloud-relay`) | A relay that exposes an HTTP API + SSE to the app instead of a WebSocket per client.  `POST /hub/:id/auth` authenticates, `POST /hub/:id/cmd` and `/req` forward commands, `GET /hub/:id/events` is a live SSE stream. | No (hub connects via ESP or tunnel) |

### 13.2 Wire protocol (relay ↔ hub/tunnel)

Text WebSocket frames, same for all three options:

| Frame | Direction | Meaning |
|-------|-----------|---------|
| `!auth:<secret>` | hub/tunnel → relay | First message; shared secret |
| `!ok` | relay → hub/tunnel | Auth accepted |
| `!open:<cid>` | relay → hub/tunnel | A remote client connected |
| `!ready:<cid>` | hub/tunnel → relay | Local connection ready, start forwarding |
| `!close:<cid>` | either | Client disconnected |
| `!error:<msg>` | relay → hub/tunnel | Error message |
| `<cid>:<payload>` | either | A hub-protocol JSON frame for client `<cid>` |

The relay assigns a numeric `cid` (client id) to each remote client.  The hub/tunnel
matches it to a virtual or local socket.  The hub-protocol payload (`hello`, `welcome`,
`cmd`, `state`, …) is forwarded as-is — the relay does not inspect it.

### 13.3 ESP relay client (brain-s3)

`firmware/brain-s3/main/relay_client.{hpp,cpp}`.  Console:

- `relay` — show status (url, connected/disconnected, authenticated, remote client count)
- `relay <url> <secret>` — save credentials to NVS (`relay_url`, `relay_sec`), connect
- `relay clear` — disconnect, erase NVS credentials

Credentials persist across reboots.  The relay client starts automatically on boot if
credentials are saved.  Virtual client fds are `relay::kFdBase - cid` (≤ −100) so dispatch
routes them to `relay::send` instead of the local WebSocket server.

The hub's `webtoken` is shared: when it changes, both the local server and the relay client
get the update; clearing the token stops the local server but the relay stays connected
(it just won't authenticate anyone).

Uses `esp_websocket_client` (managed component `espressif/esp_websocket_client ≥ 1.0.0`)
with the Mozilla CA root bundle for TLS to public relays.  Reconnects automatically
(10 s timeout).  Up to 3 virtual clients; a 4th gets `!close`.  Hello timeout 5 s.

### 13.4 Cloud relay API

`app/tools/cloud-relay.ts`.  Run: `npm run cloud-relay -- --port 9090 --secret <secret>`.

The hub connects the tunnel WebSocket to `ws(s)://host:port/tunnel/<hub-id>`.
App clients use the HTTP API:

| Method | Path | Auth | Body / result |
|--------|------|------|---------------|
| `POST` | `/hub/:id/auth` | — | `{token}` → `{session, messages: [...]}` |
| `GET`  | `/hub/:id/state` | Bearer session | `{messages: [...]}` (cached snapshot) |
| `POST` | `/hub/:id/cmd` | Bearer session | `{entity, attr, value}` → `{status}` |
| `POST` | `/hub/:id/req` | Bearer session | `{op, args?}` → `{ok, result?}` |
| `GET`  | `/hub/:id/events` | Bearer or `?session=` | SSE stream of live events |

Sessions expire after 10 min of inactivity (no SSE, no pending requests).

### 13.5 App client

`app/src/protocol/cloud-client.ts` — `CloudClient` class with the same public interface
as `HubClient` (`subscribe`, `getSnapshot`, `cmd`, `req`, `subscribeHistory`).  The
connect screen has a **Cloud** mode (server + hub ID) and a **Relay** mode (direct WebSocket
to the relay's `/client/<hub-id>` endpoint, works with the existing `HubClient`).
`useHubClient` picks `CloudClient` or `HubClient` based on the URL shape.

### 13.6 Console

`app/tools/console.ts` — an interactive CLI for the hub over the app protocol.
Run: `npm run console -- --url ws://hub:8080/ws --token <token>`.
Supports all device, automation, hub and management commands (type `help`).
Live state updates are printed as they happen (`--quiet` to suppress).

### 13.7 Not verified / not done

- The cloud relay has no persistent state (sessions are in memory).
- No auth rate limiting on the **cloud** relay (the hub has its own lockout, and the
  tunnel relay has one since 29.10.2026).
- The app's Cloud mode has not been tested against a live cloud relay + hub.
- The relay still uses **one shared secret for all hubs** (`RELAY_SECRET`); the target
  is a per-hub secret that never crosses the wire (nonce + HMAC). See
  [relay-security.md](relay-security.md).
- The relay sees the hub token and the traffic inside the tunnel — it is a trusted
  component, not a blind one.

### 13.8 Deploying the relay

`relay.Dockerfile` builds the relay from `relay/` (Go — one static binary, no runtime
inside the image):

```bash
docker build -f relay.Dockerfile -t keystone-relay .
docker run --rm -p 9090:9090 -e RELAY_SECRET=<secret> keystone-relay
```

Locally: `scripts/build-relay.sh` puts the binary into `build/` (`go test ./...` in
`relay/` runs the policy and the server tests).

| Variable | Meaning |
|---|---|
| `PORT` | the port to listen on (Railway sets it) |
| `RELAY_SECRET` | the shared secret the hub authenticates with — **required** |
| `RELAY_STATUS_TOKEN` | if set, the status page shows the hub ids only to whoever sends this token (`Authorization: Bearer …` or `?token=`); without it the page shows counters only |
| `RELAY_ALLOWED_ORIGINS` | comma-separated browser origins allowed to open a socket. Native clients (the hub, the phone app, the console) send no `Origin` and are always allowed; a browser is refused unless it is listed here |

TLS is terminated in front of the container. The hub verifies the relay's certificate
against the Mozilla CA bundle, so the certificate has to come from a public CA —
the deployed relay uses a Let's Encrypt certificate for `*.up.railway.app`, and
plain HTTP answers `301` to HTTPS.

The deployed instance (29.10.2026) was checked from outside: TLS valid, HTTP
redirects to HTTPS, the status page answered `200` with the hub ids — that page is
now behind a token.

Out of scope for v1: multiple users/roles, push
notifications, OTA from the app (see [backlog](backlog.md#firmware-update-from-the-phone-app)).
