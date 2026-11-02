# Deploying the relay (remote access without anything else in the house)

The hub dials out to a relay server over WebSocket. Nothing else has to run in the home:
no tunnel agent, no VPN, no extra box — just the hub's own Wi-Fi.

    phone / browser  ──wss──▶  relay server (this document)
                                    ▲
                                    │ wss, one long-lived connection, opened by the hub
                                    │
                               the hub (brain)

The relay is a dumb multiplexer: it pairs each remote client with a connection to the hub and
forwards frames without reading them. Authentication stays end to end — the hub's own token
guards the app protocol, so a leaked relay secret does not give anyone the home.

One implementation, in Go (`relay/`): one static binary, no runtime and no node_modules beside
it — it is what gets deployed on a VPS.

| | |
|---|---|
| build | `CGO_ENABLED=0 go build ./...` (or `scripts/build-relay.sh`) |
| tests | `go test ./...` (29 cases) |

The Node implementation that used to live in `app/tools/relay-server.ts` was removed; the hub
has no idea which server it is talking to, so the protocol is the same either way.

## 1. What you need

- A VPS with a domain name whose DNS `A`/`AAAA` points at it. The hub validates the relay's
  certificate against the public CA bundle, so a self-signed certificate will not do. TLS is
  terminated in front of the relay (see §3).
- Nothing else: `scp` one binary, or build it from Docker.

## 2. Run it

```bash
scripts/build-relay.sh                     # build it first, see relay/README.md
scp build/relay root@relay.example.com:/usr/local/bin/relay
ssh root@relay.example.com

sudo install -m 600 /dev/null /etc/keystone-relay.env
printf 'RELAY_SECRET=%s\nPORT=9090\n' "$(openssl rand -hex 32)" | sudo tee /etc/keystone-relay.env >/dev/null
sudo cat /etc/keystone-relay.env        # this RELAY_SECRET goes into the hub

sudo install -m 644 <repo>/scripts/keystone-relay.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now keystone-relay
curl -s http://127.0.0.1:9090/            # "Keystone Relay / 0 hub(s) connected"
sudo journalctl -u keystone-relay -f
```

Without systemd, Docker works too (see `relay.Dockerfile` in the root of the repository):

```bash
docker run -d --name keystone-relay --restart unless-stopped \
  -e RELAY_SECRET="$(grep RELAY_SECRET /etc/keystone-relay.env | cut -d= -f2)" -p 9090:9090 keystone-relay
```

## 3. TLS in front

```bash
sudo install -m 644 scripts/Caddyfile /etc/caddy/Caddyfile   # edit the domain name
sudo systemctl reload caddy
```

Caddy fetches and renews the certificate by itself. The relay itself speaks plain HTTP: the
certificate has to come from a public CA, because the hub verifies it against the CA bundle.

The hub connects to `wss://<domain>/tunnel/<hub-id>`; a phone connects to
`wss://<domain>/client/<hub-id>` (that is also what the app's **Relay** mode asks for).

## 4. Point the hub at it

On the hub's console (USB or the browser app on the LAN):

```
relay wss://relay.example.com/tunnel/home s3cr3t-from-etc-keystone-relay-env
relay
```

Credentials live in NVS (`relay_url`, `relay_sec`), the tunnel starts by itself after every
reboot, and `relay clear` removes them. Nothing about the relay travels over mDNS, so a client
has to be told the address once (or typed into the app's Relay mode).

## 5. Check it

```bash
# on the VPS
curl -s http://127.0.0.1:9090/                    # 1 hub(s) connected
sudo journalctl -u keystone-relay -f             # tunnel / client events

# from anywhere, through the relay
cd app
npm run console -- --url wss://relay.example.com/client/home --token <hub token>
npm run hub-check -- --url wss://relay.example.com/client/home --token <hub token> --mutate
```

The hub console shows what it is doing with `relay`: tunnel state, remote client count, auth
failures in the window and how many outbound frames were dropped.

## Operating notes

- **Rotating the shared secret.** `relay <url> <new-secret>` on the hub; restart the relay with
  the new value. Two hubs cannot share a secret unless they use different `<hub-id>` paths.
- **One secret, many hubs.** `<hub-id>` is only a path segment; the secret is what
  authenticates. Use a separate secret per hub if you run more than one.
- **Better: a master key.** With `RELAY_MASTER_SECRET` the relay derives the secret of every
  hub itself (`HMAC-SHA256(master, "hub:<id>")`), and a box flashed with
  `scripts/gen-hub-relay.sh` carries exactly that secret. Adding a home is then a flash of
  the box and nothing on the server — no redeploy, no list of secrets to keep.
- **Firewall.** Only 80/443 need to be open. The relay itself listens on 127.0.0.1:9090.
- **The relay keeps no state.** Hubs and clients live in memory; a restart drops everyone and
  the hub reconnects by itself within ~10 s.
- **Throughput.** The tunnel carries JSON frames up to 4 KB; a full home snapshot arrives in
  parts over a few main-loop ticks.
- **Testing on the LAN instead of a VPS.** Run the relay on a machine the hub can reach and
  point the hub at `ws://<lan-ip>:9090/tunnel/<id>`; everything except TLS is identical.