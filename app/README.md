# keystone app

Client side of the hub: the protocol library, a mock hub and (later) the Expo app
for iOS, Android and web. The hub serves no UI; everything goes through the
WebSocket+JSON protocol in [../docs/app-protocol.md](../docs/app-protocol.md).

## Layout

- `src/protocol/messages.ts` — wire types, frame parser (no dependencies).
- `src/protocol/store.ts` — `HomeState` and the pure `reduce()` that applies hub messages.
- `src/protocol/client.ts` — `HubClient`: handshake, token auth, request/ack
  correlation, reconnect with back-off. Works with `useSyncExternalStore`
  (`subscribe` / `getSnapshot`). Uses the platform's global `WebSocket`, or an
  injected `socketFactory`.
- `tools/mock-hub/` — a software hub speaking the protocol (3 clients max,
  hello timeout, lockout after failed auth, validation, re-broadcast).

## Commands

```bash
npm install
npm test            # vitest: reducer + client against the mock hub over real sockets
npm run typecheck
npm run mock-hub -- --port 8080 --token dev-token   # live fake home for UI work

# against a real hub (set a token on its console first: `webtoken <token>`)
npm run probe -- --url ws://<hub-ip>:8080/ws --token <token> [--cmd 256,1,false] [--seconds 30]
npm run hub-check -- --url ws://<hub-ip>:8080/ws --token <token>   # 20 conformance checks
```

Node >= 22 (uses the built-in `WebSocket` in one test and for ad-hoc scripts).
