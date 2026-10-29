# keystone app

Client side of the hub: the protocol library, a mock hub and the Expo app (iOS,
Android and web from one codebase). The hub serves no UI; everything goes through
the WebSocket+JSON protocol in [../docs/app-protocol.md](../docs/app-protocol.md).

## Layout

- `src/protocol/` — wire types, the pure `reduce()` that applies hub messages, and
  `HubClient` (handshake + token, request/ack correlation, reconnect with back-off;
  works with `useSyncExternalStore`). No React, no Node-only APIs.
- `src/protocol/cache.ts`, `src/statecache.ts` (a file on the phone) /
  `src/statecache.web.ts` (localStorage in a browser), `src/hooks/useStateCache.ts` —
  the **state cache**: the last state the hub sent, kept on the device. The app shows it
  while the hub is out of reach (the banner says how old it is: "the last known state
  (3 min ago)") and after the app is closed and opened again. The cache belongs to one
  hub: another address gets another home, not this one's devices.
- `src/model/` — plain functions that turn protocol data into what people see
  (value formatting, device titles, chip health, error messages) and what people
  type into `req` arguments (units, ranges, wording of rules/schedules/timers).
- `src/settings.ts`, `src/storage.ts` / `storage.web.ts` — remembering the hub
  address and token (OS keychain on a phone, `localStorage` in a browser).
- `src/hooks/`, `src/ui/`, `App.tsx` — the screens: connect, Devices (device buttons, a sheet
  on a long press, brightness, live values, rename, scenes bar, add/identify/remove a Zigbee device),
  Automations (scenes, schedules, timers, rules — each of the last three can apply a
  scene), History (what the hub did and saw, newest first; tap a row for one device), Hub (chip health).
- `tools/mock-hub/` — a software hub speaking the protocol (3 clients max, hello
  timeout, lockout, validation, re-broadcast), for development without hardware.
- `tools/probe.ts`, `tools/hub-check.ts` — talk to a real hub from a terminal.
- `src/protocol/mdns.ts`, `tools/discover.ts` — find hubs on the LAN (mDNS `_keystone._tcp`).
- `src/ui/ScenesBar.tsx`, `SceneForm` in `AutomationForms.tsx` — scenes (named groups of settings): apply with one tap on the Devices tab, create from the current state in Automations, edit them in place (`SceneEditor`).
- `src/model/history.ts`, `src/hooks/useHistory.ts`, `src/ui/HistoryScreen.tsx` — the History tab: pages of the hub's event ring (`history`, docs/app-protocol.md §5.1) merged into one list, each event said in words (a command says who asked: app, rule, schedule …); new events are pushed by the hub (`type: "history"`) and folded in at once, with a 4 s poll only for a hub that does not push (30 s safety poll otherwise).
- `src/ui/AddDeviceSheet.tsx` — "Add a device" (a bottom sheet: opens the Zigbee network for two
  minutes, counts down, lists what joined).
- `src/ui/DeviceWidget.tsx` — a device as a button in the grid: the icon, the name, what it is
  doing right now; tapping it turns the device on or off (there is no separate switch), a long
  press opens the sheet. Three shapes, chosen from the data (`src/model/grid.ts`): a wide one
  (a switchable device), a square one (a sensor with no switch) and a tall one (two or more
  readings); the grid is three columns, so a button with little to say takes one cell, not
  half the screen. While the finger rests, the button dims at once and lights up amber after
  250 ms — that is the app counting the long press. `src/ui/DeviceSheet.tsx` — everything else
  about one device: on/off, meters, brightness, colour, readings, rename, blink, remove.
- `src/ui/Sheet.tsx` — the bottom sheet; `src/hooks/useKeyboardInset.ts` — how much of the
  screen the keyboard takes (measured by hand: react-native-web's `KeyboardAvoidingView`
  does nothing), so the sheet rides up over the keyboard and never covers the field being
  typed into.
- `src/model/names.ts` — the rules for a human-readable device name (up to 15 characters,
  spaces and Cyrillic allowed, nothing that looks like an id); `src/model/rooms.ts` — the room
  a device belongs to, taken from the beginning of its name.
- `src/ui/theme.tsx` — the theme: two palettes (`sandstone`, `ember`) and the spacing/radius
  scale. `src/palette.ts` — the palette as a setting of the phone (stored like the hub
  address); the picker lives on the Hub tab, card "Appearance".
- `src/ui/ConnectScreen.tsx` — the connect form in the room's language: a bar with "Back",
  glass cards (hubs on this network / connection / token / certificate), mode chips as pills, the
  eye inside the token field, a Connect button that comes alive when the address and the token are
  filled, and the failures under the form (wrong token, unreachable + Retry, untrusted certificate).
- `modules/keystone-net/` — the native Expo module (Kotlin + Swift): certificate-pinned WebSocket
  and mDNS browsing for phones; `src/native/` is its TypeScript side. **Never built on a device.**

## Design system

The visual source of truth is [`../docs/design/keystone-spec.html`](../docs/design/keystone-spec.html)
(palette, type scale, radii, spacing, every screen). The tokens live in `src/ui/theme.ts`
(`radius`, `space`) and are mirrored for designers in [`../docs/design/tokens.json`](../docs/design/tokens.json).
Shared building blocks are in `src/ui/controls.tsx`:

- `Button` — four kinds: `primary` (accent fill), `secondary` (quiet fill), `plain` (quiet text, no
  frame), `danger` (red frame); three sizes — `default`, `compact` (inside a card), `mini` (inside a list row).
- `Chip` — a pill for section tabs and choices (active: accent fill, 12.5 px / 700).
- `Input`, `Field`, `Picker`, `ErrorText` — form fields (radius 12), errors under the field.

Use these instead of one-off styles; a screen that needs its own shape asks for a token first.

## Commands

```bash
npm install
npm test                 # vitest: protocol, model, settings, and the UI rendered in jsdom
npm run typecheck
npm run mock-hub -- --port 8080 --token dev-token   # a fake home to point the app at
npm run web              # the app in a browser (expo start --web)
npm start                # Expo dev server (QR code for a phone with Expo Go / a dev build)
npm run export:web       # static web build into dist/

# against a real hub (set a token on its console first: `webtoken <token>`)
npm run discover         # lists hubs on the LAN with their URL (the hub advertises keystone.local)
npm run probe -- --url ws://<hub-ip>:8080/ws --token <token> [--cmd 256,1,false] [--seconds 30]
npm run hub-check -- --url ws://<hub-ip>:8080/ws --token <token>   # 19 conformance checks
# over TLS: pin the certificate (the hub console prints it with `webtoken`); adds 5 TLS checks
npm run hub-check -- --url wss://<hub-ip>:8443/ws --token <token> --fingerprint <sha256>
# without --fingerprint, `probe` on a wss:// URL just shows what the hub presents, to compare
# first-time setup mode (the hub's setup access point must be open: console `setup on`)
npm run setup-check -- --url wss://<hub-ip>:8443/ws --code <setup code> --token <token> --fingerprint <sha256>
#   --apply    also send setup.apply with the hub's current network (password in KS_SETUP_PASS) and the same token:
#              the whole hand-over runs for real (~20 s; every connection drops once)
#   --mutate   also create and remove a name, schedule, timer, rule and scene, and try pair/identify
#              (12 more checks: 31 in all, 36 over TLS). Puts everything back; never unpairs a device
#   --wifi     also move the hub to a network that does not exist and check it comes back on its own
#              (~45 s; the hub is unreachable for ~35 s of it — do not run it when you need the hub)
#   --hold N   keep 3 clients connected for N seconds (read `heap` on the hub meanwhile)
#   --lockout  also test the failed-login lockout (locks this machine out for a minute)
```

Node >= 22. On first start the app asks for the hub address (`192.168.0.202` is
enough) and the token.

## Notes

- The hub speaks `wss://` (its own self-signed certificate, pinned by fingerprint; protocol
  doc §10). In a **browser** open `https://<hub>:8443/` once and accept the certificate,
  and leave the fingerprint field empty (a page cannot pin). On a **phone** the stock
  WebSocket can neither pin nor accept a self-signed certificate; the pinned socket comes
  from the native module below (a development build, written but never run on a device).
  Until that is checked, a phone can fall back to `webplain on` on the hub (plain `ws://`,
  trusted network only), which may need cleartext traffic allowed (iOS ATS exception for
  local networking, Android `usesCleartextTraffic`).
- **Phones** need a development build (step by step for iOS: [../docs/ios-build.md](../docs/ios-build.md)), because the pinning module is native code (Expo Go
  cannot load it): `npx expo run:android` / `npx expo run:ios` (needs the Android SDK / Xcode)
  or an EAS build. The module has not been compiled or run by the author: expect to fix
  build errors, and test the three cases below before trusting it — (1) the right
  fingerprint connects, (2) a wrong one ends in the "untrusted" banner without the hub seeing a
  `hello`, (3) a hub whose certificate was regenerated is refused. For plain `ws://` development
  Android needs `usesCleartextTraffic`.
- Discovery is mDNS (`_keystone._tcp`, host `keystone.local`). It only says where the hub is;
  the fingerprint, not mDNS, is what you trust. The connect screen lists hubs on a native build;
  on the web (a browser cannot do mDNS) type the address, or the `keystone.local` name if your
  OS resolves it.
- The web build keeps the token in `localStorage`: fine for development, prefer the
  native app for daily use.
- `npm audit` reports vulnerabilities in the React Native / Expo build tooling; none
  of it ships in the app bundle, but keep the lockfile current.
- The icon set is the app's own (`src/ui/Glyph.tsx`); the design spec's icon sheet is generated
  from it (`node ../docs/design/icons/build-icons.mjs`), so the two cannot drift apart.
