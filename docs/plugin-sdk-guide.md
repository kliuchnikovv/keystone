# Keystone Plugin SDK — гайд для разработчиков

**Статус:** v0.1
**Родительские документы:**
- [`plugin-store-architecture.md`](./plugin-store-architecture.md) — модель плагинов и trust tiers.
- [`sidecar-protocol-v1.md`](./sidecar-protocol-v1.md) — транспортный контракт.
- [`plugin-ui-integration.md`](./plugin-ui-integration.md) — UI-интеграция плагинов.

## 1. Цель

Барьер входа для community-плагина — три часа реальной работы. Автор Go/Node/Python со средним опытом:
- изучает Getting Started (~15 мин)
- запускает `keystone plugin new` (~1 мин)
- пишет бизнес-логику своего плагина (~2 часа)
- запускает тесты (~30 мин)
- открывает PR в registry (~5 мин)

Достигается семью артефактами, описанными ниже.

## 2. Артефакты

| # | Артефакт | Репозиторий | Роль |
|---|---|---|---|
| 1 | `sdk-go` | `github.com/keystone/sdk-go` | Go-обёртка над Sidecar Protocol v1 |
| 2 | `sdk-node` | `github.com/keystone/sdk-node` | Node.js-обёртка |
| 3 | `sdk-python` | `github.com/keystone/sdk-python` | Python-обёртка |
| 4 | `plugin-template` | `github.com/keystone/plugin-template` | Клонируемый шаблон нового плагина |
| 5 | `plugin-matter` | `github.com/keystone/plugin-matter` | Reference plugin (сложный, с sidecar'ом) |
| 6 | `plugin-dirigera` | `github.com/keystone/plugin-dirigera` | Reference plugin (простой, pure Go) |
| 7 | `docs.keystone.io` | `github.com/keystone/docs` | Портал документации |

Плюс инфраструктура:

| # | Артефакт | Репозиторий | Роль |
|---|---|---|---|
| 8 | `plugin-registry` | `github.com/keystone/plugin-registry` | Официальный git-based tap |
| 9 | `protocol` | `github.com/keystone/protocol` | JSON Schemas, protobuf-эквиваленты |
| 10 | `design` | `keystone/packages/design/` (workspace) → npm `@keystone/design` | Web Components + CSS-tokens; см. `plugin-ui-integration.md` §8 |

## 3. SDK-Go — центральный API

### 3.1 Установка

```
go get github.com/keystone/sdk-go@latest
```

### 3.2 Minimal working plugin

```go
package main

import (
    "context"
    "log"

    "github.com/keystone/sdk-go"
)

func main() {
    p := sdk.NewPlugin("my-plugin")

    p.OnStart(func(ctx sdk.Context) error {
        log.Println("plugin started")
        return nil
    })

    p.OnDiscover(func(ctx sdk.Context, out sdk.DiscoveredDeviceSink) error {
        out.Push(sdk.DiscoveredDevice{
            Ref:  "my-plugin:demo-1",
            Type: "light",
            Name: "Demo Light",
            Features: []sdk.Feature{
                sdk.OnOffFeature(),
                sdk.BrightnessFeature(),
            },
        })
        return nil
    })

    p.OnInvokeAction(func(ctx sdk.Context, ref sdk.TransportRef,
        feature, action string, params map[string]any) error {
        log.Printf("action: %s.%s on %s", feature, action, ref)
        return nil
    })

    if err := p.Run(); err != nil {
        log.Fatal(err)
    }
}
```

`p.Run()` — блокирующий, слушает `$KEYSTONE_PLUGIN_SOCKET`, обрабатывает handshake, reconnect, seq/replay, heartbeat. Автор пишет только handler'ы.

### 3.3 Feature builders

Инкапсулируют типовые Feature+State+Action комбинации:

```go
sdk.OnOffFeature()                       // FeatureOnOff + turn_on/turn_off/toggle
sdk.BrightnessFeature()                  // FeatureBrightness + set (0..100)
sdk.ColorTempFeature(2000, 6500)         // FeatureColorTemp + set (kelvin range)
sdk.RGBFeature()                         // FeatureColor + set (xy)
sdk.TemperatureFeature()                 // read-only sensor
sdk.MotionFeature()
sdk.PowerMeterFeature()
sdk.CustomFeature("my_feature", ...)     // всё, что не покрыто builder'ами
```

### 3.4 Push events

Плагин пушит state changes через sink:

```go
p.OnSubscribe(func(ctx sdk.Context, sink sdk.EventSink) error {
    // long-lived: подписываемся на upstream (hub / cloud / hardware),
    // конвертируем изменения в sdk.StateChanged event'ы.

    upstream, err := connectToHub(ctx)
    if err != nil { return err }

    for evt := range upstream.Events() {
        sink.PushState(sdk.StateChanged{
            Ref:     sdk.TransportRef(evt.DeviceID),
            Feature: "onoff",
            Key:     "value",
            Value:   evt.NewValue,
            Origin:  sdk.OriginDeviceReport,
        })
    }
    return nil
})
```

`sink.PushState` / `sink.PushEvent` / `sink.PushLifecycle` — типизированные хелперы. seq/timestamp/replay-buffer управляются SDK.

### 3.5 Errors

Плагин возвращает typed errors, SDK транслирует в structured error по protocol'у:

```go
return sdk.ErrDeviceNotFound.WithDetails(map[string]any{"nodeId": id})
return sdk.ErrAuthExpired
return sdk.ErrNetworkTimeout.Wrap(err)      // добавляет исходную ошибку в details
return sdk.NewError("my.custom.code", "message", sdk.Retryable(true))
```

### 3.6 Config

```go
type MyConfig struct {
    APIKey      string `json:"apiKey"`
    PollInterval int   `json:"pollInterval"`
}

p.OnConfig(func(ctx sdk.Context, cfg MyConfig) error {
    log.Printf("config: %+v", cfg)
    return nil
})
```

SDK декодирует JSON согласно generic type, вызывает handler при старте и после каждого `setConfig` от ядра.

### 3.7 Config Flow

Многошаговые wizard'ы (см. `plugin-ui-integration.md` §Layer 2):

```go
p.OnConfigFlow(func(ctx sdk.Context, step string, data map[string]any) (sdk.ConfigFlowStep, error) {
    switch step {
    case "init":
        return sdk.FormStep("Введите API-ключ").
            AddString("apiKey", sdk.Required(true), sdk.Password(true)).
            Next("verify"), nil
    case "verify":
        if err := verify(data["apiKey"].(string)); err != nil {
            return sdk.ErrorStep("Ключ неверен").Retry("init"), nil
        }
        return sdk.CompleteStep("Готово. Устройства подгружаются…"), nil
    }
    return sdk.ConfigFlowStep{}, sdk.ErrValidationBadParams
})
```

### 3.8 Secrets

Секреты через SDK, а не через `os.Getenv`:

```go
apiKey, err := ctx.Secrets().Get("upstream.api_key")
if err != nil { return err }

// Секретов НЕТ в конфиге — они в секретсторе ядра, зашифрованы, никогда в логах.
```

### 3.9 Sidecars

Если плагин запускает свои под-процессы (matter.js, python-runtime, whisper), объявляет их в манифесте — SDK сам их супервайзит:

```go
// Ничего не пишем — sidecars запускаются автоматически.
// Плагин видит их через ctx.Sidecar("matter-server"):

conn := ctx.Sidecar("matter-server")
result, err := conn.Call(ctx, "commission", params)
```

## 4. `keystone plugin new` — начало

```
$ keystone plugin new my-vendor
? Language:               Go (default) / Node.js / Python
? Category:               transport / device / automation / ai / ui
? License:                Apache-2.0 (default) / MIT / GPL-3.0
? Include sidecars:       No (default) / Node.js / Python
? Include custom UI:      No (default) / Web Component / Iframe page

Creating plugin from template...
Cloned into ./my-vendor/
Initialized git, added remote origin: (not set)

Next:
  cd my-vendor
  keystone plugin build
  keystone plugin test
  keystone plugin install --local .
```

### 4.1 Директория из шаблона

```
my-vendor/
├── plugin.yaml
├── go.mod
├── main.go
├── internal/
│   └── adapter/
│       ├── adapter.go
│       ├── adapter_test.go
│       └── mock_upstream.go
├── tests/
│   └── e2e_test.go
├── ui/                     # если выбран custom UI
│   └── device-detail.js
├── sidecars/               # если выбран sidecar
├── .github/workflows/
│   ├── test.yaml
│   ├── build.yaml
│   └── publish.yaml
├── LICENSE
├── README.md
├── icon.svg
└── screenshots/
```

Каждый файл в шаблоне — работающий пример. Автор постепенно заменяет своим кодом.

## 5. `keystone plugin build` — сборка

```
$ keystone plugin build

Validating manifest... ok
Building Go binary (linux/amd64, linux/arm64, darwin/arm64)... done
Building sidecar matter-server (node20)... done
Bundling UI assets... done
Packaging... done

Output: dist/plugin-my-vendor-0.1.0.tar.gz (23.4 MB)
```

Форматы вывода:
- `.tar.gz` — стандартный distribution artifact
- `.deb` (linux-arm64/amd64) — опционально
- Directory (для local install через `--local .`)

Cross-compile из коробки для всех архитектур, которые keystone поддерживает: `linux/amd64`, `linux/arm64` (Pi), `darwin/arm64` (Mac dev).

## 6. `keystone plugin test` — testing

Три уровня тестов:

### 6.1 Unit tests (в шаблоне из коробки)

```go
// internal/adapter/adapter_test.go
func TestOnOffTurnOn(t *testing.T) {
    adapter := &MyAdapter{upstream: NewMockUpstream()}
    err := adapter.InvokeAction("device1", "onoff", "turn_on", nil)
    require.NoError(t, err)
    require.True(t, adapter.upstream.LastCommand.IsTurnOn())
}
```

Обычные Go-тесты, никакой магии.

### 6.2 SDK-integration tests

Через `sdk.MockCore` — mock keystone-ядра, позволяющий тестировать плагин end-to-end без реального keystone:

```go
func TestPluginE2E(t *testing.T) {
    plugin := setupMyPlugin(t)

    core := sdk.NewMockCore(t)
    core.RunPlugin(plugin)

    // Симулируем discover
    devices, err := core.Call("discover", nil)
    require.NoError(t, err)
    require.Len(t, devices, 3)

    // Симулируем commission
    _, err = core.Call("commission", CommissionParams{Payload: "test-code"})
    require.NoError(t, err)

    // Ждём push события
    ev := core.WaitPush("state.**", 5*time.Second)
    require.Equal(t, "onoff", ev.Feature)
}
```

Крутится в CI автоматически.

### 6.3 Real-keystone e2e

```
$ keystone plugin test --e2e
Starting isolated keystone instance...
Installing plugin from local build...
Running e2e scenarios...
  ✓ commission flow
  ✓ state read
  ✓ subscribe + push
  ✓ reconnect + replay
  ✓ graceful shutdown
All passed. (12.3s)
```

Изолированный keystone-instance в `$TMPDIR/keystone-plugin-e2e-*`, автоматически убирается после теста.

## 7. `keystone plugin install --local` — локальный тест

```
$ keystone plugin install --local .
Installing my-vendor@0.1.0-dev from local build...
Signature check: skipped (--local)
Warning: local plugins have trust tier `unsafe`
Enabling... ok
Plugin `my-vendor` running at http://localhost:7777/plugins/my-vendor
```

Живой инстанс на своей машине. Full-cycle разработка — hot-reload при пересборке.

## 8. `keystone plugin publish` — публикация

```
$ keystone plugin publish
Validating manifest... ok
Running tests... ok
Signing artifacts...
  Using Sigstore keyless (github.com/my-vendor/plugin-my-vendor identity)
  Signed: dist/plugin-my-vendor-0.1.0.tar.gz.sig
Pushing artifacts to release...
Opening PR in keystone/plugin-registry...
PR opened: https://github.com/keystone/plugin-registry/pull/342

Next:
  Await CI checks (~5 min)
  Address review feedback if any
  On merge: plugin available in official store
```

### 8.1 Registry CI checks

При открытии PR запускается автоматика:

1. **Manifest validation** — JSON Schema, required fields, versioning consistency
2. **Security scan** — govulncheck (Go), npm audit (sidecars), базовый SAST
3. **Signature verification** — Sigstore verify
4. **Integration test** — SDK MockCore-тесты запускаются в registry-CI на свежесобранном артефакте
5. **Trust tier check** — новые плагины автоматически `experimental`. Промоушен в `verified` — по review core team.

### 8.2 Ревью процесс

- **`core` tier** — только core team, полный code review, security-аудит.
- **`verified` tier** — 1+ approve от maintainer'а другого verified-плагина, CI зелёный, лицензия ок, автор известен.
- **`experimental` tier** — только CI зелёный. Автомерж возможен через 7 дней без review.

## 9. Публикация в custom registry

Если автор не хочет через официальный registry (например, приватная интеграция для компании):

```
# my-registry (git repo)
├── plugins/
│   ├── my-corp-integration/
│   │   ├── plugin.yaml
│   │   └── versions/1.0.0.yaml
```

Пользователь подключает:

```
$ keystone store tap https://raw.githubusercontent.com/my-corp/registry/main
$ keystone plugin install my-corp-integration
```

Автоматически помечается badge'ем «external source», не автообновляется без явного согласия.

## 10. Документационный портал

`docs.keystone.io`, структура:

```
docs/
├── getting-started/
│   ├── first-plugin.md            # tutorial "Your first plugin in 15 minutes"
│   ├── development-setup.md
│   └── testing.md
├── guides/
│   ├── config-flow.md
│   ├── secrets.md
│   ├── polling-integrations.md
│   ├── realtime-integrations.md
│   ├── ui-web-components.md
│   ├── ui-iframe-pages.md
│   └── publishing.md
├── reference/
│   ├── manifest.md                # spec plugin.yaml
│   ├── sidecar-protocol-v1.md
│   ├── sdk-go/                    # godoc
│   ├── sdk-node/
│   ├── sdk-python/
│   ├── errors.md                  # error taxonomy
│   └── design-system.md           # link to storybook
├── plugins/                       # ecosystem overview
│   ├── matter.md                  # walkthrough reference plugins
│   └── dirigera.md
└── contributing/
    ├── code-of-conduct.md
    ├── security.md
    └── trust-tiers.md
```

Хостинг — GitHub Pages сначала, Docusaurus позже. Каждый merge в `docs` main → auto-deploy.

## 11. Non-Go SDKs

Одинаковый API-контракт, идиоматичный код в целевом языке.

### 11.1 SDK-Node (TypeScript)

```typescript
import { Plugin, OnOffFeature, StateChanged } from '@keystone/sdk';

const p = new Plugin('my-plugin');

p.onDiscover(async () => [
  {
    ref: 'my-plugin:demo-1',
    type: 'light',
    features: [OnOffFeature()],
  },
]);

p.onInvokeAction(async (ref, feature, action, params) => {
  console.log(`action: ${feature}.${action} on ${ref}`);
});

p.onSubscribe(async (sink) => {
  // long-lived subscribe
  upstream.on('change', (evt) => {
    sink.pushState({ ref: evt.id, feature: 'onoff', key: 'value', value: evt.value });
  });
});

await p.run();
```

### 11.2 SDK-Python (для HA-Bridge и ML-плагинов)

```python
from keystone_sdk import Plugin, OnOffFeature

p = Plugin("my-plugin")

@p.on_discover
async def discover():
    return [{
        "ref": "my-plugin:demo-1",
        "type": "light",
        "features": [OnOffFeature()],
    }]

@p.on_invoke_action
async def invoke(ref, feature, action, params):
    print(f"action: {feature}.{action} on {ref}")

p.run()
```

## 12. Reference plugins как учебный материал

### 12.1 `plugin-matter` (сложный, с sidecar'ом)

Демонстрирует:
- Управление под-процессом (matter.js через WS)
- Realtime push (peer-attribute changes)
- Commissioning с multi-step flow
- Custom UI: device-detail component для Matter lights
- Persistent storage (Matter fabric на диске)

### 12.2 `plugin-dirigera` (простой, pure Go)

Демонстрирует:
- Pure Go — только SDK + HTTP-клиент к DIRIGERA hub'у
- Polling + push (DIRIGERA поддерживает WS)
- Auth flow с pairing button
- Отсутствие sidecar'ов

Оба разобраны по шагам в documentation portal (`docs/plugins/*.md`).

## 13. Порядок работ

Вписываем в общую roadmap:

- **Phase B** (~2 недели): Sidecar Protocol v1 spec (`sidecar-protocol-v1.md`), JSON Schemas, `keystone/protocol` репозиторий.
- **Phase E** (~1 неделя): `sdk-go` v0.1, CLI-scaffold (`plugin new/build/test/install --local/publish`), template repo.
- **Phase E+** (~1 неделя): `plugin-matter` и `plugin-dirigera` — extraction from core.
- **Phase D+** (~1 неделя): Registry CI (validation, security scan, signing), documentation portal.
- **Post-v1** (~2-3 недели): `sdk-node`, `sdk-python`. Обвязка вокруг них, туториалы.

## 14. Метрики успеха

Не для v1, но целевые:

- **Time to first plugin** — от clone template до running local: < 15 минут.
- **Time to publish** — от готового кода до merged PR: < 24 часа (CI + auto-merge при зелёном для experimental).
- **Community plugins в первый год** — 30+ verified, 100+ experimental.
- **Coverage** — 80% типовых smart-home устройств покрыты либо native plugins, либо через ha-bridge.

## 15. Открытые вопросы

- **Плагин-signing UX**: Sigstore keyless требует GitHub Actions run. Для solo-разработчика без CI — нужен fallback (GPG key upload)?
- **Monorepo plugin support**: несколько плагинов в одной репе, sharing common code. Support в CLI: `keystone plugin build --path ./plugins/matter/`. Пока — YAGNI.
- **Plugin dependencies**: плагин A требует плагин B (например, ha-bridge требует secretstore). Резолвить через manifest `spec.dependsOn` в plugin-manager'е.
- **Semantic versioning enforcement**: как реагировать на breaking changes в плагине? Проверять по exported API? Пока — только по major bump'у в manifest.
- **Multi-language plugins**: плагин на Go + собственный Node.js sidecar — уже покрыто (SDK управляет). Плагин на Rust + Python sidecar — тоже должно работать (SDK Rust появится по community-запросу).
