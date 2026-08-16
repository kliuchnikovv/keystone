# Keystone — статус

**Дата:** 2026-08-17
**Обновляется** на переходах между фазами, а не при каждом коммите.

**Куда смотреть за деталями:**
- Задачи по фазам → https://github.com/users/kliuchnikovv/projects/5
- Author-facing docs → `docs/portal/README.md`
- Архитектура → `docs/plugin-store-architecture.md`, `docs/sidecar-protocol-v1.md`, `docs/plugin-ui-integration.md`, `docs/plugin-sdk-guide.md`, `docs/keystone-cli-spec.md`
- Технический прогресс → `git log`, PR'ы
- Что не работает в Matter → `docs/matter-implementation-gaps.md`

---

## 0. TL;DR

**Phase B–G closed.** Plugin runtime, store с sigstore-keyless-подписью через TUF, SDK+CLI, четыре слоя UI runtime и author-facing docs portal — всё поднято end-to-end. Ветка `plugin/manifest-v1` содержит 57 коммитов над `master`, всё запушено. Осталось живое железо для верификации `plugins/matter` + Phase F (HA Bridge) + `[CLI] keystone` command surface.

---

## 1. Что закрыто

### Phase A — Matter hardening

- ✅ Realtime state push, BasicInformation, reconnect/heartbeat, error taxonomy, Coverage wave 1 (+ bonus: valves / temp control / mode select / buttons / WebRTC camera).

⚠️ В `In progress` на борде (нужна hardware-верификация):
- Auto-subscribe after commission
- Per-feature endpoint mapping
- Read initial state after commission

### Phase B — Plugin architecture (end-to-end)

- ✅ **Sidecar Protocol v1** — отдельный репо `keystone-api` (private, есть на GitHub).
- ✅ **Plugin manifest v1 + validator** — `internal/plugin/*`, JSON Schema, CLI.
- ✅ **Plugin manager** — supervisor + registry + FSM + declared sidecars + persistent enabled state, `internal/plugin/{supervisor,registry,manager}`.
- ✅ **HTTP `/plugins/*`** — list/get/enable/disable/restart/logs/config/install/discover/uninstall/registry/flow/ui, `internal/api/plugins`.
- ✅ **Bridge** (`internal/plugin/bridge`) — реализация `ports.Adapter` поверх `*sidecar.Client`. Плюс `CommissionableDiscoverer` и `CameraStreamer` — оба опциональных интерфейса `ports.Adapter` работают через bridge.
- ✅ **Extract Matter** — `plugins/matter/` работает как отдельный процесс; in-tree Matter из `cmd/keystone` удалён (`refactor(core): drop the in-tree Matter wiring`).
- ✅ **UI Design System v0.1** (token layer) — `apps/web/src/theme/tokens.css` + 418 использований `--ks-*`.

### Phase D — Store

- ✅ **Git-based plugin registry** — HTTP client `internal/plugin/store`, index.json format, `POST /plugins/install` + `DELETE /plugins/{name}`.
- ✅ **Signing** — трёхуровневый `Verifier` интерфейс:
  - `SHA256Verifier` (default)
  - `Ed25519Verifier` (detached sig под trusted public keys)
  - `SigstoreVerifier` — Fulcio cert chain + SAN identity allowlist + signature over sha256 digest + Rekor SET binding + inclusion proof (verified checkpoint, не self-attested root) + artifact binding через hashedrekord body.
- ✅ **TUF client** — `internal/plugin/store/tuf/`. Trusted-once initial root, atomic Refresh, cross-role integrity (timestamp→snapshot→targets), rollback защита, expiry re-check, length + sha256 gates, **root rotation** (rolling-key chain: current-root sig + self-sig + strict N+1 version + 128 step backstop).
- ✅ **SSRF hardening** — `TrustedRegistries` allowlist + scheme+host validation + IP resolution refusing private/link-local/loopback (кроме explicit loopback base).
- ✅ **Store UI** (React) — browse registry + install / enable / disable / restart / uninstall / logs / config + nav-link из HomeScreen.
- ✅ **Config-flow parser Layer 1** — `SchemaForm` компонент.

### Phase E — SDK

- ✅ **`sdk-go v0.1`** — `plugins/sdk` — обёртка вокруг `sidecar.RunPlugin`, автоматическая регистрация 8 базовых методов + опциональные `CommissionableDiscoverer`/`CameraStreamer`, `Options.ConfigFlow` handler, `LoadConfig`, `ErrorMapper`.
- ✅ **CLI `keystone-plugin`** — `new/build/test/install/publish` — embedded templates, tarball round-trip с exec-битом, `--reload http://...` для discover после install, defense-in-depth path safety.
- ✅ **Reference plugin: `plugin-dirigera`** — HTTPS REST client с TLS pinning (CA PEM или SHA-256 fingerprint, **не** `InsecureSkipVerify`), WebSocket realtime → `TransportEvent`, error mapping в sidecar codes.
- ✅ **Docs portal** — 7 chapters plain markdown в `docs/portal/`: quick start / manifest / SDK / UI layers / publishing / security. Всё против реального кода в дереве.

### Phase G — UI runtime (все 4 слоя)

- ✅ **Layer 1** — JSON Schema → auto-form (`SchemaForm`).
- ✅ **Layer 2** — Declarative Config Flow — bridge method + `sdk.Options.ConfigFlow` handler + HTTP proxy + React wizard с 9 step types (info/form/oauth/qr-scan/progress/confirm/pick-device/manual-action/error/complete).
- ✅ **Layer 3** — Web Components loader — `GET /plugins/{name}/ui/*` static serve (symlink-resolving path safety), `window.keystone` API, `PluginDetailSlot` монтирует custom element с валидацией tag name по WHATWG regex.
- ✅ **Layer 4** — Iframe embed + postMessage bridge — `keystone-embed.js` bootstrap для плагина, `attachEmbedBridge` в parent, sandbox `allow-scripts allow-same-origin`, 64-sub cap.

### Bonus (сверх плана)

- **Compromised-SET binding fix** (security review) — `rekor.verifyEntryBindsArtifact` парсит hashedrekord body и cross-checks с `sha256(payload)` до принятия signature.
- **Self-attested-root fix** (security review) — Rekor checkpoint verifier, отказ от bundle's own rootHash без Rekor-signed checkpoint.
- **4 fail-open windows в TUF** (security review) — atomic Refresh, snapshot meta required, root expiry re-check, target length gate.
- **Symlink bypass fix в UI static serve** — `filepath.EvalSymlinks` перед containment check.

---

## 2. Что в работе

Только hardware-верификация:
- Три Matter-задачи в `In progress` на борде (auto-subscribe / per-feature endpoint / initial state read).
- Полный e2e `plugins/matter/` против живого WARMBLIXT.
- `plugins/dirigera/` против живого хаба IKEA (TLS pin + WebSocket realtime).

---

## 3. Что дальше

**Phase F — HA Bridge:**
- HA Bridge plugin skeleton
- Store 2 UI внутри ha-bridge
- Config-flow parser HA → keystone Layer 2

**Phase — CLI `keystone` command surface** (три задачи в Backlog):
- Базовая поверхность: plugin/device/system/self/stream
- Полный набор: rule/room/scene/config/store/secret
- Dev commands + shell completions + extensions

**Matter Backlog:**
- Snapshot re-fetch after reconnect + seq/replay

---

## 4. Repo hygiene

**Всё запушено** (`plugin/manifest-v1` = 57 коммитов над `master`, `master` sync с `origin/master`). `keystone-api` опубликован приватно.

Uncommitted misc в working tree: `.claude/`, `create-project-tasks.py`, пустой `packages/design/` — не относится к plugin-стеку.

---

## 5. Известные gaps

- **Sandboxing** — плагины бегут как обычные child-процессы под user'ом daemon'а. cgroups/rlimit enforcement `spec.resources`, no-network / read-only-fs sandboxing, UID mapping — не сделано. Открыто описано в `docs/portal/06-security.md`.
- **Permissions enforcement** — `spec.permissions` пока advisory (schema-валидируется, store UI показывает, но runtime не гейтит). См. `docs/portal/06-security.md`.
- **Matter coverage wave 2** — Thermostat, FanControl, Air quality, SmokeCoAlarm — отложено.
- **Voice/ML плагины** — Post-v1.
