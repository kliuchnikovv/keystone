# App protocol — how a phone/web client talks to the hub

Status: **v1, stages 1–3 done.** The client library, a mock hub (stage 2) and the
hub side over plain `ws://` with a token (stage 3: handshake, snapshot, live
`state`/`chip` events, `cmd`/`ack`) are implemented and verified on hardware. Still
to come: management requests (`req`), TLS, screens, mDNS discovery. The hub serves no UI
and no static files; it only speaks this protocol. The client is a React Native
(Expo) app that also builds for the web, in [`app/`](../app/).

Decisions (maintainer, 2026-10-10): WebSocket + JSON; Expo (iOS, Android, web);
shared token **and** TLS; code in `app/` of this repo.

This is a *client* protocol. It is unrelated to hubproto (COBS+CRC over UART
between the chips) and does not change it; the hub translates.

## 1. Transport

- `wss://<hub>:8443/ws` (TLS). `ws://<hub>:8080/ws` exists only as a development
  fallback, compiled in with a Kconfig flag and off by default.
- One WebSocket, text frames, one JSON object per frame, UTF-8, max 4096 bytes.
  The hub closes a connection that sends more.
- WebSocket ping/pong keeps it alive (hub pings every 20 s; no pong in 10 s = drop).
- At most **3 simultaneous clients** (heap, see "Resource budget"). A 4th gets
  `error: busy` and is closed.
- Discovery: mDNS service `_keystone._tcp`, TXT `v=1`, `name=<hub name>`. A client
  can always connect to a typed-in address instead.

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
     "hub": { "name": "home", "fw": "7843023", "time": 1781000000, "tz": "UTC", "synced": true } }
   ```
   or `{ "type": "error", "code": "unauthorized" }` and closes. A different major
   `v` gets `error: "version"`.
3. The token is **never** in the URL (URLs end up in logs). It is compared in
   constant time. After 5 failed attempts from one IP the hub refuses that IP
   for 60 s. The token is set on the hub console (`webtoken <value>`, stored in
   NVS) — there is no way to read or set it over this protocol.

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
  "timers":    [ … ] }
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

**What the real hub does today.** Live attribute changes are pushed as `state`
events and chip health as `chip` events (on change, and every 10 s for telemetry).
Any *structural* change — an entity appearing or disappearing, a rename, a rule,
schedule or timer added/removed/fired — is delivered by **re-sending the snapshot**
(part 0 again) instead of the finer-grained `entity`/`rules`/`schedules`/`timers`
events above, because a list can exceed one frame. Clients must therefore handle
the snapshot at any time (the reducer does); the fine-grained events remain valid
protocol for a hub that wants to use them. Checked at most once a second.

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
| `name.set` / `name.clear` | `entity`, `name` | names `[a-z0-9_]{1,15}`, unique |
| `rule.add` | rule (§6) | `res.result.index` |
| `rule.del` | `index` | later rules shift down |
| `schedule.add` | `time` `"HH:MM"`, `days`, `entity`, `action` | `action`: `on`/`off`/`toggle` |
| `schedule.del` | `index` | |
| `timer.add` | `delay_s`, `entity`, `action` | |
| `timer.cancel` | `index` | |
| `pair` / `unpair` | `seconds` / `entity` | Zigbee join window |
| `identify` | `entity` | |

Every mutating op re-broadcasts the affected list (`rules`/`schedules`/…) to all
clients. Anything that changes the hub's security posture (token, Wi-Fi
credentials, OTA) is **not** available over this protocol.

## 6. Rule / schedule objects

Mirror the console grammar and `core::Rule`, with numeric ids:

```jsonc
{ "index": 0,
  "when":  { "entity": 1, "attr": 112, "op": "any" },                 // op: any == != < <= > >=
  "then":  { "entity": 256, "attr": 1, "value": true },
  "between": { "from": "18:00", "to": "23:00" },                       // optional, local time
  "and":   { "entity": 257, "attr": 1, "op": "==", "value": true },   // optional
  "cooldown_s": 300 }                                                  // optional
```

```jsonc
{ "index": 0, "time": "22:00", "days": "daily", "entity": 256, "attr": 1, "action": "off" }
// days: "daily" | "weekdays" | "weekends" | ["mon","tue",…]
```

```jsonc
{ "index": 0, "entity": 256, "attr": 1, "action": "on", "left_s": 42 }   // a pending timer
```

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

Measured with stage 3 (plain `ws://`), free / lowest since boot / largest block:

| state | free | lowest | largest block |
|---|---|---|---|
| server running, no clients | 211 KB | 199 KB | 168 KB |
| 3 authenticated clients | 210 KB | 195 KB | 160 KB |
| after they disconnect | 211 KB | 195 KB | 168 KB |

The server itself costs ~15–20 KB; three clients add ~1 KB (lwIP's pools are
preallocated) and nothing leaks. Each TLS session is estimated at ~50 KB while it
exists, so stage 5 must be re-measured. **Max 3 clients**, and the rule for every
stage: with 3 connected clients free heap must stay above ~100 KB.

## 10. TLS and trust (the hard part)

A device on a LAN cannot get a public CA certificate, so the hub presents a
**self-signed ECDSA P-256 certificate, one per device**, generated by
`scripts/gen-hub-cert.sh` into gitignored files embedded at build time (same
pattern as `wifi_secrets.h`); the key never enters git.

Clients must then trust it explicitly:
- **App (native):** pin the certificate's SHA-256 fingerprint, trust-on-first-use:
  the first connection shows the fingerprint and the user confirms it (the hub
  also prints it on its console). This needs a native module, so the app is run
  as an Expo **development build**, not Expo Go.
- **Web:** browsers cannot pin; the user opens `https://<hub>:8443/` once and
  accepts the certificate, after which `wss://` works in that browser.

Until that stage lands, development uses the `ws://` fallback (§1) on a trusted
network.

## 11. Stages

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
   tests); `app/tools/hub-check.ts` runs 20 conformance checks against a live hub
   (mock or real) and all pass on the brain; heap measured (§9).
4. **App screens:** entity list + toggles, chips/health, then rules, schedules,
   timers, names.
5. **TLS:** certificate script, `wss://`, fingerprint pinning in the app, heap
   re-measured with TLS sessions.
6. **mDNS discovery** and polish.

Out of scope for v1: remote (outside-LAN) access, multiple users/roles, push
notifications, OTA from the app (see [backlog](backlog.md#firmware-update-from-the-phone-app)).
