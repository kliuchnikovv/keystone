# Keystone Web MVP — бриф для дизайнера и фронтендера

**Статус:** v0.1, entry point
**Аудитория:** дизайнер (UI финальный) + фронтендер (React), + продакт как заказчик.
**Родительские документы:**
- [`smart-home-engine-brief.md`](../../../claude/smart-home-engine-brief.md) — продукт
- [`ui-design-brief.md`](./ui-design-brief.md) — общий дизайн-язык, принципы, палитра
- [`ui-mockups.html`](./ui-mockups.html) — черновые wireframes
- [`ux-automations-ai-first.md`](../../../claude/ux-automations-ai-first.md) — экран автоматизаций (позже)
- [`go-core-spec.md`](../../../claude/go-core-spec.md) — бэкенд-контракт

## 1. Цель этого MVP

Сделать веб-приложение, через которое можно:

1. Увидеть все подключённые к Keystone устройства с их живым состоянием.
2. Добавить новое Matter-устройство (тестируем на **IKEA WARMBLIXT** — Matter over Thread, tunable white).
3. Управлять базовыми параметрами (on/off, яркость, цветовая температура).

Больше в MVP не идёт. Automations, Energy, Rooms, Family — следующие итерации.

**Зачем именно веб, а не сразу нативное приложение.** Мы хотим быстро проверить UX и получить работающую demo для показов. Стек выбран так, чтобы 70% кода переехало в React Native без переписывания — экраны, компоненты, стейт, API-слой. Заменять придётся только рендер (`<div>` → `<View>`) и стили (CSS → StyleSheet).

## 2. Что должно быть готово на бэке до старта фронта

Блокеры:

- **Matter adapter** в keystone. Сейчас его нет — есть только virtual adapter и планируется DIRIGERA. Без него WARMBLIXT не подключится физически.
- **HTTP API** — уже готово (`/devices`, `/devices/{id}/actions`, `/devices/{id}/state`, `/stream`).
- **Commissioning endpoint** — нужен `POST /devices/commission` с параметрами Matter setup payload. Сейчас есть в service-слое, надо только HTTP-обёртка.

Не-блокеры (можно параллелить):

- Дизайнер работает по mockups и brief, не ждёт бэк.
- Фронтендер начинает с виртуальных устройств (существующий demo-scene) и переключается на Warmblixt как только Matter adapter готов.

## 3. Целевое устройство: IKEA WARMBLIXT

- Форм-фактор: настольная декоративная LED-лампа.
- Протокол: **Matter over Thread**.
- Требования commissioning: QR-код на коробке + Thread Border Router в сети (у большинства пользователей уже есть через Apple TV 4K / HomePod / Google Nest Hub).
- Фичи, которые показываем в UI:
  - **on/off** — базовый toggle.
  - **brightness** — 0–100%.
  - **color_temp** — от warm 2200K до cool 4000K (tunable white, не RGB).
- Что читаем в state:
  - `onoff.value` (bool)
  - `brightness.level` (0–100)
  - `color_temp.kelvin` (int)

Feature-set в keystone для WARMBLIXT (для маппинга Matter → наша model):

```json
{
  "Type": "light",
  "Features": [
    {"Key": "onoff",       "States": ["value"],  "Actions": ["turn_on","turn_off","toggle"]},
    {"Key": "brightness",  "States": ["level"],  "Actions": ["set"]},
    {"Key": "color_temp",  "States": ["kelvin"], "Actions": ["set"]}
  ]
}
```

**Fallback для тестов пока Matter-адаптер не готов:** дизайнер и фронтендер могут смоделировать WARMBLIXT через virtual adapter — просто добавить в demo-scene ещё один виртуальный девайс с этим feature-set. UI будет работать 1-в-1.

## 4. Технический стек — обоснование

Каждый выбор ниже — с прицелом на React Native.

| Решение | Выбор | Почему |
|---|---|---|
| Bundler | **Vite** | Быстрый dev-server, TS из коробки, минимум магии. Не Next.js — SSR не нужен, а RN Next.js не полезет. |
| Язык | **TypeScript** (strict) | Типы на API + типизация экранов + сразу типы можно шарить с RN. |
| UI-фреймворк | **React 18** | Единая ментальная модель с RN. |
| Routing | **react-router v6** | RN-совместимый (react-router-native). Или переехать на expo-router в RN — легко. |
| State (глобальный) | **Zustand** | Работает 1-в-1 в RN, без context-ада. |
| Async / API state | **@tanstack/react-query** | Официальная поддержка RN. Auto-refetch, кэши, оптимизм. |
| Styling | **CSS Modules + design tokens в JSON** | Токены (`tokens.ts`) шарятся с RN. CSS Modules — только для web-рендера, в RN переписываются на `StyleSheet`. Не Tailwind (в RN не работает). Не styled-components (можно, но CSS Modules проще). |
| Icons | **lucide-react** | Есть RN-версия (`lucide-react-native`). |
| QR-scanner | **@zxing/browser** для web | В RN — `expo-camera` + `expo-barcode-scanner`. Обёртка `<QRScanner>` с одинаковым API. |
| API-транспорт | **fetch + own client** | Работает в обоих. Готов к замене на Connect (proto-over-HTTP) когда захотим типизацию. |
| Live-stream | **fetch + ReadableStream** | Работает в web. В RN — `react-native-fetch-api` или полифилл. |
| Testing | **Vitest + React Testing Library** | Vitest — быстро; RTL шарится с RN. |
| Форматирование | **Prettier + ESLint** | Стандарт. |

## 5. Структура проекта

```
apps/web/                           # монорепо-ready структура
├── src/
│   ├── main.tsx                    # entry
│   ├── App.tsx                     # router
│   ├── screens/                    # экраны — переносятся в RN как есть (кроме рендера)
│   │   ├── HomeScreen.tsx
│   │   ├── AddDeviceScreen.tsx
│   │   └── AddDeviceSuccessScreen.tsx
│   ├── components/                 # atomic components
│   │   ├── DeviceTile/
│   │   │   ├── DeviceTile.tsx
│   │   │   ├── DeviceTile.module.css
│   │   │   └── DeviceTile.test.tsx
│   │   ├── Toggle/
│   │   ├── Slider/
│   │   ├── PowerMeter/
│   │   ├── IntentCard/
│   │   ├── QRScanner/              # обёртка над @zxing/browser
│   │   ├── FoundDeviceItem/
│   │   ├── ColorTempSlider/
│   │   └── NavBar/
│   ├── api/
│   │   ├── client.ts               # fetch wrapper
│   │   ├── devices.ts              # list/get/invoke/write/commission
│   │   ├── stream.ts               # /stream subscription
│   │   ├── rules.ts                # rules CRUD (для v1.1)
│   │   └── types.ts                # TS-типы (можно сгенерить из proto через ts-proto)
│   ├── state/
│   │   ├── devicesStore.ts         # Zustand: devices + liveState
│   │   ├── connectionStore.ts      # host, статус
│   │   └── commissioningStore.ts   # текущая сессия pairing
│   ├── hooks/
│   │   ├── useDevices.ts           # react-query wrapper
│   │   ├── useLiveStream.ts        # subscribes to /stream
│   │   └── useCommission.ts
│   ├── theme/
│   │   ├── tokens.ts               # colors, spacing, radii, motion — SHARED WITH RN
│   │   └── theme.module.css        # CSS-vars из tokens.ts
│   └── lib/
│       └── format.ts               # форматтеры (Kelvin → «тёплый»/«холодный»)
├── public/
├── index.html
├── package.json
├── tsconfig.json
└── vite.config.ts
```

Название директории `apps/web/` подразумевает, что позже мы добавим `apps/mobile/` (Expo) в тот же репозиторий, и оба будут делить `packages/shared/` (theme, types, api client).

## 6. Design system — что уже определено

Все токены — в `ui-design-brief.md §5–6`. Кратко:

```typescript
// src/theme/tokens.ts
export const colors = {
  bg:       '#141210',
  surface:  '#1E1B18',
  surface2: '#262220',
  line:     '#322D28',
  text:     '#F4EFE7',
  text2:    '#A69F94',
  text3:    '#6D655C',
  accent:   '#F4A860',   // amber, для active state
  accent2:  '#D96B4A',   // terracotta
  success:  '#7FB069',
  warn:     '#E4A649',
  danger:   '#D45D5D',
};

export const radii   = { sm: 12, md: 16, lg: 22, xl: 32 };
export const spacing = { xs: 4, sm: 8, md: 16, lg: 24, xl: 32, xxl: 48 };
export const type    = {
  display: 'Fraunces, ui-serif, serif',
  sans:    'Inter, system-ui, sans-serif',
  mono:    'ui-monospace, SF Mono, monospace',
};
export const motion  = {
  fast:   '160ms',
  base:   '220ms',
  slow:   '320ms',
  spring: 'cubic-bezier(0.34, 1.56, 0.64, 1)',
};
```

CSS-модули читают эти токены через CSS-переменные, установленные в `theme.module.css :root`. В React Native — читаются напрямую в `StyleSheet.create({...})`.

## 7. Экран 1 — Home

**Задача:** показать все устройства пользователя с живым состоянием, дать быстрый доступ к управлению.

### Информационная архитектура

```
┌─────────────────────────────────────┐
│  Header                              │
│    Логотип + приветствие + время     │
│    Кнопка «+ Добавить» справа        │
├─────────────────────────────────────┤
│  Intent-card (optional, v1.1)        │
├─────────────────────────────────────┤
│  Section: Свет  (2 устройства)       │
│    [Device tile]  [Device tile]      │
│                                       │
│  Section: Розетки (2 устройства)     │
│    [Device tile]  [Device tile]      │
│                                       │
│  Section: Датчики (пусто пока)       │
│                                       │
├─────────────────────────────────────┤
│  Nav bar (fixed bottom, 4 tabs)      │
│    Дом · Энергия · Правила · Я       │
└─────────────────────────────────────┘
```

### Device tile — центральный компонент

Один тайл на одно устройство. Реагирует на live-стейт (accent-свечение при active).

**Обязательное содержимое:**
- Иконка типа (💡, 🔌, 📡...) — TL corner.
- Название устройства.
- Комната (мелким текстом под названием). Если комнаты нет — «Без комнаты».
- Основной control по типу:
  - **light** → toggle справа сверху + slider яркости (компактный) внизу.
  - **plug** → большой toggle + текущее потребление (`340 W`) если есть power_meter.
  - **sensor** → просто показатель, крупно (23°C, 42%).
- Дополнительная строка: color temperature (для WARMBLIXT — «2700K тёплый»).

**Состояния:**
- Idle / active (свет вкл) / offline (серый, полупрозрачный) / pending (после тапа, пока стрим не подтвердил) / error (тонкая коралловая обводка).

**Взаимодействия:**
- Тап на toggle → изменение сразу (optimistic), реальный стейт приходит из `/stream` в течение 100–300ms.
- Тап на body тайла (не на control) → навигация в Device Detail (v1.1, пока nop).
- Long-press → быстрое меню сцен (v1.1).

### Empty state

Первый запуск, ни одного устройства:

```
        🕸️  (иконка большим размером)
   
   Пока ни одного устройства.
   
   Добавь первое — начни с лампы,
   розетки или датчика движения.
   
   [ + Добавить устройство ]   ← primary CTA
```

## 8. Экран 2 — Add Device

**Задача:** превратить обычно-сложный процесс Matter-commissioning'а в один естественный поток.

Это ключевая точка UX-дифференциации от HA (там процесс — 12 шагов и YAML). У нас — **один экран с камерой + live-список найденных устройств**.

### Flow: 3 экрана, из которых главный — второй

```
[1] Intro  →  [2] Scan  →  [3] Success
```

### 8.1 Intro (короткий, можно пропускать через настройки)

Показывается только при первом Add Device — потом сразу в 8.2.

- Иллюстрация (дизайнер решает какая).
- Заголовок: «Найдём твоё устройство.»
- Подзаголовок: «Наведи камеру на QR-код или коробку. Мы также сканируем сеть.»
- Кнопка «Начать» + чекбокс «Больше не показывать это».

### 8.2 Scan (главный экран)

**Верх экрана (~40% высоты):**
- **Camera preview** — live-view с задней (для мобилы) / фронтальной (для web-версии) камеры.
- Поверх — рамка «прицела» для QR-кода, стилизованная угловыми уголками accent-цветом.
- Если QR распознан → мгновенная обратная связь: рамка мигает зелёным, вибрация (в RN).

**Низ экрана (~60%):**
- Заголовок: «Найдено рядом · 3»
- Live-список устройств, которые keystone сам обнаружил через discovery. Каждый элемент:
  - Иконка типа.
  - Имя (модель, если известна: «WARMBLIXT»).
  - Транспорт (Matter, Zigbee, DIRIGERA…) — маленьким pill'ом.
  - Правее — кнопка «Добавить» (primary) или иконка «уже добавлено» (галка, серым).

**Мелкое, но важное:**
- Newly-appeared устройства подсвечиваются accent-точкой слева на 3 секунды. У пользователя ощущение «оно нашлось только что».
- Если ничего не найдено за 15 секунд — карточка «Не нашли? Введи setup code вручную» с полем ввода.

### 8.3 Success

После тапа «Добавить»:

- Прогресс commissioning (несколько шагов от keystone: discovering → pairing → verifying → done). Стриммится через WebSocket или long-poll от `POST /devices/commission`.
- Каждый шаг с иконкой и коротким текстом.
- В конце — экран «Готово!» с превью тайла того устройства и вопросом «В какую комнату?» (dropdown комнат + возможность создать новую).
- Кнопка «Проверить» — включает WARMBLIXT на секунду для подтверждения физического соединения.

## 9. API-контракт для фронта

Всё уже есть в keystone, никаких изменений на бэке не требуется, кроме одного:

### Что использовать сейчас

| Endpoint | Метод | Что делает |
|---|---|---|
| `/devices` | GET | Список устройств |
| `/devices/{id}` | GET | Одно устройство |
| `/devices/{id}/actions` | POST | Вызов action (`turn_on`, `turn_off`, `set`, `toggle`) |
| `/devices/{id}/state` | POST | Прямая запись state |
| `/stream` | GET (NDJSON) | Live-стрим state changes и events |
| `/rules` | GET/POST | v1.1 |
| `/rules/runs` | GET | v1.1 |
| `/health` | GET | ping |

### Что нужно добавить бэку

**`POST /devices/commission`** — сейчас только через gRPC `DeviceService.Commission`. Нужен HTTP-эквивалент со streaming-ответом (SSE или NDJSON):

```
POST /devices/commission
Body: { transport, payload, name, type, wifi_ssid?, wifi_credential? }
Response (stream):
  {"stage":"discovering","message":"..."}
  {"stage":"pairing","message":"..."}
  {"stage":"done","device":{...}}
```

**`GET /discover`** — новый endpoint для live-списка найденных устройств:
```
GET /discover?transport=matter
Response (stream):
  {"ref":"...","name":"WARMBLIXT","type":"light","transport":"matter"}
```

Оба — работа бэкенда одновременно с фронтом.

### Форматы данных

`Device` от `/devices` (текущий формат из json-encoder Go):

```json
{
  "ID": "f1dc0f97-b5c56f6becafe088",
  "Type": "light",
  "Name": "WARMBLIXT",
  "Manufacturer": "IKEA",
  "Model": "TABLE",
  "Room": "",
  "Transport": "matter",
  "TransportRef": "matter-node-42",
  "Features": [
    {"Key":"onoff","States":["value"],"Actions":["turn_on","turn_off","toggle"]},
    {"Key":"brightness","States":["level"],"Actions":["set"]},
    {"Key":"color_temp","States":["kelvin"],"Actions":["set"]}
  ],
  "CreatedAt": "2026-08-11T17:26:29Z"
}
```

⚠️ Обрати внимание: у Go-энкодера дефолтно **CapitalCase** для полей (`ID`, `Name`). Это фиксируется добавлением `json:"id"` тегов на бэке — задача под фронт-старт. Пока — работаем как есть.

`Stream message` от `/stream` (NDJSON, по строке на событие):
```json
{"type":"state","payload":{"DeviceID":"...","Feature":"onoff","Key":"value","Value":true,"UpdatedAt":"...","Origin":"device_report"}}
{"type":"event","payload":{"DeviceID":"...","Feature":"motion","Name":"motion_detected","Data":{},"At":"..."}}
```

## 10. Что делает дизайнер (список deliverables)

Приоритет 1 (для MVP старт фронта):

1. **Home screen** — dark + light, состояния: с устройствами, empty, offline.
2. **DeviceTile** — все 4 варианта (light on/off, plug on/off, sensor idle, offline).
3. **Add Device Scan** — с камерой + список найденного, 3 варианта: пусто / 3 найдено / после QR.
4. **Add Device Success** — процесс commissioning + финал с выбором комнаты.
5. **NavBar** — 4 таба + активное состояние.
6. **Compact component library** в Figma (Toggle, Slider, IntentCard, Chip, Button primary/ghost).

Приоритет 2 (для v1.1):

7. Intent-cards на Home.
8. Room detail (тап на тайл).
9. Empty/error/loading skeleton'ы.
10. Пиктограммы устройств — набор (12–15 штук). Пока — используем эмодзи как placeholder.

Формат: Figma-файл со auto-layout компонентами. Design tokens → в файле `tokens.ts` (дизайнер отдаёт как JSON, фронтендер копирует).

## 11. Что делает фронтендер (список deliverables)

Приоритет 1:

1. Setup Vite + TS + линтеры + prettier + testing.
2. Roter c двумя маршрутами (`/` и `/add-device`).
3. Design tokens в `theme/tokens.ts` + CSS-vars в `theme.module.css`.
4. API-client (`api/client.ts`) + specific-модули (`devices.ts`, `stream.ts`).
5. Zustand store для devices + liveState.
6. Хук `useLiveStream()` — подписка на `/stream`, обновление store, авто-reconnect.
7. `HomeScreen` — список устройств, группировка по типу, empty state, header.
8. `DeviceTile` — все состояния, оптимистичные updates.
9. `AddDeviceScreen` — камера через `@zxing/browser`, live-список из `/discover`, тап → commission.
10. `AddDeviceSuccess` — прогресс commissioning из streaming-endpoint.
11. Responsive: мобильная ширина работает нормально (это ведь скоро станет RN).

Приоритет 2:

12. Storybook (для дизайнера и себя) — все компоненты в изоляции.
13. Начальный тест-набор в Vitest (базовые проверки store, api).
14. `NavBar` (пока без работающих табов кроме Home и Add Device).

## 12. Дальнейший переход в React Native

Что уже сейчас делаем правильно для будущего RN-порта:

- **Логика в hooks/state** — на 100% переносится.
- **API-слой** — на 100% (замена `fetch` не нужна, работает и в RN).
- **Design tokens** — на 100% через `tokens.ts`.
- **Экраны** — переименовать теги, оставить структуру. `<div>` → `<View>`, `<button>` → `<Pressable>`, etc.
- **Компоненты** — рендер меняется, props/логика остаются.

Что придётся переписывать:

- **Стили** — CSS Modules → `StyleSheet.create`. Утилита `tokens-to-stylesheet.ts` возможна.
- **Router** — react-router → expo-router (структура файлов немного другая).
- **Camera/QR** — `@zxing/browser` → `expo-camera`. Обёртка `<QRScanner>` минимизирует боль.
- **Stream** — если `ReadableStream` не работает в RN, использовать `react-native-fetch-api`.

**Оценка порта:** 3–5 дней после того, как web MVP полностью работает.

## 13. Оценка сроков

MVP (два экрана + API + стрим + commissioning):

| Этап | Дни | Условия |
|---|---|---|
| Дизайн приоритета 1 | 3–5 | Дизайнер полный день |
| Setup + tokens + API-client + store + хуки | 2 | Фронтендер |
| HomeScreen + DeviceTile + все состояния | 2 | Мокапы готовы к моменту |
| AddDeviceScreen + camera QR + live-discover | 3 | HTTP `/discover` есть на бэке |
| AddDeviceSuccess + commission stream | 1 | HTTP `/commission` есть на бэке |
| Полировка + бэги + первый прогон с реальным Warmblixt | 2 | Matter-adapter в keystone готов |
| **Итого:** | **~10 рабочих дней** | если дизайн и бэк не блокируют |

Параллельно бэкенд должен успеть с (a) `POST /devices/commission` HTTP-обёрткой, (b) `GET /discover` streaming, (c) Matter adapter через matter.js sidecar.

## 14. Открытые вопросы

Прежде чем стартовать код — ответить.

1. **Кто commissioning'ит Warmblixt в первый раз?** Есть три варианта:
   - a) Через keystone напрямую (нужен свой Thread Border Router — есть ли он у нас?). Если нет — покупаем Nanoleaf Essentials starter kit или Apple TV 4K.
   - b) Через iOS Home App, потом multi-admin в keystone. Работает быстрее, но требует настроить matter.js на приём multi-admin invitation.
   - c) Через iPhone Home App как proxy (Apple TV/HomePod уже подключён к нашей сети). Легче всего.
2. **HTTP-стриминг для commissioning'а — SSE или NDJSON?** SSE стандартнее, но NDJSON проще и мы уже его используем в `/stream`.
3. **Auth и multi-user?** В MVP — нет, single user, single hub. Add-user flow — v1.1.
4. **Deployment фронта.** Serve keystone его сам через `/ui/`? Или отдельный статик на GitHub Pages / Cloudflare Pages? Для MVP — через keystone (уже реализовано), проще.
5. **CI/CD.** GitHub Actions с build + test on PR. Не блокер, но настраиваем сразу.
6. **Дизайнерский stack** — Figma? Sketch? Sketch не поддерживает design tokens нормально; предпочитаем Figma.

## 15. Что делать сейчас

Пока читаешь бриф — ставится в бэклог:

- Фронтендер: setup проекта в `apps/web/`, tokens, API-client. Не ждёт дизайна.
- Дизайнер: приоритет 1 в Figma. Первый рабочий проход — 3 дня.
- Продакт (я): договориться с командой keystone-бэка про commission + discover endpoints.

Готовые артефакты для контекста:
- `docs/ui-design-brief.md` — общий дизайн-язык (обязательно к прочтению).
- `docs/ui-mockups.html` — визуальные наметки.
- `site/dashboard.html` — референс, как выглядит текущее HTTP-API-подключение (можно код смотреть — это одностраничный vanilla JS, за час портируется в React).
