# Keystone × Matter (multi-admin через любую экосистему) — бриф разработки

**Статус:** v0.1
**Роль:** backend-инженер (Go + Node.js sidecar)
**Родительские документы:**
- [`go-core-spec.md`](../../../claude/go-core-spec.md) — устройство ядра.
- [`smart-home-engine-brief.md`](../../../claude/smart-home-engine-brief.md) — продуктовая рамка.
- [`frontend-brief.md`](./frontend-brief.md) — фронт использует те же endpoints.

## 0. TL;DR

Делаем **Matter-adapter** для keystone. **Первичный commissioning идёт через любую Matter-экосистему пользователя** (Apple Home / Google Home / SmartThings / Amazon Alexa — что уже стоит), а keystone добавляется вторым администратором фабрики через **Matter multi-admin**. Это даёт нам:

- Реальную работу с WARMBLIXT и любым другим Matter-устройством.
- Не нужно писать свой QR-scanner и commissioning UI для MVP.
- Не нужен собственный Thread Border Router — Apple TV/HomePod/Google Nest у пользователя уже им работает.
- Если keystone упадёт — родная экосистема продолжит управлять устройствами.
- Работает **со всеми** major smart-home экосистемами одинаково — Matter multi-admin это стандарт CSA.

Транспорт с Matter — через **matter.js sidecar** (Node.js процесс), Go общается с ним по WebSocket JSON-RPC. Это ровно тот же паттерн, что использует Home Assistant с python-matter-server, только на TypeScript.

**Целевое устройство для теста:** IKEA WARMBLIXT (Matter over Thread, tunable white). В примерах ниже используем Apple Home как самую распространённую в нашей аудитории — но flow идентичен для Google Home / SmartThings / Alexa.

**v1.x roadmap:** когда у нас появится собственная железка с Thread Border Router и primary commissioner — добавим self-commissioning path (пользователь сканирует QR прямо в keystone, без промежуточной экосистемы). Multi-admin остаётся как альтернативный путь.

## 1. Почему именно так

### Почему Matter

- Стандарт индустрии, растёт быстрее всех остальных экосистем.
- Одна абстракция — потом легче добавить Nanoleaf, Aqara, Eve, etc.
- Транспортно-агностичен: работает поверх Wi-Fi, Thread, Ethernet.

### Почему через multi-admin (любая экосистема), а не сами commissioning'ит

**Плюсы multi-admin через существующую экосистему:**

1. **Не нужен Thread Border Router на нашей стороне** — Apple TV / HomePod / Google Nest Hub / SmartThings Hub у пользователя уже роутит Thread.
2. **Не нужен свой QR-scanner в MVP** — родное приложение делает это глазами.
3. **Fail-safe:** если keystone сломается, родная экосистема продолжает управлять устройствами. Пользователь не остаётся без света.
4. **UX первоклассный без наших усилий** — Apple/Google/Amazon шлифуют commissioning UI годами.
5. **Работает со всеми** — multi-admin это Matter-стандарт от CSA, поведение одинаковое во всех совместимых экосистемах.
6. **Позже** мы добавим собственное primary commissioning для тех, у кого никакой экосистемы нет (тогда — со своей железкой + Thread Border Router).

**Минусы:**

- Пользователь **обязан** иметь хоть какой-то Matter-хаб (Apple TV / HomePod / Google Nest / SmartThings / Amazon Echo с Thread). Для нашего mass-market позиционирования — это допустимая цена входа: WARMBLIXT-покупатели чаще всего уже в одной из этих экосистем.
- Multi-admin flow требует ~5 тапов в родном приложении, не самое очевидное место в настройках. Мы это компенсируем чёткими инструкциями в UI keystone (`frontend-brief.md §6.2`).

### Почему matter.js sidecar, а не native Go

Native Go-реализации Matter не существует. Варианты:
- **matter.js (TypeScript, Node.js)** — самая живая, боевой опыт (matterbridge, Home Assistant Matter Hub). ✓ выбор
- **connectedhomeip (C++)** — референс от CSA, но интеграция дорогая (cgo + тяжёлый build).
- **rs-matter (Rust)** — только device-mode, controller ещё не готов.

Позже, когда rs-matter дозреет — можем переехать. Контракт между Go-ядром и sidecar останется тот же.

## 2. Архитектура

```
┌─────────────────────────────────────────────┐
│              Keystone Go binary              │
│                                              │
│  ┌────────────────────┐                     │
│  │  Matter Adapter    │                     │
│  │  (Go, ports.Adapter)│                     │
│  └─────────┬──────────┘                     │
│            │ WebSocket JSON-RPC              │
│            │ ws://localhost:5580             │
└────────────┼─────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────┐
│         matter.js server (Node.js)           │
│         port 5580, TS/JS                     │
│                                              │
│  - Matter controller / commissioner          │
│  - Хранит fabric-состояние в ./matter-data  │
│  - Broadcasts state changes over WS         │
└────────────┬─────────────────────────────────┘
             │ Matter protocol (Wi-Fi/Thread)
             ▼
┌─────────────────────────────────────────────┐
│  Apple TV 4K или HomePod                     │
│  (Thread Border Router)                      │
└────────────┬─────────────────────────────────┘
             │ Thread
             ▼
┌─────────────────────────────────────────────┐
│  IKEA WARMBLIXT (Matter over Thread lamp)    │
└─────────────────────────────────────────────┘
```

**Все процессы — на одном хосте (Mac / Pi). Общаются по localhost, ноль латентности между Go и matter.js.**

## 3. Multi-admin flow (пользовательский путь)

Пошагово, что делает пользователь:

**Один раз при установке:**
1. Пользователь ставит keystone (наш бинарь + docker-compose с matter.js).
2. Keystone стартует и объявляется в mDNS как commissionable Matter node.

**На каждое новое устройство:**
1. Пользователь commissioning'ит WARMBLIXT в **своей экосистеме** — Apple Home / Google Home / SmartThings / Amazon Alexa. Наводит камеру iPhone/Android на QR-код на коробке WARMBLIXT. Через 30 секунд лампа в родной экосистеме.
2. В родном приложении пользователь открывает настройки лампы → **«Turn on Pairing Mode»** (в Apple), **«Linked services → Add another Matter-enabled app»** (в Google), **«Share with Matter → Generate Code»** (в SmartThings), **«Other Assistants and Apps»** (в Alexa). Приложение генерирует одноразовый setup code (11-значный).
3. Пользователь открывает keystone-приложение → Add Device → выбирает свою экосистему → **вводит этот 11-значный код**.
4. Keystone передаёт код в matter.js → matter.js commissioning'ится как второй admin в фабрику WARMBLIXT.
5. Лампа теперь в двух фабриках — родная экосистема и Keystone. Обе могут её включать/выключать параллельно.

**UI-требование (детально в `frontend-brief.md §6`):** экран Add Device — ecosystem picker (Apple/Google/SmartThings/Alexa) → инструкция под выбранную → setup-code input. QR-scanner в MVP не нужен, добавим в v1.x когда появится своя железка.

## 4. matter.js sidecar

### Что берём

- **[matter.js](https://github.com/matter-js/matter.js)** — Apache 2.0, TypeScript, активнейший проект в Matter-экосистеме.
- Из него — `@matter/nodejs` и `@matter/protocol` пакеты.
- Wrapper `@matter/nodejs-shell` даёт готовый CLI + WebSocket API, но он более «demo» — для нас нужен свой минимальный сервер.

### Свой mini-server (пишем сами)

Файл: `sidecars/matter-server/index.ts` — ~150 строк. Задача:

1. Стартует matter.js Controller и Commissioner.
2. Слушает `ws://localhost:5580` — принимает JSON-RPC команды от Go.
3. Broadcasts state changes всем подключенным клиентам.
4. Персистит fabric-state в `./matter-data/` (matter.js сам это делает).

### JSON-RPC контракт

Простой protocol: каждое сообщение — `{"id":"...", "method":"...", "params":{...}}` или ответ `{"id":"...", "result":{...}}` или ошибка `{"id":"...", "error":{"code":..., "message":"..."}}`. Плюс серверные события `{"event":"...", "data":{...}}`.

**Методы:**

```
commission(setupCode: string) -> { nodeId: string, fabricIndex: number }
listNodes() -> Node[]
readAttribute(nodeId: string, endpointId: number, cluster: string, attribute: string) -> Value
writeAttribute(nodeId: string, endpointId: number, cluster: string, attribute: string, value: Value) -> void
invokeCommand(nodeId: string, endpointId: number, cluster: string, command: string, args?: object) -> Result
removeNode(nodeId: string) -> void
```

**События (server → client):**

```
{"event":"attributeChanged", "data":{"nodeId":"...", "endpointId":1, "cluster":"OnOff", "attribute":"OnOff", "value":true}}
{"event":"nodeOnline", "data":{"nodeId":"..."}}
{"event":"nodeOffline", "data":{"nodeId":"..."}}
{"event":"commissioningProgress", "data":{"stage":"pairing", "message":"..."}}
```

Полный формат — определим в `sidecars/matter-server/protocol.ts` + `internal/adapters/matter/protocol.go` (совместимо).

### Deployment

**Локально в dev:** `docker-compose up` — keystone + matter.js в двух контейнерах, network mode `host` для Matter mDNS discovery (нужен bridge/host сеть для Bonjour).

**Reference-hardware:** systemd — два юнита (`keystone.service` + `keystone-matter.service`).

## 5. Go-сторона (Matter Adapter)

Файл: `internal/adapters/matter/`

Структура:
```
internal/adapters/matter/
├── adapter.go        # ports.Adapter реализация
├── client.go         # WebSocket JSON-RPC клиент к matter.js
├── protocol.go       # типы JSON-RPC + событий
├── mapping.go        # Matter clusters ↔ наши Feature'ы
└── config.go         # host + port sidecar'а
```

### Реализация Adapter interface

- `Kind()` → `TransportMatter`
- `Start(ctx)` → устанавливает WS-соединение с matter.js, subscribe на events
- `Stop(ctx)` → closes WS
- `Discover(ctx)` → JSON-RPC `listNodes`, каждый node → `DiscoveredDevice`
- `Commission(ctx, req)` → JSON-RPC `commission(setupCode)` (нужен `req.Payload` == 11-digit code)
- `ReadState(ctx, ref, feature, key)` → `readAttribute` с маппингом feature → (endpoint, cluster, attr)
- `WriteState(ctx, ref, feature, key, value)` → `writeAttribute`
- `InvokeAction(ctx, ref, feature, action, params)` → `invokeCommand` (например, on/off → OnOff cluster, On/Off command)
- `Subscribe(ctx)` → возвращает канал, куда льются `attributeChanged`/`nodeOnline`/`nodeOffline` события, декодированные в `TransportEvent`
- `Decommission(ctx, ref)` → `removeNode`

### Маппинг Matter → наша Feature-модель

Полный список Matter clusters — [здесь](https://project-chip.github.io/connectedhomeip-doc/spec_v1.4/index.html). Для MVP нужны:

| Matter cluster | Наш `FeatureKey` | Атрибут / команда |
|---|---|---|
| OnOff (0x0006) | `onoff` | attribute `OnOff` (bool), commands `On`/`Off`/`Toggle` |
| LevelControl (0x0008) | `brightness` | attribute `CurrentLevel` (uint8 0..254 → мы масштабируем в 0..100) |
| ColorControl (0x0300) | `color_temp` | attribute `ColorTemperatureMireds` (uint16 Mireds → мы конвертим в Kelvin: `K = 1_000_000 / mireds`) |
| ColorControl (0x0300) | `color` | attributes `CurrentHue`, `CurrentSaturation` |
| TemperatureMeasurement (0x0402) | `temperature` | `MeasuredValue` (int16 в 0.01°C) |
| RelativeHumidityMeasurement (0x0405) | `humidity` | `MeasuredValue` (uint16 в 0.01%) |
| OccupancySensing (0x0406) | `motion` | `Occupancy` (bitmap → bool) |
| BooleanState (0x0045) | `contact` | `StateValue` (bool) |
| ElectricalMeasurement (0x0B04) | `power_meter` | `ActivePower`, `TotalActiveEnergy` |
| PowerSource (0x002F) | `battery` | `BatChargeLevel` / `BatPercentRemaining` |

Файл `mapping.go` — таблица + функции `matterToFeature(cluster, attr, val)` и `featureToMatter(feature, key, val)`.

## 6. Фазы разработки

### Phase 1 — matter.js sidecar (2–3 дня)

- [ ] `sidecars/matter-server/` — Node.js проект.
- [ ] `package.json` с зависимостями `@matter/main` (текущий metapackage).
- [ ] `index.ts` — стартует Controller + WebSocket-сервер на :5580.
- [ ] Хранение fabric-state в `./matter-data/`.
- [ ] JSON-RPC handler'ы для `listNodes`, `readAttribute`, `writeAttribute`, `invokeCommand`, `commission`, `removeNode`.
- [ ] Broadcast `attributeChanged` при подписке.
- [ ] Dockerfile.
- [ ] docker-compose для локального dev.

**Тест Phase 1:** через `wscat` вручную commissioning'уем WARMBLIXT (после того как он в Apple Home + pairing mode). `listNodes` возвращает наш node. `writeAttribute(OnOff.OnOff, true)` — лампа физически включается.

### Phase 2 — Go Matter adapter (2 дня)

- [ ] `internal/adapters/matter/` — весь пакет.
- [ ] WebSocket клиент к matter.js (`gorilla/websocket` или свой минимальный — стрим JSON-lines через `nhooyr.io/websocket`).
- [ ] Полная реализация `ports.Adapter`.
- [ ] Маппинг из/в Matter clusters (для MVP: OnOff, LevelControl, ColorControl mireds).
- [ ] Config: `matter.json` в `<data>/` с `{host: "localhost", port: 5580}` (опционально — вообще не нужно, всё дефолтит в localhost).
- [ ] Регистрация в `main.go` — enable если matter.js доступен (ping WS при старте).
- [ ] Ingress loop — attribute events из sidecar'а → внутренний event bus.

**Тест Phase 2:** после Phase 1 + Phase 2 — WARMBLIXT появляется в `GET /devices` keystone'а с features `onoff`+`brightness`+`color_temp`, `POST /devices/{id}/actions turn_on` физически включает лампу.

### Phase 3 — Commissioning UI (1–2 дня)

Работаем в паре с фронтендером. Из бэка:

- [ ] HTTP `POST /devices/commission` — streaming response с прогрессом. Проксирует `commission(setupCode)` в matter.js.
- [ ] Обработать stages: `discovering` → `pairing` → `verifying` → `done`.

Фронт (см. `frontend-brief.md`):

- [ ] В экране Add Device — раздел «У меня есть устройство в Apple Home».
- [ ] Поле для 11-значного кода (auto-format `1234-5678-901`).
- [ ] Инструкция с 3 картинками: где найти "Turn on Pairing Mode" в iOS Home.

**Тест Phase 3:** пользователь в приложении вводит код, WARMBLIXT в keystone за ~20 секунд.

### Phase 4 — Стабилизация (1–2 дня)

- [ ] Reconnect logic между Go и matter.js (WS падает — Go retry с exp-backoff).
- [ ] Persistence device-list (уже есть в keystone) — синхронизация между matter.js fabric и Go registry по `nodeId`.
- [ ] Обработка offline-устройств.
- [ ] Логирование, метрики.
- [ ] Docs — `docs/matter-setup.md` для пользователя.

## 7. Требования к среде

**Host:**
- Node.js 20+ (для matter.js).
- Docker (для docker-compose).
- Bonjour/mDNS не блокирован файрволлом.
- Host-mode network в docker (`network_mode: host`) — обязательно для Matter discovery.

**Локальная сеть:**
- IPv6 включен (Matter over Thread требует).
- Apple TV 4K или HomePod в той же сети — как Thread Border Router.
- Пользователь на iPhone/iPad с iOS 16.1+ (multi-admin из Apple Home).

## 8. Что берём из существующего кода

- `ports.Adapter` interface — не меняется.
- `service.DeviceService.AddDiscovered` — используется для регистрации устройств, найденных adapter'ом.
- `service.DeviceService.Commission` — уже вызывает `Commission()` через нужный adapter по kind.
- HTTP endpoint `POST /devices/commission` — не написан ещё, добавим в Phase 3.
- Event bus и rules engine — работают из коробки.

## 9. Risks и open questions

### Риски

1. **matter.js API нестабилен** — библиотека активная, breaking changes. **Митигация:** пиним конкретную версию, вендорим типы, регрессионные тесты через WARMBLIXT.
2. **Multi-admin не всегда работает надёжно** — некоторые Matter-устройства «забывают» второго admin при apple firmware-updates. **Митигация:** документируем «если пропало — снова pair-mode из Apple Home».
3. **Thread commissioning требует специфической сети** — IPv6 mandatory, некоторые роутеры блокируют. **Митигация:** документируем требования, добавляем `keystone doctor` команду для проверки.
4. **Пользователь может не иметь Apple TV/HomePod** — тогда multi-admin невозможен. **Митигация:** документируем требование в MVP. В v1.1 добавляем self-hosted Thread Border Router (можно на Raspberry Pi с OpenThread).

### Open questions

1. **Storage matter-fabric-state.** matter.js хранит в файлах. Персистить ли в `<keystone-data>/matter-fabric/` или в отдельной volume? Мой вкус — в keystone-data, вместе с devices.json/rules.json.
2. **QR-код или setup code?** Apple Home «Turn on Pairing Mode» отдаёт код текстом (11 цифр). QR-код у нас не работает через Apple flow. В UI — сразу текстовое поле; QR-scanner отложим до primary commissioning.
3. **Backup fabric.** matter.js fabric в файле — можно ли его копировать между машинами? Ответ — «в теории да, но не поддерживается официально». В MVP — не заморачиваемся; при переезде user re-pair'ит.
4. **Диагностика.** matter.js даёт хорошие логи, но их формат неудобен. Пробросить в наш slog как отдельный child-logger?

## 10. Ссылки / reference

- [matter.js repo](https://github.com/matter-js/matter.js)
- [matter.js example controller](https://github.com/matter-js/matter.js/tree/main/packages/nodejs-shell)
- [matterbridge](https://github.com/Luligu/matterbridge) — референс-реализация архитектуры, ровно как у нас (Node.js sidecar с WS + client приложение)
- [Home Assistant python-matter-server](https://github.com/home-assistant-libs/python-matter-server) — тот же паттерн, но с Python-обёрткой над connectedhomeip. Хорошая референс-имплементация JSON-RPC контракта.
- [Matter Data Model — Google](https://developers.home.google.com/matter/primer/device-data-model)
- [Apple Home multi-admin docs](https://support.apple.com/guide/iphone/set-up-and-share-accessories-iph1c50f4bce/ios)
- [WARMBLIXT product page](https://www.ikea.com/us/en/p/warmblixt-led-table-decoration-yellow-70544964/) — Matter over Thread lamp

## 11. Итоговая оценка сроков

| Phase | Работа | Дни |
|---|---|---|
| 1 | matter.js sidecar + docker | 2–3 |
| 2 | Go Matter adapter | 2 |
| 3 | Commissioning UI (совместно с фронтом) | 1–2 |
| 4 | Стабилизация | 1–2 |
| **Итого** | | **~7–9 рабочих дней** |

Плюс — работа фронтендера параллельно с Phase 3 (~1 день).

## 12. Отношение к DIRIGERA-адаптеру (откат)

Прошлая попытка через DIRIGERA была разумной, но:
- Требует физической кнопки на хабе → user friction.
- Работает только с IKEA-устройствами → узко.
- Локальная REST-API не документирована официально → хрупко.

Мы **откатываем** DIRIGERA-код из `main.go` и `Makefile`, пакет `internal/adapters/dirigera/` перемещаем в `docs/_to_delete/dirigera/` (для истории). При желании можно вернуть позже.

**Заменяем на Matter-adapter через Apple Home** — более общий, стандартизованный, с лучшим UX.
