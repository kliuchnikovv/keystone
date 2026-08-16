# Keystone × Plugin Store — архитектура

**Статус:** v0.1
**Родительские документы:**
- [`smart-home-engine-brief.md`](../../../claude/smart-home-engine-brief.md) — продуктовая рамка.
- [`go-core-spec.md`](../../../claude/go-core-spec.md) — устройство ядра.
- [`matter-adapter-brief.md`](./matter-adapter-brief.md) — референс-плагин (первый нативный).

## 1. Мотивация

Keystone позиционируется как «красиво, быстро, совместимо». Первые две опоры мы держим сами — своим ядром, дизайном и UX. Опору «совместимо» невозможно закрыть в одиночку: экосистема smart home — это сотни вендоров, десятки протоколов, тысячи облачных API. Home Assistant за 10 лет собрал 1400+ интеграций трудом сотен контрибьюторов; повторить это в одиночку не получится, а без этого мы либо покрываем только Matter/Zigbee (узко), либо становимся дашбордом поверх HA (несамостоятельны).

Решение — **двухуровневый магазин плагинов**:

- **Store 1 (нативные плагины Go)** — курируемый список плагинов, написанных на Go против нашего SDK и sidecar-контракта. Отвечаем за качество мы. Именно тут живёт вся «первая линия»: Matter, Zigbee2MQTT, DIRIGERA, HomeKit-bridge, voice-agent, ML-плагины.
- **Store 2 (HA-совместимые расширения)** — сам по себе один из плагинов Store 1 (`ha-bridge`). Внутри него — витрина 1400+ HA-интеграций. Тир качества ниже, но экосистемное покрытие полное.

Ключевой архитектурный эффект: **Matter перестаёт быть частью ядра** и становится первым нативным плагином. Ядро худеет до минимума, всё функциональное — snap-in.

## 2. Модель ядра после реорганизации

```
keystone-core (Go binary, ~30MB, ~30MB RAM)
├── domain / ports / registry / eventbus / service / rules
├── plugin-manager (lifecycle, health, sandbox)
├── secretstore, scheduler, observability, access-control
├── HTTP API + gRPC + NDJSON stream + WS
└── UI (bundled static assets)

<installed plugins live outside the binary>
/etc/keystone/plugins/
├── matter/               (Store 1, official)
├── zigbee2mqtt/          (Store 1, official)
├── ha-bridge/            (Store 1, official — но своя витрина внутри Store 2)
└── my-custom-plugin/     (Store 1, community)
```

Ядро на первом запуске **не знает ни про Matter, ни про Zigbee**. Пользователь через store ставит нужные плагины. Baseline RAM keystone без плагинов ~30MB; каждый транспортный плагин добавляет 50-300MB (Matter/Node ~150MB, HA-bridge/Python ~300MB).

## 3. Формат плагина

Плагин — это директория со следующей структурой:

```
keystone-plugin-matter/
├── plugin.yaml              # манифест
├── LICENSE
├── README.md
├── icon.svg
├── screenshots/*.png
├── go/                      # адаптер (компилится в бинарь plugin-matter)
│   ├── main.go
│   └── ...
├── sidecars/                # опционально: под-процессы, нужные плагину
│   └── matter-server/
│       ├── src/
│       ├── package.json
│       └── dist/
├── ui/                      # опционально: JSX для config-flow, device-detail
│   ├── config-flow.tsx
│   └── device-detail.tsx
└── tests/
```

### 3.1 Манифест `plugin.yaml`

**Нормативный источник — [`internal/plugin/manifest_schema.json`](../internal/plugin/manifest_schema.json).** Пример ниже держим синхронным со схемой; при расхождении прав файл схемы. Проверка манифеста:

```
$ keystone-plugin-validate ./keystone-plugin-matter
$ make validate-manifests MANIFESTS='plugins/*/plugin.yaml'
```

Валидатор указывает файл, строку и колонку:

```
plugin.yaml:22:16: spec.sidecars[0].restart: value must be one of 'always', 'on-failure', 'never'
```

```yaml
apiVersion: keystone.plugin/v1
kind: Plugin
metadata:
  name: matter                    # уникальный slug в registry
  displayName: Matter
  version: 1.2.0                  # SemVer
  description: |
    Multi-admin Matter controller via matter.js. Supports commissioning,
    control, and subscription for lights, plugs, and sensors.
  maintainer: keystone team <core@keystone.io>
  license: Apache-2.0
  homepage: https://github.com/keystone/plugin-matter
  category: transport             # transport | device | automation | ai | ui
  tags: [matter, thread, wifi, official]
  trustTier: core                 # core | verified | experimental

spec:
  keystoneCoreMin: "0.5.0"        # минимальная версия ядра
  keystoneCoreMax: "1.x"          # верхняя граница совместимости

  capabilities:                   # dotted-id'ы, тот же словарь, что в hello
    - transport.matter            # sidecar-протокола v1 — см. §4
    - discover.passive
    - commission.setup-code
    - commission.qr-code
    - realtime.state-stream

  dependsOn: []                   # опционально: слаги плагинов, нужных до старта

  permissions:                    # что плагин просит у ОС/ядра
    - network.lan
    - network.multicast
    - storage.persistent
    - secretstore.read: [matter.fabric-key]
    - secretstore.write: [matter.fabric-key]

  resources:
    memory: 256Mi
    cpu: 100m
    disk: 500Mi

  entrypoint:                     # опционально — UI-only плагину бинарь не нужен
    exec: go/matter-plugin        # главный бинарь плагина
    args: ["--socket", "$KEYSTONE_PLUGIN_SOCKET"]

  sidecars:                       # дочерние процессы, которыми плагин управляет
    - name: matter-server
      exec: sidecars/matter-server/dist/index.js
      runtime: node20
      args: []
      env:
        KEYSTONE_MATTER_STORAGE: "$KEYSTONE_PLUGIN_DATA/matter-fabric"
      restart: on-failure

  config:                         # схема пользовательских настроек (JSON Schema)
    schema: |
      {
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "properties": {
          "fabricLabel": {"type": "string", "default": "Keystone"}
        }
      }
```

Манифест — единственный источник истины про плагин. По нему plugin-manager решает: что запускать, какие права давать, куда лить данные, как обновлять.

Правила, которые JSON Schema не выражает, живут в `internal/plugin/semantic.go` и проверяются тем же вызовом:

- `spec.entrypoint` обязателен, **если** плагин не объявляет top-level блок `ui` — плагин обязан либо запускать процесс, либо давать UI.
- Имена sidecar'ов уникальны внутри плагина: по ним именуются data-dir, логи и health-записи.
- `exec`-пути не выходят за пределы директории плагина (`..` запрещён).
- `spec.config.schema` компилируется как JSON Schema — сломанная схема иначе всплыла бы только при рендере формы настроек.
- `keystoneCoreMax` не ниже `keystoneCoreMin`; `dependsOn` не содержит самого плагина.

Блок `ui` пока принимается как есть, без валидации: его формат задаёт [`plugin-ui-integration.md`](./plugin-ui-integration.md) и формализуют тикеты [UI]. Там же надо закрыть расхождение — этот документ кладёт схему настроек в `spec.config.schema`, а `plugin-ui-integration.md` §3.1 — в `ui.config.schema`. Каноном v1 считается `spec.config.schema`.

### 3.2 Trust tiers

Каждый плагин имеет один из четырёх уровней доверия, отображаемых в UI как badge:

| Trust tier | Badge | Кто пишет | Что проверяет keystone |
|---|---|---|---|
| `core` | 🟢 Green | core team keystone | подпись + полный ревью + прод-тест-плейбук |
| `verified` | 🔵 Blue | community, но прошёл review | подпись + автотесты + мануальное ревью |
| `experimental` | 🟡 Yellow | community | подпись, warning на установке |
| `unsafe` | 🔴 Red | сторонний источник | только `keystone plugin install --unsafe git://...` |

Только `core` + `verified` показываются в основной витрине store. `experimental` за флажком «Show community plugins». `unsafe` не появляется в витрине никогда — только явная установка через CLI.

## 4. Sidecar-протокол v1

Плагин коммуницирует с ядром через один Unix-socket (`$KEYSTONE_PLUGIN_SOCKET`) по JSON-RPC. Тот же протокол между плагином и его собственными sidecar'ами. Спецификация — отдельный документ `docs/sidecar-protocol-v1.md` (TBD).

Кратко, message-типы:

- **command** (ядро → плагин): `{id, method, params}` → `{id, result | error}`. Методы: `commission`, `decommission`, `discover`, `readState`, `writeState`, `invokeAction`, `subscribe`, `unsubscribe`, `getHealth`.
- **push** (плагин → ядро): `{topic, seq, payload}`. Topics: `state.<deviceRef>.<feature>.<key>`, `event.<deviceRef>.<eventName>`, `lifecycle.<deviceRef>.online|offline`, `commissioning.progress`, `plugin.health`.

Ключевые свойства v1:
- **`seq`** на каждом push → replay при reconnect.
- **`getHealth()`** для supervisor'а плагин-менеджера.
- **Structured errors**: `{code: "device.not_found", message: "...", retryable: false}`.
- **Heartbeat**: ping от ядра каждые 10 сек, timeout 30 сек → forced restart плагина.

Полная спека — в отдельном документе.

## 5. Lifecycle плагина

```
[ Discover ] ──install──> [ Configured ] ──enable──> [ Running ]
                                ↑                        │
                                └─────── disable ────────┘
                                         uninstall
                                          ↓
                                   [ Removed ]
```

Каждый переход — RPC-вызов plugin-manager'а. Manager супервайзит `entrypoint` через ОС-фичи (systemd child unit / proc-supervisor / launchd). При падении плагина — restart по политике из манифеста (`always | on-failure | never`), с exponential backoff.

## 6. Registry infrastructure

### 6.1 Официальный registry

Git-based, как Homebrew tap:

```
github.com/keystone/plugin-registry/
├── plugins/
│   ├── matter/
│   │   ├── plugin.yaml           # копия из плагин-репы + подпись core team
│   │   ├── versions/
│   │   │   ├── 1.0.0.yaml
│   │   │   ├── 1.1.0.yaml
│   │   │   └── 1.2.0.yaml
│   │   └── icon.svg
│   ├── zigbee2mqtt/
│   ├── ha-bridge/
│   └── ...
└── index.json                    # генерируется автоматически, скачивается клиентом
```

`keystone` при старте (и на команду `store update`) скачивает `index.json`, из него — карточки плагинов и ссылки на конкретные версионные архивы (`https://cdn.keystone.io/plugins/matter/1.2.0.tar.gz`).

### 6.2 Custom repositories

Пользователь может добавить свои источники:

```
$ keystone store tap https://raw.githubusercontent.com/some-org/my-registry
```

Плагины из custom registries помечаются badge'ем «external source», не автообновляются, всегда с warning.

### 6.3 Публикация плагина

Для нативных Store 1:
1. Автор форкает `plugin-registry`, добавляет свой манифест
2. PR в основную репу
3. Автоматика: валидация манифеста, автотесты, security-скан
4. Ревью core team → merge → появление в витрине

## 7. UX магазина

Основная витрина в keystone-UI:

- **Home** — «рекомендуем», «недавно установлены», «доступны обновления»
- **Categories**: Transports, Devices, Automations, AI, UI Extensions, Utilities
- **Search** — глобальный поиск с фильтрами по trust tier, устройствам, вендорам
- **Detail page**: описание, скриншоты, схема config'а, changelog, зависимости, resource-footprint

Внутри плагина `ha-bridge` открывается вторичная витрина (Store 2): 1400+ HA-интеграций сгруппированных по вендорам/категориям, с чётким предупреждением про best-effort quality и типичные ограничения (polling latency, cloud dependency).

## 8. Config flow

Плагины принимают пользовательскую настройку двумя способами:

- **JSON Schema в манифесте** (простые случаи: API-key + URL) → ядро автоматически рендерит форму
- **Custom UI-компонент** (сложные случаи: OAuth wizard, device pairing из нескольких шагов) → плагин поставляет `ui/config-flow.tsx`, keystone-UI встраивает через iframe/module-federation

Для `ha-bridge` — отдельный слой: HA-integrations используют HA's voluptuous config_flow, `ha-bridge` парсит их и ре-эмитит в наш формат.

## 9. Go SDK

Для плагин-авторов Store 1 публикуем `github.com/keystone/sdk-go`:

```go
package main

import "github.com/keystone/sdk-go"

func main() {
    plugin := sdk.NewPlugin("matter")

    plugin.OnStart(func(ctx sdk.Context) error {
        // инициализация, подключение sidecar'ов
    })

    plugin.OnCommission(func(ctx sdk.Context, req sdk.CommissionRequest) (sdk.DiscoveredDevice, error) {
        // multi-admin через 11-digit код
    })

    plugin.OnInvokeAction(func(ctx sdk.Context, ref sdk.TransportRef, feature, action string, params map[string]any) error {
        // On/Off/Toggle/…
    })

    plugin.OnSubscribe(func(ctx sdk.Context, sink sdk.EventSink) error {
        // подписываемся на изменения, sink.Push(...) для forward'а
    })

    plugin.Run() // блокирующий, слушает $KEYSTONE_PLUGIN_SOCKET
}
```

SDK инкапсулирует sidecar-протокол, reconnect, seq/replay, health, sandbox. Автор пишет только бизнес-логику.

## 10. HA Bridge — специфика

`ha-bridge` — самый нестандартный плагин из Store 1:

- **Bundle**: содержит Python venv + Home Assistant Core + наш кастом-компонент `keystone_bridge`.
- **Sidecar**: `hass --config /var/lib/keystone/plugins/ha-bridge/config` — headless HA, слушающий unix socket, не открывающий 8123 наружу.
- **Store 2**: витрина внутри детальной страницы плагина. Установка HA-интеграции = вызов `config_flow.async_init` в HA под капотом, парсинг схемы, рендер формы в keystone-UI.
- **Обновления**: релизы `ha-bridge` = свежий HA-runtime + куратор-выборка HA-интеграций. Пиним LTS-версии HA (раз в квартал).
- **Attribution**: в About-странице плагина открытым текстом «Powered by Home Assistant Core, Apache 2.0». Никакого стыда.

## 11. CLI

Плагин-менеджер полностью управляем через CLI (см. `docs/keystone-cli-spec.md`). Ключевые команды:

```
$ keystone install matter                       # brew-style shortcut
$ keystone plugin list
$ keystone plugin update ha-bridge
$ keystone plugin config matter set fabricLabel "Home"
$ keystone plugin logs matter --tail
$ keystone plugin new my-plugin                 # dev: scaffold
$ keystone plugin publish                       # dev: PR в registry
```

CLI, HTTP API и gRPC — три равноправные точки входа с одинаковым набором операций.

## 12. Дорожная карта

Фазы (объединено с общей keystone roadmap):

**Phase A — Matter hardening** (~1 неделя)
Доделать гэпы из `matter-implementation-gaps.md`. Цель — Matter надёжно работает end-to-end как «встроенный» транспорт. Считаем это этапом «доводки первого плагина», но пока внутри ядра.

**Phase B — Plugin architecture v1** (~2 недели)
- Формализовать sidecar-protocol v1 (`docs/sidecar-protocol-v1.md`)
- Формализовать plugin manifest v1 (уточнить схему из §3)
- Реализовать plugin-manager в ядре: discovery, lifecycle, sandbox, restart policy
- Добавить `/plugins/*` API endpoint'ы

**Phase C — Matter extraction** (~1 неделя)
- Вынести `internal/adapters/matter/` + `sidecars/matter-server/` в отдельную репу `keystone-plugin-matter`
- Ядро больше не знает про Matter
- Установка Matter через `keystone plugin install matter`

**Phase D — Registry & Store UI** (~2 недели)
- Git-based registry (первый tap: `github.com/keystone/plugin-registry`)
- Store UI в keystone (браузер, install/update/uninstall, config-flow парсер для простых JSON Schema)
- CLI-команды `plugin *`, `store *`

**Phase E — SDK & second plugin** (~1 неделя)
- Go SDK (`github.com/keystone/sdk-go`)
- CLI `keystone plugin new/build/publish`
- Второй нативный плагин как валидация модели (кандидаты: DIRIGERA прямой, HomeKit-bridge)

**Phase F — HA Bridge** (~4 недели)
- Bundle HA-runtime + `keystone_bridge` custom_component
- Store 2 (витрина HA-интеграций) в UI детальной страницы плагина
- Config-flow парсер под топ-20 HA-интеграций
- Пилотный запуск для ранних пользователей

**Итого до keystone 1.0 с полным store'ом:** ~11 недель после текущей точки.

## 13. Открытые вопросы

- **Custom repositories vs. централизованный store** — начинаем с одного официального registry; custom taps на этапе E-F.
- **Монетизация плагинов** — не в MVP. Долгосрочно — premium tier для core team plugins, revenue-share для verified authors.
- **WASM-плагины** — оставляем на v2. Sidecar-модель проще, безопаснее для старта.
- **Автообновление** — в MVP делаем нотификацию, авто-apply только для security-фиксов. Мажоры — опт-ин.
- **Совместимость плагинов между версиями ядра** — `keystoneCoreMin/Max` в манифесте + автотесты в CI registry.
- **Isolation** — насколько жёстко sandbox'ить плагины (cgroups? namespaces? capabilities?). Стартуем с process isolation + resource limits; полная sandbox-модель — post-1.0.
