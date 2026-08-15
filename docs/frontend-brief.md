# Keystone Web MVP — бриф для фронтендера (v0.2)

**Статус:** v0.2 · replaces v0.1
**Роль:** фронтенд-разработчик (React + TypeScript)
**Параллельная работа:** [`designer-brief.md`](./designer-brief.md), [`matter-adapter-brief.md`](./matter-adapter-brief.md)
**Контекст-документы:**
- [`go-core-spec.md`](../../../claude/go-core-spec.md) — устройство бэкенда.
- [`matter-adapter-brief.md`](./matter-adapter-brief.md) — как работает Matter multi-admin (обязательно к прочтению — определяет UX Add Device).
- [`ui-design-brief.md`](./ui-design-brief.md) — дизайн-принципы и токены.
- `apps/web/` в репо — **уже существующий код**, из которого мы стартуем.

## 0. Что изменилось относительно v0.1

Три больших изменения. Если работал по v0.1 — прочитай эту секцию внимательно.

**1. Возврат к mobile-first.** Существующая реализация ушла в desktop-first 3-column layout (`Shell` + `LeftNav` + `RightRail` + `CommandPalette`). Мы это откатываем в **mobile-first одноколонной композиции** (390pt iOS). NavBar снизу, никаких боковых панелей. Причины:
- MVP → React Native порт. Desktop-only layout в RN не поедет.
- Аудитория mass market — сначала мобильное, потом веб. Даже web-версия должна работать на телефоне.
- Меньше кода — быстрее ship.

**2. Скоуп +1 экран.** Теперь три экрана: Home, Add Device, Device Detail. Device Detail нужен для WARMBLIXT-демо (яркость + цветовая температура — компактно в тайле не влезают).

**3. Add Device — новая модель.** Мы отказались от «keystone-как-primary-commissioner» в MVP. Теперь Matter-устройства попадают в keystone через **multi-admin из любой Matter-экосистемы** (Apple Home / Google Home / SmartThings / Amazon Alexa). Пользователь commissioning'ит устройство в своей экосистеме → генерирует одноразовый setup-code через «Turn on Pairing Mode» → вводит его в keystone. Camera-QR остаётся как placeholder в UI (эстетика), но подключаем `@zxing/browser` только когда появится своя железка (v1.x).

Подробнее — раздел 7.

## 1. Скоуп v0.2

**Три экрана:**

1. **Home** — все устройства пользователя, live-состояние, базовое управление.
2. **Add Device** — multi-ecosystem multi-admin flow.
3. **Device Detail** — детальные controls (яркость с крупным слайдером, цвет, история срабатываний, настройки).

**Целевой тестовый девайс:** IKEA WARMBLIXT (Matter over Thread, tunable white — `onoff` + `brightness` + `color_temp`).

**Не входит в MVP:** Rooms, Automations, Energy, You (профиль/семья), Settings, авторизация, i18n, PWA, real QR scanner, real intent-cards (LLM). Всё это — v1.1+.

## 2. Multi-ecosystem Matter multi-admin — что нужно знать фронту

**Как работает multi-admin.** Matter спецификация позволяет одному устройству находиться в нескольких «фабриках» одновременно. Пользователь commissioning'ит WARMBLIXT в **любой** Matter-экосистеме (Apple/Google/SmartThings/Alexa) — эта экосистема становится первой фабрикой. Потом в UI той же экосистемы включает «Turn on Pairing Mode» — получает одноразовый 11-значный setup code. Этот код передаётся в keystone → keystone commissioning'ится как **вторая фабрика**. Устройство теперь в двух экосистемах, обе могут управлять параллельно.

**Почему это круче чем primary commissioning в MVP:**
- Не нужен собственный Thread Border Router (у пользователя уже есть Apple TV / Google Nest / SmartThings hub).
- Не нужен UI для commissioning-погружения (пользователь пользуется UX своей экосистемы).
- Fail-safe: если keystone упал — родная экосистема продолжает работать.
- Работает с ЛЮБОЙ Matter-экосистемой одинаково.

**Где UI это отражает.** На Add Device экране пользователь сначала выбирает **ecosystem** (Apple Home / Google Home / SmartThings / Alexa / Прочее). Под каждый — своя короткая инструкция (2 скрина с шагами) и одинаковый setup-code input.

**Будущее (v1.x).** Когда у нас появится своя железка с Thread Border Router + primary commissioner, добавим:
- Отдельный exec-раздел «Прямо в Keystone» с camera-QR-scan.
- Discovery-based flow для non-Matter устройств через свою железку.

## 3. Технический стек (напоминание)

Без изменений от v0.1. Всё уже в проекте.

| Слой | Выбор |
|---|---|
| Bundler | Vite |
| Язык | TypeScript strict |
| UI | React 18 |
| Routing | react-router v6 |
| Global state | Zustand |
| Async | @tanstack/react-query |
| Styling | CSS Modules + design tokens в TS |
| Icons | lucide-react |
| Testing | Vitest + React Testing Library |

## 4. Инвентарь текущего кода apps/web/ — что оставляем

Аудит на 2026-08-12. Ниже — что уже написано и наш вердикт по каждому.

### Оставляем без изменений

- `theme/tokens.ts` + `theme.module.css` — палитра, spacing, radii, motion. Ок.
- `api/client.ts` — fetch wrapper, ApiError. Ок.
- `api/devices.ts` — list/get/invoke/write. Ок.
- `api/stream.ts` — NDJSON parser с reconnect. Ок.
- `api/types.ts` — Device, Feature, StateSnapshot types.
- `state/devicesStore.ts` — Zustand, Map + liveState + liveKey. Ок.
- `state/connectionStore.ts` — host + status. Ок.
- `state/eventsStore.ts` — live event feed. Оставляем, используется в Device Detail.
- `hooks/useDevices.ts` — react-query wrapper. Ок.
- `hooks/useLiveStream.ts` — подписка на /stream. Ок.
- `hooks/useMediaQuery.ts` — переиспользуется в motion / prefers-reduced-motion. Оставляем, но `useIsDesktop` больше не вызывается.
- `lib/format.ts`, `lib/colorTemp.ts` — форматтеры. Ок.
- Компоненты: `DeviceTile`, `Toggle`, `BrightnessSlider`, `ColorTempSlider`, `Chip`, `Button`, `NavBar`, `SectionHeader`, `EmptyState`, `LoadingSkeleton`, `FoundDeviceItem`, `CommissioningStep`, `ConnectionBanner` — все ок для MVP.

### Переделываем

- **`screens/HomeScreen.tsx`** — сейчас использует `Shell` + `LeftNav` + `RightRail` + Modal для Add Device. Переписываем в mobile-first одноколонное поведение с NavBar снизу и `+`-кнопкой в header'е. Секции устройств группировкой по типу — оставляем.
- **`screens/AddDeviceScreen.tsx`** — переписываем полностью под multi-ecosystem multi-admin flow. Discovery-часть (`openDiscover`, live-list) остаётся, но переезжает во второй раздел «Найдено рядом» (для будущей своей железки — сейчас endpoint не отвечает, показываем пустоту с пояснением). Primary — ecosystem picker + setup-code input.
- **`state/commissioningStore.ts`** — расширяем: добавляем `ecosystem` field, `setupCode` field, состояния под новый flow.

### Удаляем

- **`screens/DeviceLabScreen.tsx`** — вспомогательный экран, не нужен. Storybook позже, если надо.
- **`components/Shell/`** — 3-column layout, вон.
- **`components/LeftNav/`** — desktop sidebar, вон.
- **`components/RightRail/`** — desktop right panel, вон.
- **`components/CommandPalette/`** — ⌘K palette, вон (можем вернуть в v1.1 как onboarding-easter).
- **`components/IntentCard/`** — заглушка для v1.1 без реального use — держать в репо смысла нет, вернём когда LLM подключим.
- **`components/Modal/`** — использовался для Add Device на desktop. Больше не нужен. Если понадобится позже — вернём отдельно.
- Route `/lab` в `App.tsx`.

### Добавляем

- **`screens/DeviceDetailScreen.tsx`** — новый третий экран.
- **`components/EcosystemPicker/`** — сегментированный контрол выбора экосистемы (Apple Home / Google Home / SmartThings / Alexa / Прочее).
- **`components/SetupCodeField/`** — 11-значный input с auto-format `1234-5678-901`.
- **`components/EcosystemGuide/`** — 2-3 шаговая инструкция под выбранную экосистему (текст + иконки, скриншоты позже от дизайнера).
- **`screens/DeviceDetail.module.css`** и модули для новых компонентов.

## 5. Экран Home — переделка

### Layout mobile-first

```
┌─────────────────────────────────────┐
│  Header                              │
│    Доброе утро, Валерий              │
│    22° · солнечно     [+ ►Add]      │
├─────────────────────────────────────┤
│                                       │
│  Свет (3)                            │
│    ┌─── Tile ───┐ ┌─── Tile ───┐    │
│    │            │ │            │    │
│    └────────────┘ └────────────┘    │
│    ┌─── Tile ───┐                    │
│    └────────────┘                    │
│                                       │
│  Розетки (2)                         │
│    ...                               │
│                                       │
│  Датчики (—)                         │
│    Empty hint                        │
│                                       │
├─────────────────────────────────────┤
│  NavBar fixed bottom                  │
│  🏠 ⚡ ✨ 👤                            │
└─────────────────────────────────────┘
```

- **Header** — non-sticky, но `+`-кнопка **всегда доступна** через floating action button снизу справа (перекрывает NavBar с offset). Тап → навигация `/add-device`.
- **Секции** — группировка по типу. Порядок: light → plug → sensor → motion → contact → other.
- **NavBar** — fixed внизу, 4 таба (только «Дом» активен, остальные disabled).
- **Ambient-подсветка активных тайлов** — тайл включённого света получает тёплый градиент внутри.
- **Adaptive grid** — при ширине ≥720pt переходим в grid-2 внутри секции, всё остальное — grid-1.

Убираем: `Shell`, `LeftNav`, `RightRail`, `CommandPalette`, Modal-based AddDevice.

### Refactor plan

1. Убрать `import Shell / LeftNav / RightRail / CommandPalette / Modal` из `HomeScreen.tsx`.
2. Собственная структура: `<header>` + `<main>` + `<NavBar>` + `<FloatingActionButton>`.
3. Sections оставляем как есть (`groupDevices`, `SECTION_META` — рабочая логика).
4. `<AddDeviceScreen>` больше не embedded — только через route.

## 6. Add Device — детальный редизайн

Экран из четырёх стадий: `pick-ecosystem` → `enter-code` → `commissioning` → `success/error`. Все через один Zustand-стор.

### 6.1 Экран pick-ecosystem

Первый экран Add Device. Пользователь выбирает откуда пришло устройство.

```
┌─────────────────────────────────────┐
│  ‹ Отмена                            │
│                                       │
│  Добавить устройство                 │
│  Где оно уже настроено?              │
│                                       │
│  ┌───────────────────────────────┐  │
│  │   Apple Home                  │  │
│  │   ⌂                           │  │
│  └───────────────────────────────┘  │
│  ┌───────────────────────────────┐  │
│  │   Google Home                 │  │
│  │   G                           │  │
│  └───────────────────────────────┘  │
│  ┌───────────────────────────────┐  │
│  │   SmartThings                 │  │
│  │   ⚫                           │  │
│  └───────────────────────────────┘  │
│  ┌───────────────────────────────┐  │
│  │   Amazon Alexa                │  │
│  │   ▷                           │  │
│  └───────────────────────────────┘  │
│                                       │
│  ─── или ───                          │
│                                       │
│  Устройство ещё не настроено?         │
│  [ Другой способ ]                    │
│                                       │
└─────────────────────────────────────┘
```

Тап на карточку → переход в `enter-code` с сохранением `ecosystem` в store.

Кнопка «Другой способ» ведёт на fallback-экран с camera-placeholder'ом и текстом: «Скоро мы сможем commissioning'ить устройства напрямую. Пока — используй один из указанных выше домов, чтобы добавить Matter-устройство в Keystone.»

### 6.2 Экран enter-code

Верх — короткая инструкция под выбранную экосистему.

**Apple Home:**
1. Открой приложение **Дом** на iPhone/iPad.
2. Долгий тап на нужном устройстве → «Настройки».
3. Прокрути вниз → **«Turn on Pairing Mode»**.
4. Появится 11-значный код — введи его ниже.

**Google Home:**
1. Открой **Google Home** приложение.
2. Тап на устройство → значок настроек.
3. **«Linked services»** → **«Add another Matter-enabled app»**.
4. Скопируй 11-значный код.

**SmartThings:**
1. **SmartThings** → устройство → **шестерёнка**.
2. **«Share with Matter»** → **«Generate Code»**.
3. Скопируй 11-значный код.

**Amazon Alexa:**
1. **Alexa** → устройство → **Settings**.
2. **«Other Assistants and Apps»** → **«Add another Matter service»**.
3. Скопируй 11-значный код.

Точные тексты — согласовать с дизайнером и обновить, когда получим финальные скрины. **Инструкции хранятся в `lib/ecosystem-guides.ts`** — массив шагов + слот для скриншота (пока — placeholder). Тексты локализуемые.

Под инструкцией — большой input поле:

```
┌───────────────────────────────────┐
│  1234 — 5678 — 901                │
└───────────────────────────────────┘
   
   Auto-format при вводе.
   При заполнении всех 11 цифр — кнопка «Подключить» становится активной.
   
   [ ‹ Назад ]     [ Подключить → ]
```

**SetupCodeField behavior:**
- Принимает только цифры.
- После 4 и 8 цифр — auto-insert dash.
- Ctrl+V / paste-anywhere — цифры извлекаются, dash'и игнорируются.
- Validation: ровно 11 цифр → primary CTA активна.
- Focus-glow при вводе (accent-цвет).

**Кнопка «Подключить»** → dispatch `startCommission(setupCode, ecosystem)` в store → переход в `commissioning`.

### 6.3 Экран commissioning

Progress этапов с backend-стриминга (`POST /devices/commission`):

- discovering (нашли устройство по коду)
- pairing (обмен ключами Matter)
- verifying (первая команда — sanity check)
- done

Каждый этап — компонент `<CommissioningStep>` (уже есть, оставляем). Иконка + label + status (pending/active/done/failed).

### 6.4 Success screen

- Большой чек-марка.
- Заголовок: **«Готово. WARMBLIXT в твоём Keystone.»**
- Превью `<DeviceTile>` того устройства.
- Field/dropdown: «В какую комнату?» (пока — заглушка с 3–4 preset'ами: «Гостиная», «Кухня», «Спальня». Rooms API — v1.1).
- Кнопка **«Проверить»** — включает WARMBLIXT на 1 сек через `POST /devices/{id}/actions turn_on` + `turn_off`.
- Кнопка **«Готово»** — навигация обратно на Home + invalidate devices query.
- Кнопка **«Добавить ещё»** — reset store, переход обратно на pick-ecosystem.

### 6.5 Failure screen

- Тёплый тон, не красный алерт.
- Заголовок: **«Не получилось. Попробуем ещё раз?»**
- Body: конкретный error message от бэка (например «Устройство не отвечает — проверь, что оно на связи, и что Turn on Pairing Mode ещё активен» — окно ~60 сек).
- Кнопки: **«Попробовать снова»** (retry с тем же кодом), **«Другой код»** (обратно на enter-code с очищенным input), **«Отмена»**.

### 6.6 «Найдено рядом» (второстепенно, но остаётся)

Ниже основного flow — сворачиваемая секция «Найдено рядом» с live-list из `/discover` (когда бэк реализует). Сейчас — пустая, с подписью «Функция активна для устройств, добавленных напрямую в Keystone. Скоро.»

Это оставляем как задел под будущую свою железку с Thread Border Router.

## 7. Экран Device Detail — новый

Открывается тапом на body тайла (не на toggle) с Home.

Route: `/device/:id`.

### Layout

```
┌─────────────────────────────────────┐
│  ‹ Дом                               │
│                                       │
│  WARMBLIXT           [ ⋯ ]           │
│  Гостиная · Matter · Активна         │
│                                       │
│  ┌───────────────────────────────┐  │
│  │                                │  │
│  │   Крупный main control         │  │
│  │   (для light: brightness       │  │
│  │    slider + on/off + preview)  │  │
│  │                                │  │
│  └───────────────────────────────┘  │
│                                       │
│  Цветовая температура                │
│  ├─────●────────────┤                │
│  2700K тёплый                        │
│                                       │
│  ─── История ───                     │
│  · 21:34  включено (правило)         │
│  · 21:12  яркость → 65%              │
│  · 20:45  выключено                  │
│                                       │
│  ─── Автоматизации ───               │
│  · v1.1                              │
│                                       │
│  ─── Настройки ───                   │
│  Комната:   [ Гостиная  ▾ ]          │
│  Имя:       WARMBLIXT                │
│  Тип:       Matter Light             │
│  Node ID:   ......                    │
│  [ Удалить устройство ]              │
│                                       │
└─────────────────────────────────────┘
```

### Content by device type

Одна универсальная структура, controls меняются:

- **light (WARMBLIXT):** main = brightness slider + toggle; sub = color temp slider; для RGB — color picker (v1.1).
- **plug:** main = toggle крупный; sub = current W + total kWh.
- **sensor:** main = readouts крупно; controls нет.
- **motion:** main = «сейчас есть / нет / последнее движение X мин назад»; sub = battery.
- **contact:** main = «открыто / закрыто»; sub = battery.

### История

Секция сворачиваемая, показывает **последние 20 событий из `eventsStore`** для этого device_id. Формат — время + короткое описание («яркость → 65%», «включено (правило)», «выключено (пользователь)»). Origin из `StateSnapshot.Origin` мапится в подпись.

### Настройки

Read-only большинство полей (пока Rooms API нет). Кнопка «Удалить устройство» — `DELETE /devices/{id}` (endpoint пока не реализован — TODO для бэка).

### Из готовых компонентов

Переиспользуем: `Toggle`, `BrightnessSlider`, `ColorTempSlider`, `Chip`, `Button`, `SectionHeader`, `LoadingSkeleton`, `EmptyState`.

Новые:
- `<DeviceMainControl>` — контейнер, выбирает control по типу.
- `<HistoryList>` — список событий с форматтером.
- `<DeviceSettingsSection>` — read-only + Delete кнопка.

## 8. API-контракт — updates

Всё что было в v0.1 остаётся. Плюс/минус:

### Что нужно от бэка для v0.2

**`POST /devices/commission`** (streaming NDJSON) — теперь принимает setup-code:

```
POST /devices/commission
Body: {
  "transport": "matter",
  "payload": "12345678901",   // 11-значный setup code от Apple Home/Google/etc.
  "ecosystem_hint": "apple_home",  // optional, для метаданных
  "room": "Гостиная"           // optional
}

Response stream (NDJSON):
  {"stage":"discovering","message":"..."}
  {"stage":"pairing","message":"..."}
  {"stage":"verifying","message":"..."}
  {"stage":"done","device":{...}}
  {"stage":"error","error":"..."}
```

**`GET /discover?transport=matter`** — **отложен** до v1.x (нужен для собственной железки). Пока endpoint возвращает `410 Gone` с подписью «Reserved for v1.x — commissioning through Keystone hardware».

**`DELETE /devices/{id}`** — TODO для Device Detail screen. Пока фронт может просто `POST /devices/{id}/actions decommission` — семантически то же.

### Форматы Device — JSON поля

Всё остаётся Go-CapitalCase (`ID`, `Name`, `Features`), в `api/types.ts` обёрнуто TypeScript-типами. Позже бэк добавит json-теги — фронт мигрирует одним patch'ем в `types.ts` без затрагивания консьюмеров.

## 9. State management updates

### `state/commissioningStore.ts` — расширение

Текущая структура (упрощённо):

```typescript
type Stage = 'scanning' | 'commissioning' | 'success' | 'error';
{
  stage: Stage;
  found: DiscoveredDevice[];
  addedRefs: Set<string>;
  progress: CommissionEvent[];
  currentRef?: string;
  finalDevice?: Device;
  errorMessage?: string;
  // methods...
}
```

**Расширяем:**

```typescript
type Stage =
  | 'pick-ecosystem'   // NEW — начальный экран
  | 'enter-code'        // NEW — ввод setup code
  | 'scanning'          // остаётся, для v1.x «Найдено рядом»
  | 'commissioning'     // без изменений
  | 'success'
  | 'error';

type Ecosystem = 'apple_home' | 'google_home' | 'smartthings' | 'alexa' | 'other';

interface CommissioningStore {
  stage: Stage;
  ecosystem?: Ecosystem;             // NEW
  setupCode?: string;                // NEW (11-значная строка без dash'ей)
  found: DiscoveredDevice[];         // остаётся
  addedRefs: Set<string>;
  progress: CommissionEvent[];
  currentRef?: string;
  finalDevice?: Device;
  errorMessage?: string;

  // Actions
  pickEcosystem: (e: Ecosystem) => void;   // NEW → stage: 'enter-code'
  setSetupCode: (code: string) => void;    // NEW
  submitCode: () => void;                  // NEW → dispatch commission + stage: 'commissioning'
  backTo: (s: Stage) => void;              // NEW навигация назад
  reset: () => void;
  resetAll: () => void;
  // ... остальное как есть
}
```

## 10. Motion / interactivity

- Все переходы между стадиями Add Device — slide-transitions вправо/влево через CSS transforms + prefers-reduced-motion respect.
- Focus-glow на SetupCodeField при вводе.
- Ambient-подсветка активных Device Tiles.
- Pending-состояние toggle: 300ms opacity + spinner. Если бэк не подтвердил через 3 сек — откат + error.
- Все motion tokens в `theme/tokens.ts` (уже есть).

## 11. Roadmap реализации (последовательно)

Оценка условная — при команде 1 фронтендер full-time.

### Sprint 1 (2 дня) — Cleanup + Home mobile-first

- [ ] Удалить: `Shell`, `LeftNav`, `RightRail`, `CommandPalette`, `IntentCard`, `Modal`, `DeviceLabScreen`, route `/lab`.
- [ ] Переписать `HomeScreen` в mobile-first одноколонный.
- [ ] Верификация: `yarn dev` → mobile-view работает 1-в-1 с dashboard.html.

### Sprint 2 (2 дня) — Add Device redesign

- [ ] `screens/AddDeviceScreen.tsx` — переписать под 4 стадии.
- [ ] `components/EcosystemPicker/` — новый компонент.
- [ ] `components/EcosystemGuide/` — новый компонент.
- [ ] `components/SetupCodeField/` — новый компонент с auto-format.
- [ ] `lib/ecosystem-guides.ts` — тексты инструкций.
- [ ] `state/commissioningStore.ts` — расширить схему.
- [ ] Verification: flow работает end-to-end с мок-бэком (fake stream в `api/discover.ts`).

### Sprint 3 (2 дня) — Device Detail

- [ ] `screens/DeviceDetailScreen.tsx` — новый.
- [ ] Route `/device/:id` в `App.tsx`.
- [ ] `components/DeviceMainControl/`, `components/HistoryList/`, `components/DeviceSettingsSection/`.
- [ ] Navigation с Home: тап на body тайла → detail.

### Sprint 4 (2 дня) — Интеграция с Matter-бэком

- [ ] Backend: `POST /devices/commission` HTTP + streaming.
- [ ] Frontend: replace mock в `api/discover.ts` на реальный.
- [ ] End-to-end тест с виртуальным устройством (`WARMBLIXT` virtual через demo scene).
- [ ] End-to-end тест с реальным WARMBLIXT (когда matter.js sidecar заработает).

### Sprint 5 (1–2 дня) — Полировка

- [ ] Empty/loading/error states для всех трёх экранов.
- [ ] Motion tokens применены везде.
- [ ] Финальная интеграция с макетами дизайнера (когда придут).
- [ ] Первый набор Vitest тестов.

**Итого:** ~9–10 рабочих дней.

## 12. Работа в парах с дизайнером

Параллельно и не блокирует друг друга:

- Sprint 1 (cleanup) — фронт без дизайнера, никаких новых визуалов.
- Sprint 2 (Add Device) — фронт с placeholder-стилями, дизайнер отдаёт финалы к концу спринта.
- Sprint 3 (Device Detail) — новый экран, дизайнер должен успеть с макетами.
- Sprint 4 (интеграция) — стиль уже финальный.

**Что нужно от дизайнера дополнительно к v0.1 брифу:**

1. Скрины/иконки для 4 ecosystem'ов (Apple Home, Google Home, SmartThings, Alexa).
2. Иллюстрации для инструкций «Turn on Pairing Mode» под каждый ecosystem (по 2–3 шага).
3. SetupCodeField макет (focus, filled, error).
4. Device Detail — вся композиция.
5. Success/failure экраны commissioning'а.

## 13. Что уже готово из бэка (сейчас)

- `/devices`, `/devices/{id}`, `/devices/{id}/actions`, `/devices/{id}/state`, `/stream` — работают.
- Demo scene с виртуальным WARMBLIXT (`onoff` + `brightness` + `color_temp`) — можно тестировать все три controls.
- HTTP admin с всем этим — на порту 7777 через `/ui/dashboard.html`.

## 14. Open questions

1. **Скриншоты инструкций.** Кто их делает — дизайнер отрисует mock или берём реальные скриншоты iOS/Android? Мой вкус — иллюстрации в стиле бренда, а не реальные скрины (устареют быстро при апдейтах Apple/Google).
2. **Room-picker в success-экране** — используем hardcoded «Гостиная/Кухня/Спальня» или подтягиваем реальные комнаты из `/rooms` (endpoint пока не реализован)? Мой вкус — hardcoded 4–5 с возможностью «Другая…» текстовым вводом. Rooms API — v1.1.
3. **Delete device UX** — в Device Detail через модалку с confirm? Или свайп-to-delete в списке Home? Первое — понятнее и портируется в RN.
4. **Reset onboarding-флага** — где-то в настройках или совсем не даём? В MVP не даём, добавим в v1.1.
5. **Ecosystem «Прочее»** — что показывать? Настраиваемый Home Assistant / Nabu Casa? Или просто «Скоро добавим больше». Мой вкус — второе.

## 15. Reference — что смотреть в существующем коде

Живые reference-имплементации, к которым обращаться:

- **`site/dashboard.html`** — vanilla-JS клиент. Полностью работающий обвес API + стрим. Показывает как реализовать optimistic updates и reconnect.
- **`apps/web/src/state/devicesStore.ts`** — правильный паттерн Zustand + Map + liveKey.
- **`apps/web/src/hooks/useLiveStream.ts`** — правильная реализация NDJSON парсера в React.
- **`apps/web/src/components/DeviceTile/DeviceTile.tsx`** — как рендерить controls по типу.

Не смотреть как reference (переделываем):
- `apps/web/src/components/{Shell,LeftNav,RightRail,CommandPalette,Modal}` — удаляем.
- `apps/web/src/screens/DeviceLabScreen.tsx` — удаляем.
- `apps/web/src/screens/AddDeviceScreen.tsx` — полностью переписываем.
