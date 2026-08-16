# Keystone — статус

**Дата:** 2026-08-16 (обновление после сверки с GH project 5)
**Обновляется** на переходах между фазами, а не при каждом коммите.

**Куда смотреть за деталями:**
- Задачи по фазам → https://github.com/users/kliuchnikovv/projects/5
- Архитектура → `docs/plugin-store-architecture.md`, `docs/sidecar-protocol-v1.md`, `docs/plugin-ui-integration.md`, `docs/plugin-sdk-guide.md`, `docs/keystone-cli-spec.md`
- Технический прогресс → `git log`, PR'ы
- Что не работает в Matter → `docs/matter-implementation-gaps.md`

---

## 0. TL;DR

**Phase B закрыта end-to-end.** Plugin runtime построен (supervisor + registry + manager + HTTP `/plugins/*` + bridge). Matter extract'нут в `plugins/matter/` — идентичный in-tree adapter, но живёт отдельным процессом; enable/disable через `/plugins/matter/enable` работает на живом keystone, smoke-тест проходит. Дальше — Phase D (git-registry, Store UI) и удаление in-tree Matter, когда плагин обкатается в проде.

---

## 1. Что закрыто

### Phase A — Matter hardening

Из 8 плановых задач:

- ✅ **Realtime state push** (StateStream + Descriptor.ServerList) — физические изменения из Apple Home приходят в keystone <500 мс.
- ✅ **Read BasicInformation + DeviceTypeList** — устройства именуются корректно («IKEA WARMBLIXT», не «Matter device N»), тип определяется из Matter-спеки.
- ✅ **Reconnect + heartbeat** — обрыв sidecar посреди сессии восстанавливается автоматически, WS ping/pong каждые 10 сек.
- ✅ **Structured error taxonomy** — 7 ErrorKind (bad_request/not_found/not_ready/unreachable/timeout/unsupported/internal), sentinelByKind + `errors.Is` в handler'ах вместо regex по тексту.
- ✅ **Coverage wave 1+ (bonus)** — Illuminance/Switch/RGB/DoorLock/WindowCovering плюс сверх плана: valves, temperature control, mode select, buttons, WebRTC-камера.

⚠️ Требуют верификации на живом WARMBLIXT (остаются `In progress` на борде):
- Auto-subscribe после commission — код с StateStream работает, явного `subscribeAll` нет, полагается на matter.js 0.17 auto-subscribe.
- Per-feature endpoint mapping — логика есть, `defaultEndpoint = 1` остался как fallback. Multi-endpoint не тестили.
- Initial state read после commission — нет явного кода, но `internal/service/confirm.go` (watch-and-verify) закрывает практический риск.

### Phase B — Plugin architecture (частично)

- ✅ **Sidecar Protocol v1** — отдельная репа `keystone-api`. 9 коммитов. Cross-language conformance testing (Go ↔ TS peers). Бонусом: миграция gRPC API ядра туда же (`core/proto/keystone/`).
- ✅ **Plugin manifest v1 + validator** — `internal/plugin/{manifest.go, semantic.go, validator.go, testdata/, manifest_schema.json}` + CLI `cmd/keystone-plugin-validate/` (commit `7052c6f`).
- ✅ **UI Design System v0.1 (token layer)** — `apps/web/src/theme/tokens.css` + 418 использований `--ks-*` (commits `046cffa` / `6589f8a` / `2f93318`). Web Components отложены до Phase G — треки `Layer 3 Web Components loader` / `Layer 4 iframe router` остались в бэклоге.

### Bonus (сверх плана)

Сделано, но не было в Ready:

- **BLE transport + Wi-Fi credentials** — out-of-box commissioning устройств без внешней экосистемы.
- **Full device-type coverage** — сильно превышает «Coverage wave 1»: valves, temperature control, mode select, buttons.
- **Camera WebRTC** — Matter 1.4 camera cluster + WebRTC data plane.
- **CSA data model generator** — `matter_gen.go` / `generated_test.go` auto-generated из спецификации CSA. Toolchain в `tools/`.
- **Fabric management** — remove без trust'а stale flag'ам, консистентное состояние при аварийных сценариях.
- **Command confirmation** (`internal/service/confirm.go`) — после state-changing команды ждём report; если не приходит — surface в UI, а не «нашумели впустую».

---

## 2. Что закрыто в этой сессии (Phase B финиш)

Runtime уже готов в `keystone-api/sidecar` (`sidecar.RunPlugin`, `sidecar.Connect`, NDJSON codec, symmetric Peer, seq/replay ringbuf, heartbeat, backoff, topic matcher, structured errors) — фаза «Runtime» пропущена. Все остальные слои Phase B реализованы:

1. **`internal/plugin/supervisor`** — spawn entrypoint (`os/exec.CommandContext` под supervisor-owned ctx, чтобы жизнь ребёнка не привязывалась к Enable-request-ctx), listen UDS на `$KEYSTONE_PLUGIN_SOCKET`, connect, keep-alive с RestartOnFailure policy, log capture.
2. **`internal/plugin/registry`** — walk `-plugins-dir`, load+validate манифесты, sort по `spec.dependsOn`, broken-entry surfacing.
3. **`internal/plugin/manager`** — FSM (`Discovered/Running/Stopped/Failed`), `Discover/Enable/Disable/List/Get/Client`, callback-хуки `OnEnable`/`OnDisable` для интеграции с DeviceService.
4. **`internal/api/plugins`** — `/plugins`, `/plugins/discover`, `/plugins/{name}`, `/plugins/{name}/enable`, `/plugins/{name}/disable`.
5. **`internal/plugin/bridge`** — `ports.Adapter` поверх `*sidecar.Client` + wire-контракт (`adapter.start/stop/discover/commission/readState/writeState/invokeAction/decommission` методы + `adapter.event` topic).
6. **`plugins/matter/`** — extract готов. `main.go` использует `sidecar.RunPlugin`, оборачивает `internal/adapters/matter` через bridge-vocabulary, `parseMatterURL` вынесена в `matter.ParseSidecarURL`. Manifest `plugin.yaml` описывает TS `matter-server` как declared sidecar. **Smoke-тест на живом keystone проходит**: `POST /plugins/matter/enable` → state=running/connected=true, adapter регистрируется, `POST /plugins/matter/disable` → clean teardown.
7. **`service.DeviceService.RegisterAdapter/UnregisterAdapter`** — thread-safe хук для runtime-mount плагинов через RWMutex.

**Core больше не говорит Matter напрямую:** `-matter-sidecar` флаг удалён, `internal/adapters/matter` из `cmd/keystone` не импортируется, `matter.KindOf`/`IsRetryable` в commission-progress handler заменены на generic `sidecar.CodeOf`/`sidecar.IsRetryable`. Пакет `internal/adapters/matter` остаётся — это библиотека, которую использует плагин. `domain.TransportMatter` тоже остаётся — used by matter package + tests, никакой matter-специфики в ядре нет.

---

## 3. Что дальше

**Валидация на живом железе:**
- Прогнать `plugins/matter/` против реального WARMBLIXT — commission/read/write/invoke/decommission по всей цепочке core→bridge→plugin→matter-server.
- После — удалить in-tree Matter из `cmd/keystone/main.go` и `internal/adapters/matter` (или оставить как `pkg/matter` для плагина), убрать `domain.TransportMatter` и `-matter-sidecar` флаг.
- Опционально: перенести `matter.KindOf`/`matter.IsRetryable` вызовы из `cmd/keystone/main.go` в service-level generic error taxonomy, чтобы progress-frames работали и с plugin-errors.

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

Uncommitted файлы на `plugin/manifest-v1` (осталось только misc):
- `.claude/`, `create-project-tasks.py`, пустой `packages/design/`.

Task 9 (`internal/plugin/*`, `cmd/keystone-plugin-validate/`), Task 10 (token layer), design docs — уже в ветке.

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
