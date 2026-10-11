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
              "location": { "lat": 50.08, "lon": 14.43 },        // null until set
              "sun": { "rise": "06:12", "set": "20:01" } } }       // today, local; null if unknown
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
| `schedule.add` | `time` `"HH:MM"` or `"sunrise"`/`"sunset"` with an optional offset `"sunset+30"` (±180 min, see below), `days` (default daily), `entity`, `action` — or `scene` instead of the last two | `action`: `on`/`off`/`toggle`; the entity needs a writable on/off. `index` is the slot number, as in the console's `unschedule n` (slots are reused, so indexes can have gaps) |
| `schedule.del` | `index` | |
| `timer.add` | `delay_s` (1..604800), `entity`, `action` — or `scene` instead of the last two | `index` is the slot number |
| `timer.cancel` | `index` | |
| `location.set` / `location.clear` | `lat`, `lon` (degrees, north/east positive; −90..90, −180..180) / — | the hub's position, for sunrise/sunset; kept in NVS; every client gets a `hub` event with the new `location` and today's `sun` |
| `scene.put` | `name`, `actions: [{entity, attr, value}…]` | creates or **replaces** a scene (name `[a-z][a-z0-9_]{0,22}`, 1..8 actions, up to 8 scenes). Every action must target a writable attribute of a real device (chips excluded) with a value that fits it, and an attribute may appear once: `bad_args` / `read_only` / `not_found` / `table_full`, nothing stored on error |
| `scene.del` | `name` | `not_found` if there is no such scene |
| `scene.apply` | `name` | sends every action as an ordinary command (so desired state and reconciliation see it) and answers `result: {sent, skipped}` once they are queued, not when the devices acked. An action whose entity has since gone, turned read-only or changed type, or that the command queue refused, is `skipped` rather than an error. Scenes change only through these ops, so clients just receive the new snapshot (`scenes` is a snapshot section; there is no scene event) |
| `pair` | `seconds` (optional, 1..254, default 180) | opens the Zigbee network for joining. Fire-and-forget: `res ok` means the command was sent to the Zigbee chip; a device that joins simply appears (the hub re-sends the snapshot). The hub does not report the window, so a client keeps its own countdown |
| `unpair` | `entity` | sends Leave to the device and drops it; the Zigbee chip then re-HELLOs so the brain forgets the entity (the snapshot is re-sent). Only devices on the Zigbee chip: a chip entity or an unknown id is `not_found`, a Matter-side entity `bad_args` |
| `identify` | `entity` | makes the device blink for ~10 s (same rules as `unpair`) |

Error codes: a missing or ill-typed argument is `bad_args` (the `message` names it);
an entity or attribute that does not exist, or an index with nothing behind it, is
`not_found`; a `then` that targets a read-only attribute is `read_only`; a full
table is `table_full`. Both the hub (`core::app`, `core/app_requests.hpp`) and the
mock hub follow this.

Every mutating op re-broadcasts the affected list (`rules`/`schedules`/…) to all
clients. Anything that changes the hub's security posture (token, Wi-Fi
credentials, OTA) is **not** available over this protocol.

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
```

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
   tests); `app/tools/hub-check.ts` runs 19 conformance checks against a live hub
   (mock or real) and all pass on the brain (36 with `--mutate` over TLS); heap measured (§9).
4. ✅ **App screens** (Expo, `app/`): connect screen, device cards with switches /
   brightness / live values, offline marking when a chip is silent, failure messages, chip
   health, auto-reconnect with the stale picture cleared; **Automations** tab (schedules,
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

Out of scope for v1: remote (outside-LAN) access, multiple users/roles, push
notifications, OTA from the app (see [backlog](backlog.md#firmware-update-from-the-phone-app)).
