# keystone — smart home engine (POC)

Local-first, transport-agnostic smart home engine. Go core, one static binary,
sidecars for real transports (matter.js, zigbee2mqtt, IKEA DIRIGERA REST/SSE).

**Status:** POC day-5. Virtual adapter, JSON-file persistence, rules engine
(time / state / threshold / event triggers; time-range + state-equals
conditions; invoke_action + set_state + delay actions), NDJSON state-stream
endpoint. The demo scene seeds two virtual plugs plus a threshold-triggered
automation so the full pipeline can be exercised without any hardware.

## Quick start

```sh
make build
./bin/keystone                        # persists to ./keystone-data
```

In a second terminal:

```sh
# What's registered
curl -s localhost:7777/devices | jq

# What automations exist
curl -s localhost:7777/rules | jq

# Turn on the first device
DEV=$(curl -s localhost:7777/devices | jq -r '.devices[0].ID')
curl -sX POST localhost:7777/devices/$DEV/actions \
  -H 'content-type: application/json' \
  -d '{"feature":"onoff","action":"turn_on"}'

# Watch live state changes streaming
curl -N localhost:7777/stream

# Wait ~20s for the kettle sawtooth to cross 1500W, then:
curl -s localhost:7777/rules/runs | jq
# You should see status: "success" entries.
```

Restart the process — devices and rules survive because they live in
`./keystone-data/{devices,rules}.json`.

## Layout

```
cmd/keystone/            main.go — composition root
internal/domain/      pure types + polymorphic JSON for Rule/Trigger/Action
internal/ports/       interfaces (Adapter, EventBus, DeviceRepository)
internal/adapters/    transport implementations
  virtual/            in-memory, for tests & demos
internal/registry/    in-memory device catalogue + per-device locks
internal/eventbus/    in-process fan-out pub/sub
internal/service/     use-cases (device_service — commission, invoke, ingress)
internal/rules/       engine, triggers, conditions, actions
internal/storage/     persistence
  file/               JSON-file repo (POC) — SQLite drops in behind the same API
internal/api/ws/      NDJSON live state stream endpoint
```

## Roadmap

- [x] Domain model + ports
- [x] Virtual adapter
- [x] Registry with keyed locks
- [x] In-process event bus
- [x] Device service (List/Get/Invoke/Write) + ingress loop
- [x] HTTP admin (list / get / invoke action / write state)
- [x] JSON-file persistence for devices and rules
- [x] Rules engine (time / state / threshold / event triggers; time-range + state-equals conditions; invoke_action + set_state + delay actions)
- [x] Live state stream (NDJSON over long-lived HTTP)
- [ ] SQLite behind the same repo interface
- [ ] Proper WebSocket (gorilla) with reconnect
- [ ] DIRIGERA adapter (REST + SSE)
- [ ] gRPC API
- [ ] Matter adapter (via matter.js sidecar)

## Design notes

See `../docs/go-core-spec.md` for the full spec.

Key decisions:

- **Language:** Go 1.23+. One static binary.
- **Concurrency:** per-device lock (`registry.Lock(id)`) around every write path.
  Rules never race on the same device.
- **Transport isolation:** each adapter is a goroutine with its own subscribe
  channel. A crashed adapter cannot take the others down.
- **Storage:** SQLite for config, device catalogue, rules. Live state is
  in-memory only, rehydrated from adapters on startup.
- **Internal model:** our own; inspired by Matter data model but not bound to
  it. Matter is one transport among several.
