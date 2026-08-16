# Manifest reference

Every plugin ships one `plugin.yaml` at its root. This chapter lists
every field the validator accepts, with the shape and the meaning.

The canonical schema is `internal/plugin/manifest_schema.json` — it
is embedded into every keystone binary and used to validate at
Discover time. If a field here disagrees with the schema, the
schema wins.

## Top-level shape

```yaml
apiVersion: keystone.plugin/v1   # required, exact string
kind: Plugin                     # required, exact string
metadata: { ... }                # required
spec:     { ... }                # required
ui:       { ... }                # optional; see chapter 4
```

## metadata

```yaml
metadata:
  name: dirigera                          # required, matches ^[a-z][a-z0-9-]{1,38}[a-z0-9]$
  displayName: IKEA DIRIGERA              # required
  version: 0.1.0                          # required, semver
  description: |                          # required, one paragraph
    Direct LAN control of an IKEA DIRIGERA hub.
  maintainer: you <you@example.com>       # required
  license: Apache-2.0                     # required, SPDX id
  homepage: https://…                     # optional
  category: transport                     # required, one of: transport | device | automation | ai | ui | utility
  tags: [ikea, dirigera, zigbee]          # optional
  trustTier: verified                     # required, one of: core | verified | experimental | unsafe
```

`name` is the store-facing slug **and** the filesystem name the
manager installs into. The regex forbids leading dots and slashes;
the loader rechecks even after schema-validated manifests to defend
against downgrade attacks.

`trustTier` is a badge, not a check. Setting `verified` doesn't
verify anything — the registry curator does, and the store UI
displays whatever the manifest says.

## spec

```yaml
spec:
  keystoneCoreMin: "0.5.0"                # required
  keystoneCoreMax: "1.x"                  # optional

  capabilities:                            # required, at least one entry
    - transport.dirigera                   # one transport.* per plugin (see below)
    - discover.mdns
    - realtime.state-stream

  permissions:                             # optional
    - network.lan
    - secretstore.read: [dirigera.token]

  resources:                               # optional
    memory: 256Mi
    cpu: 100m
    disk: 500Mi

  entrypoint:
    exec: bin/dirigera-plugin              # required, relative to plugin dir
    args: ["--verbose"]                    # optional
    env:                                   # optional; expanded with $KEYSTONE_PLUGIN_DATA
      KEYSTONE_DIRIGERA_HUB: "10.0.0.5"

  sidecars:                                # optional; each spawned by manager
    - name: my-runtime
      exec: sidecars/my-runtime/dist/index.js
      runtime: node20                      # node* / python* / static
      env:
        MY_DATA: "$KEYSTONE_PLUGIN_DATA/state"
      restart: on-failure                  # always | on-failure | never

  config:                                  # optional; drives the settings UI
    schema: |                              # JSON Schema string
      {
        "type": "object",
        "required": ["hubUrl", "token"],
        "properties": {
          "hubUrl": {"type": "string"},
          "token": {"type": "string", "format": "password"}
        }
      }
```

### capabilities

Dotted strings the core reads to know what your plugin does. The
one that matters most today is `transport.<slug>` — exactly one per
plugin. `<slug>` is the value `Adapter.Kind()` returns and the
identifier `/devices/commission?transport=<slug>` uses.

Other conventions:
- `discover.<method>` — how your plugin finds devices (`mdns`,
  `ble`, `zwave`, `passive`).
- `realtime.state-stream` — you push state changes rather than
  requiring polling. The core uses this today for adapter status
  hints.
- `commission.<method>` — pairing capabilities (`setup-code`,
  `qr-code`, `button-press`).

Unknown capabilities are accepted; they only affect the store's
description page.

### permissions

Advisory today, enforced later. Two spellings:

```yaml
permissions:
  - network.lan          # bare identifier
  - secretstore.read: [token, hostname]   # scoped
```

Vocabulary the store cares about:
- `network.lan` — talks on the local network
- `network.multicast` — sends multicast
- `network.internet` — reaches out through the WAN
- `storage.persistent` — writes state that survives restarts
- `secretstore.read` / `secretstore.write` — reads/writes the
  keystone secret store; scope is the list of secret keys

### resources

k8s-flavoured footprint declaration. Today they are ignored by the
supervisor; when sandboxing lands (cgroups on Linux) they become the
hard limits.

### entrypoint

The path is **relative** to the plugin's install directory. Absolute
paths are refused at Discover time (defense-in-depth against a
malicious tarball that tries to run `/bin/sh`).

`env` values pass through `os.Expand`, so `$KEYSTONE_PLUGIN_DATA` and
`$KEYSTONE_PLUGIN_NAME` land as concrete paths. Any other reference
falls through to the process env.

### sidecars

Manifest-declared side processes the manager supervises. `runtime`
tells the supervisor how to launch:
- `static` (or omitted) — exec the file directly
- `node20`, `node22` — prepends `node`
- `python3.11`, `python3.10` — prepends `python3`
- anything else — treated as static

`restart` picks the policy. On-failure is the sane default; `never`
is right for one-shot migrations.

### config.schema

A JSON Schema string. The keystone daemon validates every `PUT
/plugins/{name}/config` body against it and the store UI uses it to
render an auto-form (see chapter 4, Layer 1). Keep the schema
small — one screen's worth of fields is right.

Formats keystone's SchemaForm knows about:
- `format: password` → password input
- `format: textarea` → multi-line
- everything else → text / number / checkbox / select

## ui (optional)

Four layers, mix freely; see [chapter 4](./04-ui-layers.md).

```yaml
ui:
  # Layer 1 is implicit — spec.config.schema is enough for a
  # simple settings screen. Nothing extra here.

  # Layer 2 — the wizard is enabled by an SDK-side handler, not a
  # manifest field. Users hit the ✨ icon in the plugin card and
  # the core POSTs /plugins/<name>/flow to your process.

  # Layer 3 — declare Web Components the core renders on device
  # detail pages.
  deviceDetail:
    - matchDeviceType: thermostat
      element: hello-thermostat
      source: thermostat.js

  # Layer 4 — a full custom UI in an iframe.
  embed:
    url: index.html
    title: Hello Dashboard
```

The `ui:` block is stored as an opaque object today — the schema
does not enforce its shape. The frontend reads it directly, so a
typo goes silent rather than failing at Discover. Copy-paste from
`plugins/matter/plugin.yaml` or `plugins/dirigera/plugin.yaml` for
a working template.
