# Keystone — статус

**Дата:** 2026-08-16
**Обновляется** на переходах между фазами, а не при каждом коммите.

**Куда смотреть за деталями:**
- Задачи по фазам → https://github.com/users/kliuchnikovv/projects/5
- Архитектура → `docs/plugin-store-architecture.md`, `docs/sidecar-protocol-v1.md`, `docs/plugin-ui-integration.md`, `docs/plugin-sdk-guide.md`, `docs/keystone-cli-spec.md`
- Технический прогресс → `git log`, PR'ы
- Что не работает в Matter → `docs/matter-implementation-gaps.md`
- Developer portal → `docs/portal/`

---

## 0. TL;DR

**Phase B → K закрыты за одну волну.** 25 из 33 задач Done. Ядро плагин-архитектуры, Matter как out-of-process плагин, plugin store с полным Sigstore/Rekor/TUF-стеком, SDK + CLI, UI runtime Layer 1-4, developer portal, CLI command surface — всё в коммитах. Остались верификация Matter, HA Bridge, robustness-мелочи.

---

## 1. Что закрыто

**Phase A — Matter hardening.** Realtime state via StateStream, reconnect + heartbeat, structured error taxonomy (7 ErrorKind), BasicInformation + DeviceTypeList, BLE transport + Wi-Fi credentials, Camera WebRTC, full device-type coverage через CSA data model generator, fabric hygiene, command confirmation. Детали и оставшиеся gaps — `docs/matter-implementation-gaps.md`.

**Phase B — Plugin architecture.** Plugin manifest v1 schema + validator (`internal/plugin/`), plugin supervisor / registry / manager runtime lifecycle, `/plugins/*` HTTP API endpoints, bridge adapter (`ports.Adapter` поверх sidecar-plugin), declared sidecars + persistent enabled state, restart + tailable logs + validated config over HTTP.

**Phase C — Matter extraction.** Matter вынесен в out-of-process плагин, in-tree Matter wiring убран из ядра, bridge extensions для commission progress / CommissionableDiscoverer / CameraStreamer.

**Phase D — Store.** Store UI (browse registries + install), registry install/uninstall over HTTP, pluggable signing + SSRF hardening, полный Sigstore keyless + Rekor transparency log + SET-artifact binding, минимальный TUF-клиент для trust bundle, root rotation, 4 fail-open closures в TUF refresh/fetch.

**Phase E — SDK + CLI dev.** `sdk-go` v0.1, reference plugin-dirigera с WebSocket realtime, CLI `keystone-plugin new/build/test/publish/install` с defense-in-depth.

**Phase G-K — UI runtime.** Layer 1 config-flow auto-form из JSON Schema, Layer 2 wizard end-to-end, Layer 3 Web Components loader + `window.keystone`, Layer 4 iframe embed + postMessage bridge, plugins nav в home header.

**CLI — Phase 1-2.** Base command surface (plugin/device/system/self/stream), full surface (rule/room/scene/config/store/secret).

**Foundation.** JS workspace hoisted к repo root (yarn workspaces), theme переведён на plain CSS custom properties, `packages/design/` workspace scaffold, developer portal под `docs/portal/`.

---

## 2. Что в работе

**Matter verification** (3, ждут физического WARMBLIXT):
- Auto-subscribe peers after commission — код полагается на matter.js 0.17 auto-subscribe, нужен реальный прогон
- Per-feature endpoint mapping — логика есть, `defaultEndpoint = 1` остался как fallback, multi-endpoint не тестировался
- Read initial state after commission — заменён на command-confirmation слой (`confirm.go`), нужен визуальный тест «UI не показывает неизвестно после commission»

**Design System v0.1** — `packages/design/` workspace создан с package.json (name `@keystone/design`), tsconfig, vite.config, но `src/index.ts` пока пуст: «компоненты появляются здесь по мере готовности». Skeleton есть, наполнение — следующий шаг.

---

## 3. Что дальше — Backlog (5 задач)

1. **HA Bridge plugin skeleton** — Phase F, крупный кусок. Bundle Python + HA Core + `keystone_bridge` custom_component.
2. **HA Bridge — Store 2 UI** внутри плагина ha-bridge.
3. **HA Bridge — config-flow parser** HA voluptuous → keystone Layer 2 steps.
4. **Matter snapshot re-fetch + seq/replay** — robustness. Пиковая ценность после первого прод-инцидента.
5. **CLI dev commands + shell completions** — completions (bash/zsh/fish), plugin CLI extensions namespace.

Плюс наполнение `packages/design/` до восьмёрки Web Components + `.ks-label` utility (см. `plugin-ui-integration.md` §8).

---

## 4. Repo hygiene

**Ветки keystone:**
- `master` — база, не двигается
- `feat/matter-hardening` (@ 703e211) — 44 коммита Phase A
- `plugin/manifest-v1` (@ bfc5842) — 60 коммитов, содержит Phase A-K, developer portal, CLI

**Ветки keystone-api:**
- `main` — 9 коммитов, Sidecar Protocol v1 + gRPC миграция + cross-language conformance

Всё запушено на origin (`git@github.com:kliuchnikovv/keystone.git` и `git@github.com:kliuchnikovv/keystone-api.git`). PR'ы под отдельное решение — сейчас master не движется, работа идёт в feature-веткax.

---

## 5. Известные gaps

- Matter — 3 verification-задачи + snapshot/replay + robustness-мелочи. Детали в `docs/matter-implementation-gaps.md`.
- Design System — 8 Web Components + form-associated ElementInternals + 5 consumer-migrations в apps/web. Скоуп зафиксирован в карточке доски и `docs/plugin-ui-integration.md` §8.
- HA Bridge — вся Phase F не начата.
- Voice/ML плагины — Post-v1.
