# Quick start

Scaffold, build, install, and enable a plugin against a running
keystone daemon. Everything below is real — no placeholders, no
"and then a miracle happens".

## Prerequisites

- Go 1.23+ (the same toolchain the core uses)
- A running keystone daemon reachable on `http://localhost:7777`
  (`go run ./cmd/keystone -plugins-dir ./data/plugins` from the
  keystone tree does the job)
- `keystone-plugin` on your PATH: `go install
  github.com/kliuchnikovv/keystone/cmd/keystone-plugin@latest`

## 1. Scaffold

```sh
keystone-plugin new \
  --name hello \
  --transport hello \
  --module github.com/you/plugin-hello
cd hello
```

You get a directory with a valid `plugin.yaml`, a `main.go` that
calls `sdk.Run`, and an `internal/hello/adapter.go` where every
`ports.Adapter` method is stubbed with "not implemented". The
scaffold's manifest passes validation out of the box — the plugin
has no capabilities beyond announcing itself.

## 2. Build

```sh
keystone-plugin build
```

Produces `bin/hello-plugin`. Under the hood this is `go build -o
bin/hello-plugin .` with the manifest read to pick the name — no
per-plugin Makefile needed.

## 3. Test

```sh
keystone-plugin test
```

Runs manifest validation (the same jsonschema check the manager
runs at Discover) and then `go test ./...`. Ship with `--skip-go-test`
if you don't have tests yet.

## 4. Install into a running daemon

```sh
keystone-plugin install \
  --dir . \
  --target /path/to/keystone-plugins-dir \
  --reload http://localhost:7777
```

`--reload` POSTs `/plugins/discover` on the daemon so the plugin
appears in the store without a daemon restart. Verify:

```sh
curl -s http://localhost:7777/plugins | jq
```

You should see `hello` in state `discovered`.

## 5. Enable

```sh
curl -X POST http://localhost:7777/plugins/hello/enable
```

The manager forks your binary, connects over a Unix socket, and
your adapter's `Start` fires. The daemon adds it as a `ports.Adapter`
under the transport kind declared in your manifest's
`spec.capabilities: [transport.hello]`.

Now open `http://localhost:7777/ui/` in a browser, hit the "Plugins"
button in the top-right, and the plugin card is there with its
Enable / Restart / Settings / Uninstall controls.

## Next

- Replace the "not implemented" stubs in `internal/hello/adapter.go`
  with something that actually talks to your device. Start with
  `Discover` and `ReadState`.
- Add a config schema so the operator can set up your plugin
  through the store UI. See [chapter 2](./02-manifest.md).
- If your transport streams events, wire them into the `events`
  channel from `Subscribe`. See [chapter 3](./03-sdk.md).

## What just happened

```
keystone-plugin new  →  scaffold on disk
keystone-plugin build → bin/<name>-plugin
keystone-plugin install --reload → tarball unpacked into plugins-dir,
                                    /plugins/discover POSTed
POST /plugins/<name>/enable → manager fork()s the binary, dials
                              the Unix socket, sdk.Run's handshake
                              completes, adapter is mounted into
                              service.DeviceService
```

The core still speaks `ports.Adapter` — your plugin is one now,
behind the sidecar.
