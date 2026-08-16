# UI runtime layers

Four independent layers, mix freely per plugin. Pick the smallest
one that gets the job done — anything higher is more work for you
and more surface for a bug to live in.

| Layer | Effort | You ship | Right when |
|---|---|---|---|
| 1 | ~none | `spec.config.schema` JSON Schema | The setup is a settings screen |
| 2 | small | `Options.ConfigFlow` handler in Go | Setup is a wizard (OAuth, discover-then-pick) |
| 3 | medium | A JS Web Component under `ui/` | You need a custom device detail widget |
| 4 | large | A full HTML app under `ui/` | You have an existing React/Vue/three.js UI |

## Layer 1 — JSON Schema

`spec.config.schema` in `plugin.yaml` is a JSON Schema string. The
store UI renders a form for it, `PUT /plugins/<name>/config`
validates against it, and your plugin reads the result with
`sdk.LoadConfig`.

Formats supported:
- `string` (default text) + `format: password` / `format: textarea`
- `integer` / `number` (`minimum`, `maximum`)
- `boolean` → checkbox
- `enum` on any type → select
- `object` with `properties` → nested group
- everything else falls back to raw-JSON textarea

Example: `plugins/dirigera/plugin.yaml`.

## Layer 2 — Declarative Config Flow

The setup wizard. Your plugin exposes a handler
`(ctx, req) → *ConfigFlowStep` via `Options.ConfigFlow`. The store UI
POSTs `/plugins/<name>/flow` on the Wand icon and walks the flow.

State lives in your plugin — the client sends `{step: <lastNextId>,
data: <user submission>}` on each turn. Nine step types:

| type | What it renders | Fields |
|---|---|---|
| `info` | Read-only text + "Дальше" | `title`, `body`, `next` |
| `form` | JSON Schema auto-form | `title`, `schema` (Layer 1), `next` |
| `oauth` | External-link button + code paste | `authUrl`, `provider`, `next` |
| `qr-scan` | QR paste input | `qrHint`, `next` |
| `progress` | 0-1 progress bar | `progress`, `message` |
| `confirm` | Accept / decline | `next`, `cancel` |
| `pick-device` | Checkbox list | `field`, `options`, `next` |
| `manual-action` | Instruction + "Готово" | `instruction`, `next` |
| `error` | Red card + optional retry | `message`, `retry` |
| `complete` | Green check + close | `message` |

The complete list is in
[`plugin-ui-integration.md`](../plugin-ui-integration.md#43-step-types)
with per-type field-by-field detail.

## Layer 3 — Web Components

Ship a JS module under `ui/` that registers a custom element; the
store UI mounts it wherever the manifest binds it.

```yaml
ui:
  deviceDetail:
    - matchDeviceType: thermostat
      element: hello-thermostat
      source: thermostat.js
```

`element` must contain a hyphen (the WHATWG rule for custom
elements); the loader refuses names that could resolve to a
built-in tag like `<script>`.

Your module gets `window.keystone` injected — the same surface
Layer 4 iframes see. From it:

```js
class HelloThermostat extends HTMLElement {
  connectedCallback() {
    const id = this.getAttribute('device-id');
    window.keystone.devices.get(id).then((d) => this.render(d));
    this._unsub = window.keystone.subscribe(`state.${id}.**`, (evt) => {
      // evt is one push from /stream; update UI
    });
  }
  disconnectedCallback() { this._unsub?.(); }
}
customElements.define('hello-thermostat', HelloThermostat);
```

CSS custom properties (`--ks-color-text`, `--ks-color-accent`, …)
are inherited through shadow DOM, so your component picks up
keystone's theme automatically.

`window.keystone`:

| Method | HTTP verb |
|---|---|
| `devices.list()` | `GET /devices` |
| `devices.get(id)` | `GET /devices/{id}` |
| `devices.setState(id, feature, key, value)` | `POST /devices/{id}/state` |
| `devices.invoke(id, feature, action, params?)` | `POST /devices/{id}/actions` |
| `subscribe(topic, cb)` | fan-out from `/stream`, wildcards `*` and `**` |

`subscribe` returns an unsubscribe function; call it from
`disconnectedCallback` or you'll leak stream handlers.

Slots the manifest supports today:
- `ui.deviceDetail[]` — device detail page (matched by
  `matchDeviceType` or `matchFeature`)

Additional slots (`ui.deviceCard`, `ui.dashboardWidget`) are in
the design doc; the frontend wires the same loader when they land.

## Layer 4 — iframe embed

For plugins that need a full framework (React, Vue, three.js,
WebGL for local video decode). One entry:

```yaml
ui:
  embed:
    url: index.html
    title: Hello Dashboard
```

`/plugins/<name>/embed` renders an iframe pointing at
`/plugins/<name>/ui/index.html`. Your HTML includes the bridge
bootstrap:

```html
<script src="/ui/keystone-embed.js"></script>
<script>
  keystone.ready.then(() => {
    keystone.devices.list().then((r) => {
      // render whatever
    });
  });
</script>
```

`keystone-embed.js` installs `window.keystone` inside the iframe as
a postMessage proxy — same API as Layer 3, calls round-trip through
the parent frame. `keystone.ready` resolves once the parent has
said hello, so any code that runs before the handshake awaits.

Iframe sandbox is `allow-scripts allow-same-origin`. Cross-origin
messages are dropped; a subscription cap of 64 protects the parent
from a runaway plugin.

## Mixing layers

Real plugins pick the layer per surface:

- `plugins/matter/`: Layer 1 for `fabricLabel`, Layer 2 for the
  Matter commissioning wizard (setup code → BLE scan → Wi-Fi →
  attestation), no Layer 3/4 today because the built-in device
  UI covers everything.
- `plugins/dirigera/`: Layer 1 for the full config (`hubUrl`, `token`,
  TLS trust). No wizard because the pairing flow is
  physical-button-based.

If your plugin ends up with a Layer 4 iframe for one page, keep
the other pages on lower layers — the iframe is heavier for the
user (whole-frame reloads, focus quirks, no shared state store).
