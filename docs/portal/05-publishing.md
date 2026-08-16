# Publishing

Once your plugin builds and passes `keystone-plugin test`, ship it.

Two paths:
- **Direct install** — hand-copy or `keystone-plugin install` into
  a specific keystone's `-plugins-dir`. Fine for a single deployment
  or an internal plugin.
- **Registry** — publish to a plugin registry the store UI can
  browse. This is what public plugins do.

## Direct install

```sh
keystone-plugin install \
  --dir . \
  --target /var/lib/keystone/plugins \
  --reload http://localhost:7777
```

The tarball round-trip is available too:

```sh
keystone-plugin publish -o hello-0.1.0.tgz
keystone-plugin install --tarball hello-0.1.0.tgz --target /var/lib/keystone/plugins
```

`publish` packs `plugin.yaml + bin/ + sidecars/ + ui/`, preserving
the executable bit on the entrypoint. Everything else in your
source tree stays behind — no `.git`, no vendored source, no scratch
files.

## Registry

A registry is a plain HTTPS host that serves:

- `<baseURL>/index.json` — the catalog
- `<baseURL>/packages/<name>-<version>.tgz` — the tarballs

`index.json` is one file:

```json
{
  "plugins": {
    "hello": {
      "description": "Talks to a Hello hub.",
      "homepage": "https://github.com/you/plugin-hello",
      "versions": {
        "0.1.0": {
          "url": "packages/hello-0.1.0.tgz",
          "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "bundle": "…optional sigstore bundle…"
        }
      }
    }
  }
}
```

That's the whole contract. A static file host — GitHub Pages,
Netlify, S3, an HTTPS server on your NAS — works.

### Publish flow

1. Build and sign locally.
2. Copy the tarball to `<baseURL>/packages/<name>-<version>.tgz`.
3. Edit `index.json` to append the version with its sha256 (and
   sigstore bundle if you sign; see chapter 6).
4. Push.

### Installing from a registry

Store UI:
- Open `/plugins`, paste the registry URL, hit "Открыть".
- The browse list shows every plugin the registry offers with
  their descriptions and version drop-downs.
- Hit "Установить" and the daemon fetches the tarball, verifies
  the sha256 (and the sigstore bundle if a verifier is configured),
  unpacks into its `-plugins-dir`, and rediscovers.

HTTP:

```sh
curl -X POST -H content-type:application/json \
  -d '{"name":"hello","version":"latest","registry":"https://plugins.example.com"}' \
  http://localhost:7777/plugins/install
```

## Versioning

Semver. The registry accepts anything but the store's "latest"
picks the highest that parses as `MAJOR.MINOR.PATCH`. Bump `MAJOR`
when the wire changes (new manifest fields the old core does not
understand, entrypoint arg changes, breaking permission bumps).

`spec.keystoneCoreMin` and `spec.keystoneCoreMax` gate the core's
version. Fill them in — a plugin that shipped for keystone 0.5 must
not silently start against a hypothetical 2.0.

## Local testing before you push

Run a mini-registry from your build:

```sh
keystone-plugin publish -o out/hello-0.1.0.tgz
mkdir -p out/packages
cp out/hello-0.1.0.tgz out/packages/
SHA=$(shasum -a 256 out/packages/hello-0.1.0.tgz | cut -d' ' -f1)
cat >out/index.json <<EOF
{"plugins":{"hello":{"description":"local","versions":{"0.1.0":{"url":"packages/hello-0.1.0.tgz","sha256":"$SHA"}}}}}
EOF
python3 -m http.server 8000 --directory out
```

Then in another terminal:

```sh
curl -X POST -H content-type:application/json \
  -d '{"name":"hello","version":"latest","registry":"http://localhost:8000"}' \
  http://localhost:7777/plugins/install
```

`http://` is only allowed for loopback hostnames; that's what keeps
local dev friction-free without opening an SSRF hole in production
(where `https://` is required).

## What the daemon refuses

- A registry URL that isn't `https://` (unless the hostname is
  loopback).
- A registry URL whose DNS resolves to a private / link-local /
  unspecified IP (SSRF gate; DNS rebinding against a public host
  is refused at dial time too).
- A tarball whose sha256 does not match `index.json`.
- A tarball with a path that escapes the plugin directory (absolute
  or `..`-containing entries).
- A plugin whose `metadata.name` doesn't match the registry key.

Signing (chapter 6) turns "trusted host" into "trusted signer" —
signal ownership without pinning to a particular provider.
