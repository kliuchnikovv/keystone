# Keystone relay (Go)

The server side of remote access: the hub dials out to it, a phone or browser connects to it,
and it pairs the two. Nothing else has to run in the house — see
[../docs/app-protocol.md](../docs/app-protocol.md) §13 for the design,
[../docs/relay-deploy.md](../docs/relay-deploy.md) for putting it on a VPS and
[../docs/relay-security.md](../docs/relay-security.md) for the threat model.

One static binary for a VPS, no runtime beside it. The only dependency is
`github.com/coder/websocket` (`go mod download` once) — the WebSocket server itself.

## The wire protocol

Text frames between the relay and a tunnel:

```
!auth:<secret>   tunnel → relay, first message
!ok              relay → tunnel, auth accepted
!open:<cid>      relay → tunnel, a client connected
!ready:<cid>     tunnel → relay, the client may be forwarded to
!close:<cid>     either direction, the connection is gone
!error:<msg>     relay → tunnel
<cid>:<payload>  either direction, an app-protocol frame for client <cid>
```

The relay never looks inside a payload: the hub's own token guards the app protocol end to end,
so a leaked relay secret only lets someone attach a tunnel, not into the home.

## Endpoints

| Path | Who | What |
|------|-----|------|
| `/tunnel/<hub-id>` | the hub | the long-lived connection; the first frame is `!auth:<secret>` |
| `/client/<hub-id>` | a phone, a browser, `npm run console` | one connection per user |
| `/` | anyone | status page: counters — and, with a status token, the hub ids |
| `/healthz` | anyone | `ok`, for a health check |

## Build and test

```bash
go test ./...            # 29 cases: the policy and the protocol end to end
CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o relay .
```

`scripts/build-relay.sh [native|linux/amd64|linux/arm64|linux/arm/v7]` does the same and puts
the binaries in `build/`. A container: `docker build -f relay.Dockerfile -t keystone-relay .`
from the root of the repository.

## Flags

| Flag | Default | Notes |
|------|---------|-------|
| `--port` | `9090` (or `$PORT`) | |
| `--secret` | — | the shared secret; `$RELAY_SECRET` when the flag is absent |
| `--max-clients` | `3` | remote clients per hub — the hub itself allows 3 and answers `busy` |
| `--max-hubs` | `64` | hubs this relay serves |

## Environment

| Variable | What |
|----------|------|
| `PORT` | the port to listen on |
| `RELAY_SECRET` | one secret for every hub |
| `RELAY_MASTER_SECRET` | the key every hub's secret is derived from: `HMAC-SHA256(master, "hub:<id>")`. A new home needs nothing on the relay |
| `RELAY_HUB_SECRETS` | `id:secret,id2:secret2` — a secret for the hubs that have one of their own |
| `RELAY_STATUS_TOKEN` | with it the status page names the hubs, without it only counters |
| `RELAY_ALLOWED_ORIGINS` | comma-separated browser origins; the relay's own origin is always allowed |

A hub flashed with `scripts/gen-hub-relay.sh` carries its own derived secret and registers on
its own, so adding a home is a flash and nothing else. The relay never learns which homes it
serves unless the status token is set.

## Behaviour worth knowing

- **A client is held until `!ready`.** Frames a client sends before the hub says `!ready` are
  queued, so the hub never sees a hello before it has the client in its table. The other
  direction is not held: the hub's answer to a refused client must arrive even though that
  client will never be `ready`.
- **Frames keep their opcode.** A binary frame is forwarded, not judged, so the hub can refuse it.
- **A second tunnel for the same id replaces the first** and drops its clients: a hub that
  reconnected must find its clients gone rather than split across two sockets.
- **Nothing is stored.** Hubs and clients live in memory; a restart drops everyone and the hub
  reconnects by itself within ~10 s.

## The security policy (`policy.go`)

- the secret is compared in constant time, hashed first, so neither its length nor the place
  of the first difference leaks;
- five failed `!auth:` from one source block that source for a minute;
- a browser origin is refused unless it is listed in `RELAY_ALLOWED_ORIGINS`; the relay's own
  origin is allowed, because the iOS app (SocketRocket) sends it;
- a hub id must be boring: `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`;
- the limits: 64 KB a frame, 64 frames / 256 KB waiting for the tunnel, 3 clients a hub, 64 hubs.

## Layout

| File | What |
|------|------|
| `main.go` | flags, environment, the HTTP server |
| `server.go` | the sockets, the tunnels, the status page |
| `policy.go` | the security policy: secrets, origins, limits, the rate limit |
| `policy_test.go`, `integration_test.go` | the tests |

The Node implementation that used to live in `app/tools/relay-server.ts` was removed: this Go
server is the one that gets deployed, and the app-side tools speak the same protocol.
