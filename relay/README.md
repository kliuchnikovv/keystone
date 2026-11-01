# Keystone relay (Go)

The server side of remote access: the hub dials out to it, a phone or browser connects to it,
and it pairs the two. Nothing else has to run in the house — see
[../docs/app-protocol.md](../docs/app-protocol.md) §13 for the design and
[../docs/relay-deploy.md](../docs/relay-deploy.md) for putting it on a VPS.

Standard library only: `go.mod` has no `require` block, so `go build` needs no network and
the result is one static binary.

```bash
go test ./...
CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o keystone-relay .
./keystone-relay --secret "$(openssl rand -hex 32)"
```

## Endpoints

| Path | Who | What |
|------|-----|------|
| `/tunnel/<hub-id>` | the hub | opens the long-lived connection; first frame `!auth:<secret>` |
| `/client/<hub-id>` | a phone, a browser, `npm run console` | one connection per user |
| `/` | anyone | plain-text status: hubs and their client counts |

## Flags

| Flag | Default | Notes |
|------|---------|-------|
| `--secret` / `$SECRET` | — | required, ≥ 16 characters; `openssl rand -hex 32` |
| `--port` | `9090` | |
| `--tls-cert`, `--tls-key` | — | serve `wss://` directly; without them the relay expects TLS in front (Caddy/nginx) |
| `--max-clients` | `16` | backstop against a flood — the hub itself allows 3 authenticated clients and answers `busy` |
| `--ping-interval` | `30s` | keepalive ping; `0` disables |
| `--read-timeout` | two ping intervals + 10 s | a connection that sends nothing for this long is dropped |
| `--auth-timeout` | `10s` | how long a tunnel has to send `!auth` |
| `--max-message` | `1 MiB` | the hub's frames are at most 4 KB |

## Behaviour worth knowing

- **The relay never reads a payload.** Auth is the hub's own token, end to end; a leaked
  shared secret only lets someone attach a tunnel, not into the home.
- **A client is held until `!ready`.** Frames a client sends before the hub says `!ready` are
  queued, so the hub never sees a hello before it has the client in its table. The other
  direction is not held: the hub's answer to a refused client must arrive even though that
  client will never be `ready`.
- **Frames keep their opcode.** A binary frame is forwarded, not judged, so the hub can
  refuse it.
- **A second tunnel for the same id replaces the first** and drops its clients: a hub that
  reconnected must find its clients gone rather than split across two sockets.
- **Nothing is stored.** Hubs and clients live in memory; a restart drops everyone and the
  hub reconnects by itself within ~10 s.

## Layout

| File | What |
|------|------|
| `main.go` | flags, TLS, routing, status page |
| `relay.go` | the wire protocol and the hub/client pairing |
| `internal/ws` | RFC 6455 server: handshake, framing, fragmentation, ping/pong, close |
| `internal/wstest` | an independent client, used by the tests |

`app/tools/relay-server.ts` speaks the same protocol in Node. It stays the readable reference
and the one the app-side tooling is developed against; this one is what you deploy.