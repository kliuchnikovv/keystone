# Keystone Plugin UI Integration

**Статус:** v0.1
**Родительские документы:**
- [`plugin-store-architecture.md`](./plugin-store-architecture.md) — модель плагинов.
- [`sidecar-protocol-v1.md`](./sidecar-protocol-v1.md) — как плагин коммуницирует с ядром.
- [`plugin-sdk-guide.md`](./plugin-sdk-guide.md) — как писать плагин.
- [`ui-design-brief.md`](./ui-design-brief.md) — дизайн-система keystone-UI.

## 1. Цель

Плагин может добавлять UI, который **выглядит и ведёт себя как часть keystone**. Не бортовой iframe с рамкой, не «плагиновский раздел» в стороне — а первоклассные экраны, компоненты и wizard'ы, стилистически неотличимые от core-UI.

При этом плагин-автор:
- Не обязан знать наш React-стек.
- Не обязан ничего собирать (build step) для простых случаев.
- Использует те же дизайн-токены, компоненты, поведение.
- Работает через один API-runtime.

## 2. Четырёхслойная модель

Автор выбирает **минимальный слой**, который решает его задачу. Каждый следующий слой даёт больше свободы, но требует больше кода.

| Layer | Что | Покрытие плагинов | Кода от автора |
|---|---|---|---|
| 1 | Auto-form из JSON Schema | ~60% | 0 строк — только манифест |
| 2 | Декларативный Config Flow | ~25% | Go/JS/Python handler на step'ах |
| 3 | Web Components (device detail, cards) | ~12% | JS-модуль с custom element |
| 4 | Iframe с postMessage (escape hatch) | ~3% | Полный HTML/CSS/JS плагина |

Все четыре слоя — **не эксклюзивы**. Плагин может использовать Layer 1 для settings + Layer 3 для device detail + Layer 4 для аналитической страницы одновременно.

## 3. Layer 1 — Auto-form из JSON Schema

Простые настройки: API-key, URL, интервал опроса, флаги.

### 3.1 Как

Автор описывает JSON Schema в манифесте:

```yaml
# plugin.yaml
ui:
  config:
    schema: |
      {
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "properties": {
          "apiKey": {
            "type": "string",
            "format": "password",
            "title": "API-ключ Yandex",
            "description": "Получить на dev.yandex.ru → создать приложение"
          },
          "pollInterval": {
            "type": "integer",
            "default": 30,
            "minimum": 5,
            "maximum": 300,
            "title": "Интервал опроса (сек)"
          },
          "region": {
            "type": "string",
            "enum": ["ru", "kz", "by"],
            "title": "Регион"
          }
        },
        "required": ["apiKey", "region"]
      }
```

Ядро рендерит форму, используя наши компоненты:

- `type: "string"` + `format: "password"` → `<ks-input type="password">` + secretstore-badge
- `type: "string"` + `enum` → `<ks-select>`
- `type: "integer"` → `<ks-number>`
- `type: "boolean"` → `<ks-toggle>`
- `type: "array"` → `<ks-list>` с add/remove
- `type: "object"` → nested group

### 3.2 UI hints (расширение стандарта)

```json
"apiKey": {
  "type": "string",
  "format": "password",
  "ui:widget": "secret",           // хранить в секретсторе, не в config
  "ui:visibleIf": "region != null"  // condition-based visibility
}
```

Полный список hint'ов — в `docs/reference/manifest.md`.

### 3.3 Валидация

- **Клиент**: schema-based на форме, ошибки на полях.
- **Сервер**: та же schema перед `setConfig` вызовом на плагин.
- Плагин может дополнительно валидировать в handler'е `OnConfig` и вернуть `ErrValidationBadParams`.

### 3.4 Ограничения

- Только форма — никаких мульти-шагов, никаких OAuth.
- Никакой custom render логики (условная логика — только через `ui:visibleIf`).
- Достаточно для 60% плагинов.

## 4. Layer 2 — Declarative Config Flow

Multi-step wizard'ы: OAuth, device pairing, discovery-then-pick, миграции.

### 4.1 Модель

Плагин выдаёт шаги через RPC `configFlow`. Ядро хранит state между step'ами, рендерит текущий step, передаёт data при переходе на следующий.

```
[user opens setup] → core → plugin.configFlow(step="init", data={})
                              ↓
                     plugin returns Step { type: "form", schema, next: "verify" }
                              ↓
                     core renders form using Layer 1 renderer
                              ↓
                     [user submits] → core → plugin.configFlow(step="verify", data={...})
                              ↓
                     plugin returns Step { type: "form", schema, next: "complete" }
                              ↓
                     ... [more steps] ...
                              ↓
                     plugin returns Step { type: "complete", message }
                              ↓
                     core saves config, closes wizard
```

### 4.2 Пример (Yandex OAuth)

```go
p.OnConfigFlow(func(ctx sdk.Context, step string, data map[string]any) (sdk.ConfigFlowStep, error) {
    switch step {
    case "init":
        return sdk.OAuthStep{
            Title:       "Подключить Yandex-аккаунт",
            Provider:    "yandex",
            AuthURL:     "https://oauth.yandex.ru/authorize?...",
            RedirectURI: ctx.OAuthRedirectURI(),
            Next:        "select-home",
        }, nil

    case "select-home":
        token := data["oauth_token"].(string)
        homes, err := listHomes(ctx, token)
        if err != nil {
            return sdk.ErrorStep("Не удалось получить список домов").Retry("init"), nil
        }
        return sdk.FormStep("Выберите дом").
            AddSelect("homeId", sdk.Options(homes)).
            Next("complete"), nil

    case "complete":
        return sdk.CompleteStep(fmt.Sprintf("Подключено %d устройств", n)), nil
    }
    return sdk.ConfigFlowStep{}, sdk.ErrValidationBadParams
})
```

### 4.3 Step types

| Type | Render | Пример использования |
|---|---|---|
| `form` | JSON Schema → форма (тот же renderer что Layer 1) | login/password, select from list |
| `oauth` | «Открыть провайдера в новом окне», ловля callback | Yandex, Google, Apple auth |
| `qr-scan` | Наш QR-scanner компонент, возвращает decoded string | Matter QR-код, Zigbee pairing QR |
| `progress` | Прогресс-бар + updates от плагина через push | commissioning, discovery |
| `confirm` | Простой accept/decline экран с текстом | «Устройство найдено, добавить?» |
| `pick-device` | Список найденных устройств с checkbox'ами | pair нескольких zigbee-устройств за раз |
| `manual-action` | Инструкция + кнопка «готово» | «Нажмите кнопку на устройстве и подтвердите» |
| `error` | Красная иконка + текст + опциональный retry-step | Auth failed, network unreachable |
| `complete` | Зелёная галочка + текст | Успешное завершение |

Каждый type имеет свой JSON Schema для параметров, ядро валидирует.

### 4.4 Push-updates во время step'а

Некоторые step'ы (`progress`) обновляются от плагина:

```go
sink.PushConfigFlowUpdate(sdk.ConfigFlowUpdate{
    SessionID: ctx.ConfigFlowSessionID(),
    Progress:  0.35,
    Message:   "Договариваемся с устройством...",
})
```

Ядро форвардит в UI, прогресс-бар анимируется в реальном времени. Тот же механизм, что мы уже используем для Matter commissioning'а, только формализованный.

### 4.5 HA-Bridge как заказчик Layer 2

Плагин `ha-bridge` в своём Python-коде парсит HA's voluptuous `config_flow`, транслирует в наш `ConfigFlowStep` последовательно. Пользователь настраивает HA-интеграцию через наш UI, не подозревая что под капотом идёт HA-flow. Один универсальный wizard-компонент — 1400+ доступных интеграций.

## 5. Layer 3 — Web Components

Custom UI для конкретного device-type или feature: термостат с недельным расписанием, media player controls, camera viewport, кастомный dashboard-виджет.

### 5.1 Регистрация в манифесте

```yaml
ui:
  # Detail-страница для устройства с deviceType="thermostat"
  deviceDetail:
    - matchDeviceType: thermostat
      element: ks-thermostat-detail
      source: ui/thermostat-detail.js

  # Компакт-карточка в списке устройств для feature="media_player"
  deviceCard:
    - matchFeature: media_player
      element: ks-media-card
      source: ui/media-card.js

  # Дашборд-виджет (появляется в галерее виджетов пользовательского dashboard'а)
  dashboardWidget:
    - id: energy-summary
      title: "Энергопотребление"
      element: ks-energy-widget
      source: ui/energy-widget.js
      sizes: ["small", "medium", "large"]
```

### 5.2 Пример компонента

```js
// ui/thermostat-detail.js
import { LitElement, html, css } from 'https://cdn.keystone.io/design/v1/deps/lit.js';
import '@keystone/design';   // резолвится в import-map, загружается один раз

class ThermostatDetail extends LitElement {
  static properties = {
    deviceId: { type: String, attribute: 'device-id' },
    state:    { state: true },
    schedule: { state: true },
  };

  static styles = css`
    :host {
      display: block;
      padding: var(--ks-space-md);
      color: var(--ks-color-text);
      font-family: var(--ks-font-body);
    }

    .setpoint {
      font: var(--ks-font-heading-xl);
      text-align: center;
      padding: var(--ks-space-lg) 0;
    }

    .row {
      display: flex;
      gap: var(--ks-space-sm);
      align-items: center;
    }
  `;

  connectedCallback() {
    super.connectedCallback();
    this._unsub = window.keystone.subscribe(
      `state.${this.deviceId}.**`,
      (evt) => { this.state = { ...this.state, [evt.feature]: evt.value }; }
    );
    this._loadInitial();
  }

  disconnectedCallback() {
    this._unsub?.();
    super.disconnectedCallback();
  }

  async _loadInitial() {
    const device = await window.keystone.devices.get(this.deviceId);
    this.state = device.state;
  }

  async _onSetpoint(newValue) {
    await window.keystone.devices.setState(
      this.deviceId, 'thermostat', 'target_temp', newValue
    );
  }

  render() {
    if (!this.state) return html`<ks-spinner></ks-spinner>`;
    return html`
      <div class="setpoint">${this.state.target_temp}°</div>
      <ks-slider
        min="15" max="28" step="0.5"
        value=${this.state.target_temp}
        @change=${e => this._onSetpoint(e.detail.value)}
      ></ks-slider>
      <div class="row">
        <ks-label>Режим</ks-label>
        <ks-select
          .options=${['off', 'heat', 'cool', 'auto']}
          .value=${this.state.mode}
          @change=${e => this._onMode(e.detail.value)}
        ></ks-select>
      </div>
    `;
  }
}

customElements.define('ks-thermostat-detail', ThermostatDetail);
```

### 5.3 Что делает интеграцию seamless

**Design tokens как CSS custom properties.** Ядро определяет:
```css
:root {
  --ks-color-primary: #ff8f47;
  --ks-color-text: #f5f5f5;
  --ks-color-surface: #1a1a1a;
  --ks-space-xs: 4px;
  --ks-space-sm: 8px;
  --ks-space-md: 16px;
  --ks-space-lg: 24px;
  --ks-font-body: 'Inter', sans-serif;
  --ks-font-heading-xl: 700 48px/1.1 'Playfair Display', serif;
  /* ... */
}

@media (prefers-color-scheme: light) {
  :root { /* light overrides */ }
}
```

Плагин просто использует `var(--ks-*)`. Автоматически подхватывает dark/light mode, юзер-кастомизацию (при её появлении), обновления brand.

**Web Component library.** `@keystone/design` экспортирует 30+ компонент:
- Атомы: `<ks-button>`, `<ks-input>`, `<ks-select>`, `<ks-toggle>`, `<ks-slider>`, `<ks-icon>`, `<ks-label>`, `<ks-spinner>`, `<ks-badge>`
- Молекулы: `<ks-card>`, `<ks-modal>`, `<ks-toast>`, `<ks-tabs>`, `<ks-list>`, `<ks-avatar>`, `<ks-tooltip>`, `<ks-color-picker>`, `<ks-temperature-picker>`, `<ks-time-picker>`, `<ks-schedule-grid>`
- Специфичные: `<ks-device-icon>`, `<ks-room-picker>`, `<ks-feature-control>` (auto-render любого feature)

Плагин может использовать любую подмножество.

**Shadow DOM.** Каждый Web Component изолирует CSS. Стили плагина не текут в keystone-UI, стили keystone-UI не текут в плагин (кроме custom properties, которые сквозь shadow проходят). Изоляция без iframe.

**`window.keystone` runtime API** — единый мост к ядру (см. §7).

### 5.4 Bundle strategy

Плагин поставляет ES-модуль (`ui/*.js`). Загружается на demand когда ядру нужен компонент. Если плагин весит 500KB — грузится один раз, кешируется.

Import maps для shared deps:
```html
<!-- в keystone-UI -->
<script type="importmap">
{
  "imports": {
    "@keystone/design": "/design/v1/index.js",
    "https://cdn.keystone.io/design/v1/deps/lit.js": "/design/v1/deps/lit.js"
  }
}
</script>
```

Все плагины делят один экземпляр `@keystone/design` и `lit`. Один download на всё.

### 5.5 Fallback

Если плагин custom-компонент падает (throw в `render()`, network error при загрузке модуля), ядро откатывается на default-рендер устройства (Layer 1 auto-form + generic controls) с warning-баннером.

## 6. Layer 4 — Iframe с postMessage

Escape hatch для плагинов, которым Web Components тесноваты: свой React/Vue стек, существующий большой UI, специфичные библиотеки (три.js, WebGL, RTC-video).

### 6.1 Регистрация

```yaml
ui:
  customPages:
    - path: analytics
      title: "Аналитика энергопотребления"
      icon: chart-line
      source: ui/analytics.html
      # CSP — обязательно, дефолт жёсткий
      csp: "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:"
      # permissions запрашивают у пользователя на установке
      permissions:
        - ui.external-fetch: api.grafana.example.com
```

Доступно как `http://keystone.local:7777/plugins/my-plugin/analytics`.

### 6.2 UX

Iframe всегда в **нашей внешней рамке**:
- Наш header (breadcrumbs, back-button, plugin name)
- Наш left-nav
- Внутри рамки — плагин полностью хозяин

Визуально ясно что это плагин (не default-UI keystone), но не outsourced-feel.

### 6.3 postMessage мост

```js
// внутри iframe плагина
window.parent.postMessage({
  type: 'keystone.rpc.request',
  id: 'req-1',
  method: 'devices.list',
  params: {},
}, '*');

window.addEventListener('message', (evt) => {
  if (evt.data.type === 'keystone.rpc.response' && evt.data.id === 'req-1') {
    console.log('devices:', evt.data.result);
  }
});
```

Ядро валидирует origin, применяет permission-checks, форвардит в `window.keystone` API (тот же, что для Layer 3).

Мы поставляем `@keystone/iframe-runtime` — маленькая либа-обёртка над postMessage, эмулирующая `window.keystone` внутри iframe:

```js
import { keystone } from '@keystone/iframe-runtime';

const devices = await keystone.devices.list();
```

### 6.4 Когда использовать

Только если Layer 3 категорически не подходит. В docs честно предупреждаем: iframe не рекомендуется для основного UX плагина, он визуально заметен, стилистически будет расходиться.

## 7. `window.keystone` runtime API

Единый глобальный объект для UI-модулей плагинов (Layer 3 и Layer 4 via bridge).

```typescript
interface Keystone {
  // === Устройства ===
  devices: {
    list(filter?: DeviceFilter): Promise<Device[]>;
    get(id: string): Promise<Device>;
    invoke(id: string, feature: string, action: string, params?: object): Promise<any>;
    setState(id: string, feature: string, key: string, value: any): Promise<void>;
    readState(id: string, feature: string, key: string): Promise<StateValue>;
  };

  // === Real-time subscriptions ===
  subscribe(topic: string | string[], cb: (event: Event) => void): Unsubscribe;

  // === Config текущего плагина ===
  config: {
    get<T = any>(key?: string): Promise<T>;
    set(key: string, value: any): Promise<void>;
  };

  // === Secrets текущего плагина ===
  secrets: {
    set(key: string, value: string): Promise<void>;   // write-only
    isSet(key: string): Promise<boolean>;
    delete(key: string): Promise<void>;
  };

  // === Плагин-metadata ===
  plugin: {
    name: string;
    version: string;
    dataPath: string;    // где плагин может держать свои файлы
  };

  // === Design system awareness ===
  theme: {
    current(): 'light' | 'dark';
    onChange(cb: (t: 'light' | 'dark') => void): Unsubscribe;
    tokens: DesignTokens;
  };

  // === Navigation ===
  navigate(path: string): void;
  showModal(component: string, props?: object): Promise<any>;

  // === Feedback ===
  toast(message: string, kind?: 'info' | 'success' | 'error'): void;

  // === Rooms / Rules (read-only для UI) ===
  rooms: {
    list(): Promise<Room[]>;
    get(id: string): Promise<Room>;
  };
  rules: {
    list(): Promise<Rule[]>;
    get(id: string): Promise<Rule>;
  };
}
```

TypeScript-декларации публикуются вместе с design system: `npm i -D @keystone/types`.

## 8. Design System — детали

Живёт как yarn workspace `packages/design/` в monorepo keystone — **не отдельная репа**, **не inline в apps/web**. Публикуется как npm package `@keystone/design` в общий scope `@keystone/*` (см. `plugin-store-architecture.md` §14 «npm scope и cross-repo publishing»).

```
keystone/                          (monorepo, yarn workspaces)
├── package.json                    { "workspaces": ["apps/*", "packages/*"] }
├── apps/
│   └── web/                        deps: { "@keystone/design": "workspace:*" }
└── packages/
    └── design/
        ├── package.json            name: "@keystone/design"
        ├── tokens/
        │   ├── tokens.css          CSS custom properties (единственный источник tokens)
        │   ├── tokens.js           JS export для программной сборки
        │   ├── tokens.figma.json   Figma tokens plugin (v0.2)
        │   └── themes/
        │       ├── dark.css        v0.1: базовая тема
        │       └── light.css       v0.2 follow-up
        ├── components/             Web Components на lit (см. scope ниже)
        ├── utilities.css           .ks-label и другие CSS utility classes
        ├── deps/lit.js             vendored для CDN
        ├── storybook/              интерактивная документация
        ├── index.js                entry point
        └── README.md
```

### 8.1 Scope v0.1

Восемь Web Components + одна CSS utility class:

- **Web Components:** `<ks-button>`, `<ks-toggle>`, `<ks-slider>`, `<ks-input>`, `<ks-chip>`, `<ks-icon>`, `<ks-section-header>`, `<ks-spinner>`
- **Utility class:** `.ks-label` в `utilities.css` (11px + letter-spacing + uppercase + muted color — 4 CSS декларации не оправдывают shadow DOM)
- **Формы:** `<ks-input>`, `<ks-toggle>`, `<ks-slider>` — form-associated через ElementInternals с самого начала (`static formAssociated = true`, `setFormValue`, `setValidity`). Retrofit позже = breaking change для существующих потребителей.
- **Темы:** dark-only. `[data-theme="light"]` overrides — v0.2 follow-up.
- **Типография:** 4 канонических размера из `ui-design-brief.md` §6. При миграции все font-size в apps/web (сейчас 14 разных значений) мапятся к ближайшему из 4.

### 8.2 Обязательные consumer-migrations в apps/web

Настоящий validator — не Storybook в изоляции, а живой `apps/web`. Пять consumer-мест, миграция которых входит в DoD v0.1:

- **Toggle в DeviceTile** — сохранить zustand-selectors + optimistic-pending logic intact
- **Slider в DeviceTile** — то же
- **Chip в FoundDeviceItem**
- **SectionHeader в DeviceDetailScreen + DeviceSettingsSection**
- **Button + Input в AddDeviceScreen** (setup-code flow)

Остальные использования — только token-migration через `--ks-*`, без принудительной замены React-компонент.

### 8.3 v0.2+ backlog

Не входят в v0.1, добавляются волной когда появятся конкретные плагин-потребители:

- `<ks-card>` — после того как понятна факторизация DeviceTile/RoomTile
- `<ks-select>`, `<ks-modal>`, `<ks-toast>`, `<ks-tabs>`, `<ks-list>`, `<ks-badge>`, `<ks-tooltip>`
- Domain-specific: `<ks-color-picker>`, `<ks-temperature-picker>`, `<ks-time-picker>`, `<ks-schedule-grid>`, `<ks-device-icon>`, `<ks-room-picker>`, `<ks-feature-control>`
- Light theme (`[data-theme="light"]` overrides)
- Figma tokens sync (`tokens.figma.json`)

### 8.4 Дистрибуция

- **Публикация:** `yarn publish` из `packages/design/` при tag'е → npm package `@keystone/design`. CI-задача (post-Phase G).
- **CDN:** `cdn.keystone.io/design/vX/` — mirror npm, для плагинов которые не билдят.
- **SemVer:** major-версии несовместимы, minor+patch — backward-compatible. Deprecation window 2 major (см. §15).
- **Storybook:** deploy на `design.keystone.io` (GitHub Pages из `packages/design/storybook/`).

## 9. Testing UI из плагина

### 9.1 `keystone plugin ui preview` — dev loop

```
$ cd my-plugin
$ keystone plugin ui preview
Starting keystone dev instance with mock data...
Loading plugin UI from ./ui/
Preview available at http://localhost:7778
Watching for file changes...
```

Автор пишет код, сохраняет — hot-reload в браузере. Mock-данные позволяют тестировать разные state'ы без реального устройства.

### 9.2 Screenshot tests

В шаблоне plugin — Playwright-конфиг из коробки:

```js
// tests/ui.test.js
test('thermostat detail renders at 22°', async ({ page }) => {
  await page.goto('/plugins/my-plugin/preview/thermostat?state=%7B%22target_temp%22%3A22%7D');
  await expect(page).toHaveScreenshot('thermostat-22.png');
});
```

Ловят регрессии стиля.

### 9.3 A11y-checklist

`@keystone/design` компоненты gothrough ARIA, keyboard support, focus management автоматически. Плагин-автор thinks about basic content, а не о низкоуровневой accessibility. CLI при `plugin test` запускает axe-core на скринах — обязательный check для `verified` tier.

## 10. Безопасность

### 10.1 CSP на плагин-UI

По умолчанию:
```
default-src 'self';
script-src 'self';
style-src 'self' 'unsafe-inline';
img-src 'self' data:;
connect-src 'self';
```

Плагин может расширить через manifest `permissions:`, ядро на установке показывает пользователю: "плагин запрашивает доступ к api.vendor.com". Пользователь подтверждает или отклоняет.

### 10.2 Изоляция

- **Layer 3 Web Components** — Shadow DOM для CSS-изоляции. JS плагина live'ит в main frame, имеет доступ к `window.keystone` (и только к нему через типизированный API).
- **Layer 4 iframes** — sandbox attribute (`sandbox="allow-scripts allow-same-origin"`), postMessage-only communication, CSP-restricted.
- **Никаких `eval()`, `Function()`** от плагина через API. Ядро не даёт runtime code execution.

### 10.3 Permission model

Каждое действие через `window.keystone` проверяется по permissions в манифесте:

- `ui.devices.read` — чтение списка устройств
- `ui.devices.control` — invoke/setState
- `ui.subscribe` — real-time events
- `ui.rules.read` — read правила
- `ui.rooms.read` — read комнаты
- `ui.navigate.other-plugins` — navigate к URL другого плагина
- `ui.external-*` — внешние ресурсы

## 11. i18n

Плагин может поставлять свои переводы:

```yaml
ui:
  i18n:
    default: en
    messages:
      en: ui/i18n/en.json
      ru: ui/i18n/ru.json
      de: ui/i18n/de.json
```

`window.keystone.t('key')` в Layer 3 компонентах, `keystone.t` в iframe-bridge. Ядро форвардит текущую locale пользователя.

Layer 1 form-labels тоже локализуемы — schema поддерживает `titleI18n: { en: "...", ru: "..." }` или ссылка на i18n-ключ.

## 12. Тёмная/светлая тема

**Тема — контракт ядра, а не плагина.** Ядро принуждает плагин использовать выбранную пользователем тему. У плагина нет способа её переопределить.

Как это обеспечивается:

- Ядро сетит CSS custom properties (`--ks-color-*`) на `:root` в соответствии с выбранной темой. Плагин **обязан** использовать эти токены для цветов, а не хардкод.
- Хардкоженные цвета в плагиновском CSS будут выглядеть неправильно в противоположной теме — CI (`plugin test`) запускает lint-check на CSS, флажит любой hex/rgb-литерал в свойствах color/background/border как warning в experimental, как block в verified/core tier.
- `window.keystone.theme.current()` возвращает текущую тему (для программной логики: показать разную иконку и т.п.).
- `window.keystone.theme.onChange(cb)` — реактивно, для случаев когда компонент рисует Canvas/SVG динамически.
- **Нет** метода `setTheme` или подобного — плагин не может форсить тему в своём scope. Если попробует переопределить `--ks-*` custom properties внутри своего Shadow DOM — CSS lint при `plugin publish` это заблокирует.

Дизайн-система (`@keystone/design`) поставляет обе темы out-of-box. Плагин использует токены — темы работают автоматически.

## 13. Порядок реализации

- **Phase G — Design System v0.1** (~2 недели, параллельно с Phase D-E plugin-store)
  - Design tokens (CSS + JS)
  - 10-15 базовых Web Components
  - Storybook, npm publish, CDN endpoint

- **Phase H — UI runtime + Layer 1** (~1 неделя)
  - Auto-form renderer из JSON Schema
  - `ui.config.schema` парсер в manifest
  - Первое использование в живом плагине

- **Phase I — Layer 2 (Config Flow)** (~1 неделя)
  - RPC-контракт `configFlow`
  - Wizard-компонент в keystone-UI
  - 8 step-типов (`form`, `oauth`, `qr-scan`, `progress`, `confirm`, `pick-device`, `manual-action`, `error`)
  - Refactor Matter commissioning в этот же механизм

- **Phase J — Layer 3 (Web Components)** (~2 недели)
  - Loader кастомных элементов
  - `window.keystone` runtime API
  - `deviceDetail` / `deviceCard` / `dashboardWidget` matchers
  - Import maps, shared deps
  - Reference plugin: extended device detail для одного из существующих (Matter thermostat, когда появится)

- **Phase K — Layer 4 (Iframes)** (~3 дня)
  - Iframe-router `/plugins/{name}/{page}`
  - postMessage-мост
  - `@keystone/iframe-runtime` библиотека
  - CSP/permission enforcement

**Итого: ~4 недели** до полного UI-integration стека.

## 14. Метрики успеха

- **Time to first custom UI**: плагин от 0 до running Layer 3 device detail — < 1 час.
- **Consistency check**: сторонний рецензент не отличает Layer 3 плагина от core-UI на скриншотах.
- **Bundle overhead**: shared design system ~50KB gzipped, доп. плагин ~10-30KB.
- **A11y compliance**: 100% `<ks-*>` компонент проходят axe-core.

## 15. Компонент-версии (решено)

**Правило: не ломаем уже закреплённый API.** Только additive changes в minor. Breaking изменения — только через deprecation cycle:

1. В версии `N` объявляется устаревание: компонент/prop/атрибут помечается `@deprecated`, в console-log warning при использовании, в CI plugin-test — warning.
2. Deprecation window — **2 мажорных релиза keystone-core** (примерно 6-12 месяцев календарно). Устаревшая версия работает всё это время без изменений в поведении.
3. Через 2 мажора — удаление. Плагины, не мигрировавшие, помечаются в registry как "requires design-system vX" и требуют update от автора.

Технически:
- `@keystone/design` держит last-major endpoint (`cdn.keystone.io/design/v1/`) параллельно с новым (`v2/`).
- Плагины версионируют требуемый major в манифесте: `ui.designSystem: "^1.0.0"`.
- Ядро загружает нужную версию по manifest hint'у. Разные плагины могут работать с разными major-версиями design system параллельно.

Это обязательство core team — надёжность API дороже эволюционной скорости.

## 16. Оставшиеся открытые вопросы

- **Complex form controls в Layer 1.** JSON Schema сам по себе не выразит "picker цветовой температуры лампы". Расширяем `ui:widget` дополнительными типами (`ui:widget: "temperature-kelvin"` → рендер `<ks-temperature-picker>`) или мигрируем такое в Layer 2/3.

- **Plugin-provided icons.** Плагин Yandex Alice может захотеть свою иконку для устройств. Стратегия: `icon: plugin://my-plugin/icons/foo.svg` в discovered devices, keystone-UI загружает через plugin static server.

- **Layer 3 crash isolation.** Если Web Component плагина зависает в `render()` — Web Component API не даёт полностью изолировать. Решения: watchdog (timeout на render), скрытие компонента с fallback на default-render. Пока — YAGNI, но проектировать API так, чтобы crash не ронял всю страницу.
