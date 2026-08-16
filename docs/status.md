# Keystone — статус

**Дата:** 2026-08-16
**Обновляется** на переходах между фазами, а не при каждом коммите.

**Куда смотреть за деталями:**
- Задачи по фазам → https://github.com/users/kliuchnikovv/projects/5
- Архитектура → `docs/plugin-store-architecture.md`, `docs/sidecar-protocol-v1.md`, `docs/plugin-ui-integration.md`, `docs/plugin-sdk-guide.md`, `docs/keystone-cli-spec.md`
- Технический прогресс → `git log`, PR'ы
- Что не работает в Matter → `docs/matter-implementation-gaps.md`

---

## 0. TL;DR

**Phase A → B переход.** Matter-adapter доведён до прод-качества, параллельно построена основа plugin-архитектуры. Дальше — plugin-manager в ядре и extraction Matter как первого нативного плагина.

---

## 1. Что закрыто

### Phase A — Matter hardening

Из 8 плановых задач:

- ✅ **Realtime state push** (StateStream + Descriptor.ServerList) — физические изменения из Apple Home приходят в keystone <500 мс.
- ✅ **Read BasicInformation + DeviceTypeList** — устройства именуются корректно («IKEA WARMBLIXT», не «Matter device N»), тип определяется из Matter-спеки.
- ✅ **Reconnect + heartbeat** — обрыв sidecar посреди сессии восстанавливается автоматически, WS ping/pong каждые 10 сек.
- ✅ **Structured error taxonomy** — 7 ErrorKind (bad_request/not_found/not_ready/unreachable/timeout/unsupported/internal), sentinelByKind + `errors.Is` в handler'ах вместо regex по тексту.

⚠️ Требуют верификации на живом WARMBLIXT:
- Auto-subscribe после commission — код с StateStream работает, явного `subscribeAll` нет, полагается на matter.js 0.17 auto-subscribe.
- Per-feature endpoint mapping — логика есть, `defaultEndpoint = 1` остался как fallback. Multi-endpoint не тестили.
- Initial state read после commission — нет явного кода, но появился `internal/service/confirm.go` (watch-and-verify command effects) как reliability слой.

### Phase B — Plugin architecture (частично)

- ✅ **Sidecar Protocol v1** — отдельная репа `keystone-api`. 9 коммитов. Cross-language conformance testing (Go ↔ TS peers). Бонусом: миграция gRPC API ядра туда же (`core/proto/keystone/`).
- ✅ **Plugin manifest v1 + validator** — `internal/plugin/{manifest.go, semantic.go, validator.go, testdata/, manifest_schema.json}` + CLI `cmd/keystone-plugin-validate/`. **Не закоммичено, лежит в working tree на `plugin/manifest-v1`.**

### Bonus (сверх плана)

Сделано, но не было в Ready:

- **BLE transport + Wi-Fi credentials** — out-of-box commissioning устройств без внешней экосистемы.
- **Full device-type coverage** — сильно превышает «Coverage wave 1»: valves, temperature control, mode select, buttons.
- **Camera WebRTC** — Matter 1.4 camera cluster + WebRTC data plane.
- **CSA data model generator** — `matter_gen.go` / `generated_test.go` auto-generated из спецификации CSA. Toolchain в `tools/`.
- **Fabric management** — remove без trust'а stale flag'ам, консистентное состояние при аварийных сценариях.
- **Command confirmation** (`internal/service/confirm.go`) — после state-changing команды ждём report; если не приходит — surface в UI, а не «нашумели впустую».

---

## 2. Что в работе

- **[UI] Design System v0.1** — scope сжат до token layer: `apps/web/src/theme/tokens.css` + 418 использований `--ks-*` через `apps/web`. Web Components (`<ks-*>`), packages/design и Storybook — **отложены до Phase G**, реально нужны только когда стартует UI-runtime для плагинов.

---

## 3. Что дальше

**Phase B продолжение:**
- Plugin manager в ядре (lifecycle, sandbox, restart, health-check по heartbeat)
- HTTP API endpoints `/plugins/*` (list / install / uninstall / logs / config / health)
- Extract Matter в `keystone-plugin-matter` — первое живое использование plugin-manager'а, валидация модели

**Phase D параллельно:**
- Git-based plugin registry (`plugin-registry` репа с Homebrew-style tap)
- Store UI в keystone
- Config-flow parser: JSON Schema → auto-form (Layer 1 из `plugin-ui-integration.md`)

**Phase E:**
- `sdk-go` v0.1
- CLI `plugin new/build/test/publish` + template repo
- Reference plugin: `plugin-dirigera`

Все они — в Backlog на GH project board, разблокируются последовательно после Phase B.

---

## 4. Repo hygiene — надо разобрать

Текущее состояние **всё локально, ничего не запушено**:

- `master` — ahead 11 коммитов от `origin/master`
- `feat/matter-hardening` — 17 коммитов Matter + BLE + Coverage + WebRTC, готов к PR
- `plugin/manifest-v1` — тот же tip, что `feat/matter-hardening`, плюс uncommitted Task 9 + Task 10 работы
- `keystone-api` — только локально, репозиторий на GitHub не создан

Uncommitted файлы на `plugin/manifest-v1`:
- Task 9 code: `internal/plugin/*`, `cmd/keystone-plugin-validate/`
- Task 10 code: `apps/web/src/theme/tokens.css` + ~20 `apps/web/src/components/**/*.module.css`
- Docs: 5 файлов в `docs/` (plugin-store-architecture, keystone-cli-spec, plugin-sdk-guide, plugin-ui-integration, sidecar-protocol-v1)
- Misc: `.claude/`, `create-project-tasks.py`, `apps/web/scripts/`

**План разбора** (по appетиту):
1. `gh repo create kliuchnikovv/keystone-api --public` + push всех 9 коммитов
2. Push `feat/matter-hardening` в keystone + open PR к master
3. Отделить Task 9 и Task 10 в чистые ветки от master, отдельные PR
4. Docs + misc — либо в matter-hardening PR (docs — сателлит Matter'а), либо docs-only PR

---

## 5. Известные gaps

- Matter coverage — см. `docs/matter-implementation-gaps.md` §Coverage matrix. Wave 1 в основном закрыт (bonus агента). Wave 2 (Thermostat, FanControl, Air quality, SmokeCoAlarm) — отложена.
- Design System — только tokens, Web Components в Phase G-K.
- HA Bridge (Store 2) — Phase F, не начат.
- Voice/ML плагины — Post-v1.
