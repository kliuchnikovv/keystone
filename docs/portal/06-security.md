# Security model

A keystone plugin runs in a separate process, but it is still your
code executing on someone else's machine. The security controls
sit at three points: getting the plugin, running the plugin,
letting the plugin do things.

## Getting the plugin: install-time verification

Everything installed through the store goes through this pipeline:

```
                registry URL
                     │
        scheme + host check (SSRF gate)
                     │
             HTTP GET index.json
                     │
        resolve version, GET packages/…
                     │
              sha256 must match
                     │
        Verifier.Verify(data, package)    ← where signing plugs in
                     │
        pack.UnpackTarball (path safety)
                     │
         plugin lands in plugins-dir
```

Each step is a hard fail; the daemon does not proceed on any
error.

### SSRF gate

- Base URL must be `https://`. `http://` is only allowed for
  loopback hosts.
- Every dial re-resolves the host and refuses private / link-local
  / unspecified IPs. Loopback is refused unless the base URL was
  itself loopback — that defeats DNS rebinding against a public
  host.
- `manager.Options.TrustedRegistries` is an exact-match allowlist.
  Empty means "no allowlist" (the scheme+host check is still the
  gate); production deployments fill it.

### Signature verification (chapter 5)

`store.Verifier` is the extension point. Three implementations ship:

- `SHA256Verifier` (default) — sha256 already checked, so this is
  a no-op. Fine for trusted internal registries.
- `Ed25519Verifier` — a base64 detached signature over sha256(data)
  under a set of trusted ed25519 public keys. Small, no ceremony.
- `SigstoreVerifier` — Fulcio-issued certificate chain + SAN
  identity allowlist + signature over the artifact digest, plus
  optional Rekor SET/inclusion verification. This is the one for
  public plugins; the trust bundle is refreshed via TUF.

Wire it in via `manager.Options.PluginVerifier`. Nil keeps the
sha256-only path.

### Path safety

`pack.SafePluginName` refuses `metadata.name` values that could
escape (`..`, slashes, leading dot). `pack.UnpackTarball` refuses
tarball entries with absolute or `..`-containing paths. The `ui/`
static server on the daemon side additionally resolves symlinks
before its containment check, so a plugin dir with a symlink
inside `ui/` cannot serve `/etc/passwd`.

Every check is fail-closed — a check that errors is treated as a
rejection.

## Running the plugin: sandboxing

Today's story is honest: the plugin runs as an ordinary child
process under the daemon's user. Isolation is:

- **Separate process.** A panic or leak in the plugin cannot
  crash the core.
- **Unix-socket-only IPC.** Nothing on the sidecar wire lets the
  plugin fabricate calls; the peer at the other end is the core,
  not the plugin.
- **Own data dir.** `$KEYSTONE_PLUGIN_DATA` is scoped per plugin.

What is not landed:
- cgroups / rlimits enforcement of `spec.resources`.
- No-network / read-only-fs sandboxing.
- User namespace mapping so a plugin runs under an unprivileged
  UID.

These belong under `[Plugin] resources: cgroup enforcement` on
the backlog. Until then, running third-party plugins requires the
same trust as running an npm package.

## Letting the plugin do things: permissions

The manifest's `permissions` list is advisory today. Two things
already treat it as data:

- The store UI displays what a plugin asks for, and the operator
  approves at install time.
- The `spec.permissions` block is validated against the schema —
  bad shapes fail Discover.

Actual enforcement (locked-down file APIs, network egress, secret
store access) is future work under `[Plugin] permission enforcement`.

Design the manifest as if enforcement was live: ask for the
minimum a plugin needs. When enforcement lands, that will be the
minimum you get.

## TLS trust for plugins that talk to hardware

Plugins that talk to a device over TLS (DIRIGERA, some HTTP hubs)
face self-signed certificates. `InsecureSkipVerify` is not
supported by the SDK. Two verified paths:

- CA PEM shipped in config — the plugin trusts exactly that CA
  when validating the hub.
- SHA-256 leaf fingerprint pin — verifies via
  `VerifyPeerCertificate` after computing the fingerprint of the
  leaf cert Rekor got.

`plugins/dirigera/internal/dirigera/client.go` is the reference
implementation of both.

## What this all buys

- The store cannot install a modified tarball that a signer did
  not sign, if signing is on.
- The daemon cannot be turned into an SSRF probe by pointing
  install at a metadata service.
- A hostile registry cannot use redirects to escape the URL
  policy.
- A hostile plugin dir cannot use symlinks inside `ui/` to
  exfiltrate host files.
- A hostile plugin main cannot serve `/etc/passwd` from a `ui/`
  route.
- Root rotation on Sigstore's side reaches every install without
  a manual re-pin, so a signing-key compromise does not brick
  the plugin ecosystem.

The gaps are on the runtime sandboxing side. Fill them before
running strangers' plugins on a production install.
