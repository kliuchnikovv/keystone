# Go SDK

Your plugin is a Go binary. It implements the same `ports.Adapter`
interface the in-tree Matter adapter did, hands it to `sdk.Run`,
and the SDK does everything else: sidecar-protocol handshake, RPC
wire, event pump, config-flow proxy, error taxonomy.

Import path: `github.com/kliuchnikovv/keystone/plugins/sdk`.

## Minimum viable main.go

```go
package main

import (
    "context"
    "log/slog"
    "os"

    "github.com/kliuchnikovv/keystone/plugins/sdk"

    "github.com/you/plugin-hello/internal/hello"
)

func main() {
    log := slog.New(slog.NewJSONHandler(os.Stderr, nil))
    adapter := hello.NewAdapter(log)

    err := sdk.Run(context.Background(), sdk.Options{
        Name:         "hello",
        Version:      "0.1.0",
        Capabilities: []string{"transport.hello"},
        Adapter:      adapter,
        Logger:       log,
    })
    if err != nil {
        log.Error("plugin exited", "err", err)
        os.Exit(1)
    }
}
```

That's it. Every field of `sdk.Options`:

| Field | Purpose |
|---|---|
| `Name` | Must match `metadata.name` in `plugin.yaml`. The manager cross-checks at handshake. |
| `Version` | Announced in `hello`; matches `metadata.version`. |
| `Capabilities` | Advertised in `hello`. Must be a subset of `spec.capabilities`. |
| `Adapter` | Your `ports.Adapter` implementation. |
| `ErrorMapper` | Optional. Translates your errors into `sidecar.Error` codes (see below). |
| `ConfigFlow` | Optional. Enables the Layer 2 wizard. |
| `Logger` | Optional. Defaults to stderr JSON, info level. |

## The Adapter interface

`ports.Adapter` is the whole contract. All nine methods:

```go
type Adapter interface {
    Kind() domain.TransportKind
    Start(ctx context.Context) error
    Stop(ctx context.Context) error

    Discover(ctx context.Context) (<-chan ports.DiscoveredDevice, error)
    Commission(ctx context.Context, req ports.CommissionRequest) (domain.TransportRef, error)

    ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error)
    WriteState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) error
    InvokeAction(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error

    Subscribe(ctx context.Context) (<-chan ports.TransportEvent, error)
    Decommission(ctx context.Context, ref domain.TransportRef) error
}
```

You implement all nine. When any of them is not applicable, return
an error rather than a nil result — the SDK maps errors through
`sidecar.Error` codes so the core sees `feature.unsupported` or
`plugin.internal` as appropriate.

### Two optional interfaces

Implement these to unlock extra features. The SDK sees the type
assertion and registers the corresponding RPC handlers.

**`ports.CommissionableDiscoverer`** — a scan for devices ready to
pair.

```go
DiscoverCommissionable(ctx context.Context, window time.Duration) (<-chan ports.CommissionableDevice, error)
```

**`ports.CameraStreamer`** — WebRTC brokering.

```go
StartStream(ctx context.Context, ref domain.TransportRef, sdp string) (int, error)
AddCandidates(ctx context.Context, sessionID int, candidates []string) error
StopStream(ctx context.Context, sessionID int) error
Signals(ctx context.Context) (<-chan ports.CameraSignal, error)
```

## Events

`Subscribe` is the one method that returns a stream. Ship a
long-lived channel from your adapter:

```go
type Adapter struct {
    events chan ports.TransportEvent
}

func (a *Adapter) Start(ctx context.Context) error {
    a.events = make(chan ports.TransportEvent, 64)
    a.events <- ports.TransportEvent{Kind: ports.TransportEventAdapterStatus, Value: true}
    go a.streamRealtime(ctx)
    return nil
}

func (a *Adapter) Subscribe(_ context.Context) (<-chan ports.TransportEvent, error) {
    return a.events, nil
}
```

Kinds keystone knows about:
- `state_changed` — a device attribute changed. Set `Ref`, `Feature`,
  `Key`, `Value`.
- `event_fired` — a Matter-style event (button press, alarm). Same
  fields; `Key` is the event name.
- `online` / `offline` — a device went in or out of reach.
- `added` / `removed` — the device appeared or disappeared without
  going through Commission / Decommission.
- `adapter_status` — YOUR backend went up or down. `Ref` is empty,
  `Value` is a bool. Emit this whenever your connection to the
  hub changes state — the core uses it to distinguish "quiet
  house" from "the plugin is broken".

## Config flow (Layer 2)

Enable it by setting `Options.ConfigFlow` to a handler:

```go
sdk.Options{
    ...
    ConfigFlow: func(ctx context.Context, req bridge.ConfigFlowRequest) (*bridge.ConfigFlowStep, error) {
        switch req.Step {
        case "init":
            return &bridge.ConfigFlowStep{
                Type:  "info",
                Title: "Welcome",
                Body:  "This plugin talks to a Hello hub.",
                Next:  "credentials",
            }, nil
        case "credentials":
            return &bridge.ConfigFlowStep{
                Type:   "form",
                Title:  "Hub address",
                Schema: `{"type":"object","properties":{"host":{"type":"string"}}}`,
                Next:   "done",
            }, nil
        case "done":
            host, _ := req.Data["host"].(string)
            // save the config, spawn the connection…
            return &bridge.ConfigFlowStep{Type: "complete", Message: fmt.Sprintf("Connected to %s", host)}, nil
        }
        return nil, fmt.Errorf("unknown step %q", req.Step)
    },
}
```

Full list of step types with their fields is in
[`plugin-ui-integration.md`](../plugin-ui-integration.md#43-step-types).

## Config on the plugin side

The store UI writes to `PUT /plugins/{name}/config`, the manager
persists it under the plugin's data dir. Your plugin reads it on
startup with `sdk.LoadConfig`:

```go
type pluginConfig struct {
    HubURL string `json:"hubUrl"`
    Token  string `json:"token"`
}

func main() {
    ...
    var cfg pluginConfig
    if err := sdk.LoadConfig(&cfg); err != nil {
        log.Error("load config", "err", err)
        os.Exit(2)
    }
    if cfg.HubURL == "" || cfg.Token == "" {
        log.Error("missing hubUrl/token; set them via PUT /plugins/<name>/config")
        os.Exit(2)
    }
    ...
}
```

`LoadConfig` looks for `$KEYSTONE_PLUGIN_DATA/config.json`. A missing
file returns `nil` — first-run plugins land on defaults.

## Error mapping

Your adapter's errors need to arrive at the core with the right
`sidecar` code so the UI shows a useful message and `IsRetryable`
does the right thing. Pass an `ErrorMapper`:

```go
sdk.Options{
    ...
    ErrorMapper: func(err error) error {
        if errors.Is(err, hello.ErrUnauthorized) {
            return sidecar.Errorf(sidecar.CodeAuthInvalidCredentials, "%s", err.Error())
        }
        if errors.Is(err, hello.ErrNotFound) {
            return sidecar.Errorf(sidecar.CodeDeviceNotFound, "%s", err.Error())
        }
        return sidecar.Errorf(sidecar.CodeNetworkUnavailable, "%s", err.Error())
    },
}
```

Codes worth knowing:
- `sidecar.CodeValidationBadParams` — 4xx-shape; retrying won't help
- `sidecar.CodeDeviceNotFound` / `CodeDeviceOffline` / `CodeDeviceUnsupported`
- `sidecar.CodeFeatureUnsupported`
- `sidecar.CodePluginNotReady` / `CodePluginBusy` / `CodePluginInternal`
- `sidecar.CodeAuthInvalidCredentials` / `CodeAuthExpired`
- `sidecar.CodeNetworkTimeout` / `CodeNetworkUnavailable`
- `sidecar.CodePermissionDenied`

## Reference

Live code beats prose:
- [`plugins/matter/main.go`](../../plugins/matter/main.go) — 86
  lines. Everything above in production.
- [`plugins/dirigera/main.go`](../../plugins/dirigera/main.go) — 96
  lines. Different transport shape, same SDK.
