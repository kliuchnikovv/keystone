# keystone-matter-server

Node.js sidecar that runs matter.js and exposes a small JSON-RPC surface
over WebSocket for the Go core to drive. See
[`docs/matter-adapter-brief.md`](../../docs/matter-adapter-brief.md) §4
for the product context.

## Layout

- `src/protocol.ts` — wire types, byte-compatible with
  `internal/adapters/matter/protocol.go` on the Go side.
- `src/controller.ts` — thin facade over matter.js. The real API wiring is
  marked `TODO(matter-real)` — each stub method documents the exact
  matter.js ≥0.18 call needed to fill it in.
- `src/wsServer.ts` — WebSocket JSON-RPC dispatcher, event fan-out to
  all connected clients.
- `src/index.ts` — entry point.

## Configuration

All via environment variables:

| Var | Default | Meaning |
|---|---|---|
| `KEYSTONE_MATTER_HOST` | `0.0.0.0` | Bind address |
| `KEYSTONE_MATTER_PORT` | `5580` | WS listen port |
| `KEYSTONE_MATTER_STORAGE` | `./matter-data` | Fabric-state directory |
| `KEYSTONE_MATTER_LABEL` | `Keystone` | Fabric label shown in device apps |

## Local dev

```bash
cd sidecars/matter-server
npm install
npm run build
npm start
```

Or via Docker Compose from the repo root:

```bash
docker compose up matter-sidecar
```

Compose runs the container with `network_mode: host` — required for
Matter's mDNS discovery to work at all.

## Testing the wire

Once the process is up you can drive it directly with `wscat`:

```bash
wscat -c ws://localhost:5580
> {"id":"1","method":"listNodes"}
< {"id":"1","result":[]}
```

## Status

Phase 1 skeleton (see brief §6). The WebSocket layer, JSON-RPC dispatch,
and event fan-out are done. `controller.ts` currently throws
`not implemented` on the matter.js-backed methods — every stub carries
the exact upstream call in a code comment, tagged `TODO(matter-real)`.
Follow-up work: land those calls against a live `@matter/main` install
and pin the version once a WARMBLIXT commissioning round-trip passes.
