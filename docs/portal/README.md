# keystone plugin developer portal

Everything a plugin author needs, in reading order.

| # | Chapter | What you'll learn |
|---|---|---|
| 1 | [Quick start](./01-quick-start.md) | Scaffold, build and install a plugin in ~5 minutes |
| 2 | [Manifest reference](./02-manifest.md) | Every field of `plugin.yaml` |
| 3 | [Go SDK](./03-sdk.md) | Implement `ports.Adapter`; ship a `main.go` in ~80 lines |
| 4 | [UI runtime layers](./04-ui-layers.md) | Pick between JSON schema, wizard, Web Component, iframe |
| 5 | [Publishing](./05-publishing.md) | Sign, upload, promote |
| 6 | [Security model](./06-security.md) | Trust anchors, permissions, TUF, sandboxing |

## Design docs (for the curious)

The chapters above are the "how-to". If you want the "why", start here:

- [`plugin-store-architecture.md`](../plugin-store-architecture.md) — the whole architecture in one place
- [`plugin-sdk-guide.md`](../plugin-sdk-guide.md) — deep dive on the SDK
- [`plugin-ui-integration.md`](../plugin-ui-integration.md) — the four UI layers, in full
- [`sidecar-protocol-v1.md`](../sidecar-protocol-v1.md) — the wire between core and plugin

## Reference implementations

Two plugins live in-tree that exercise the whole stack end-to-end.
Reading their code is the fastest way to internalise the SDK:

- [`plugins/matter/`](../../plugins/matter/) — Matter over a Node.js
  sidecar. Multi-endpoint devices, BLE commissioning, WebRTC cameras,
  realtime state stream.
- [`plugins/dirigera/`](../../plugins/dirigera/) — IKEA DIRIGERA over
  HTTPS + WebSocket. Small, single-file, uses the whole
  `sdk.Options` surface without leaning on the Matter-specific
  helpers.

The scaffold you get from `keystone-plugin new` matches the shape
`plugins/dirigera` uses, so if you paste that plugin into a fresh
scaffold, everything wires up.
