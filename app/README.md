# keystone app

Client side of the hub: the protocol library, a mock hub and the Expo app (iOS,
Android and web from one codebase). The hub serves no UI; everything goes through
the WebSocket+JSON protocol in [../docs/app-protocol.md](../docs/app-protocol.md).

## Layout

- `src/protocol/` — wire types, the pure `reduce()` that applies hub messages, and
  `HubClient` (handshake + token, request/ack correlation, reconnect with back-off;
  works with `useSyncExternalStore`). No React, no Node-only APIs.
- `src/model/` — plain functions that turn protocol data into what people see
  (value formatting, device titles, chip health, error messages) and what people
  type into `req` arguments (units, ranges, wording of rules/schedules/timers).
- `src/settings.ts`, `src/storage.ts` / `storage.web.ts` — remembering the hub
  address and token (OS keychain on a phone, `localStorage` in a browser).
- `src/hooks/`, `src/ui/`, `App.tsx` — the screens: connect, Devices (switches,
  brightness, live values, rename), Automations (schedules, timers, rules), Hub
  (chip health).
- `tools/mock-hub/` — a software hub speaking the protocol (3 clients max, hello
  timeout, lockout, validation, re-broadcast), for development without hardware.
- `tools/probe.ts`, `tools/hub-check.ts` — talk to a real hub from a terminal.

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
npm run probe -- --url ws://<hub-ip>:8080/ws --token <token> [--cmd 256,1,false] [--seconds 30]
npm run hub-check -- --url ws://<hub-ip>:8080/ws --token <token>   # 19 conformance checks
#   --mutate   also create and remove a name, schedule, timer and rule (7 more checks)
#   --lockout  also test the failed-login lockout (locks this machine out for a minute)
```

Node >= 22. On first start the app asks for the hub address (`192.168.0.202` is
enough) and the token.

## Notes

- The link is plain `ws://` until the TLS stage (see the protocol doc, §10). A phone
  build may need cleartext traffic allowed for that (an iOS ATS exception for local
  networking, Android `usesCleartextTraffic`); not yet tried on a device.
- The web build keeps the token in `localStorage`: fine for development, prefer the
  native app for daily use.
- `npm audit` reports vulnerabilities in the React Native / Expo build tooling; none
  of it ships in the app bundle, but keep the lockfile current.
- Device icons are emoji, so a system without an emoji font shows empty boxes.
