# Matter implementation — gap analysis

**Дата:** 2026-08-16 (обновлено после Phase A closure)
**Скоуп:** `sidecars/matter-server/*` (Node.js, matter.js 0.17.9) + `internal/adapters/matter/*` (Go) + wiring через `internal/service/device_service.go` и `cmd/keystone/main.go`.

**Отношение к другим докам:**
- `docs/status.md` — high-level phase state, что done vs что дальше
- `docs/matter-adapter-brief.md` — исходная спека адаптера
- Этот док — детальный audit пробелов, обновляется по фазам

---

## Что уже работает

- Sidecar поднимается, хранит fabric на диске (`matter-data/keystone-controller/`), переживает рестарт.
- WS JSON-RPC request/response круглый: `commission`, `listNodes`, `readAttribute`, `writeAttribute`, `invokeCommand`, `removeNode`.
- Go adapter регистрируется в ядре независимо от готовности sidecar; ретрай коннекта с экспоненциальным backoff (`connectLoop`).
- `Discover` возвращает peer'ов с реальными кластерами (после фикса `Descriptor.ServerList` в `endpointsOf`).
- Delete устройства — best-effort с обеих сторон: адаптер + persistence.
- Realtime state changes через `StateStream(node, opts)` из `@matter/node` — physical toggle из Apple Home приходит в keystone <500 мс.
- WS heartbeat + reconnect после mid-run разрыва.
- Structured error taxonomy: 7 `ErrorKind` (bad_request/not_found/not_ready/unreachable/timeout/unsupported/internal), `sentinelByKind` + `errors.Is` в handler'ах.
- BLE transport + Wi-Fi credentials — out-of-box commissioning без внешней экосистемы (multi-admin остаётся альтернативой).
- Camera WebRTC — Matter 1.4 camera cluster + WebRTC data plane для streaming.
- Command confirmation (`internal/service/confirm.go`) — после state-changing команды ждём report; если не приходит — surface в UI.
- CSA data model generator (`matter_gen.go` / `generated_test.go`) — автогенерация cluster/attribute/command таблиц из official спецификации CSA.

---

## Пробелы

### Блокеры

**1. Push peer-attribute changes** — ✅ **Done** (commit `f422bd8`)
`consumeStateStream` через `StateStream` из `@matter/node` — AsyncGenerator, coalesced-updates всех peer'ов. Физические изменения из Apple Home доходят до keystone за <500 мс.

**2. Auto-subscribe на peer'ах после commission** — ⚠️ **Needs verification**
Явного `subscribeAll` через `ClientNodeInteraction` в коде нет, но StateStream работает в живую — это значит matter.js 0.17.9 сам открывает subscription после `peer.commission()`. Нужен ручной прогон: fresh commission WARMBLIXT → в течение 5 сек начинают идти attribute reports без явного subscribe call'а из нашей стороны.

**3. VendorName / ProductName / DeviceType не читаются** — ✅ **Done** (commits `f422bd8` + `8104d4b`)
Читаем `BasicInformation.VendorName/ProductName/VendorID/ProductID` с endpoint 0, `Descriptor.DeviceTypeList` для типа. WARMBLIXT приходит как «IKEA WARMBLIXT», тип `light`.

**4. Только endpoint#1 адресуется** — ⚠️ **Partial**
Per-feature endpoint mapping добавлен, но `defaultEndpoint = 1` остался как fallback в `adapter.go`. Multi-endpoint устройства не тестировались с живым железом (нужна двухсекционная розетка или световая лента с сегментами).

**5. Начальное состояние не читается после commission** — ⚠️ **Alternative shipped**
Явного «read all bindings post-commission» цикла нет. Вместо этого появился `internal/service/confirm.go` — watch-and-verify: после каждой state-changing команды ждём report, если не приходит — говорим вслух. Это reliability слой, а не полная замена initial-state read'а. Требует ручной проверки: fresh commission → UI показывает актуальные значения в первые 2 сек?

### Робастность

**6. Reconnect после разрыва в середине сессии** — ✅ **Done** (commit `f422bd8`)
`pumpEvents` при закрытии events restart'ит `connectLoop`. Убийство sidecar посреди сессии восстанавливается автоматически.

**7. WS heartbeat** — ✅ **Done** (commit `f422bd8`)
Ping/pong каждые 10 сек, timeout 30 сек → forced reconnect. TCP half-open детектится.

**8. Snapshot после reconnect** — ⚠️ Не проверено; вероятно закрыто через StateStream (peer state подтягивается автоматически при re-attach), но hook `listNodes + ReadState всех devices` post-reconnect в коде не найден.

**9. Нет seq / replay** — ❌ Open. Sidecar-to-keystone WS канал буферизован (128 events), при переполнении молча дропает. Во время цветовой анимации из Home app можно потерять кадры. Для v1 pluggable sidecar-протокола обязательно.

**10. NodeOnline/NodeOffline на реальный connectivity** — ❌ Open. matter.js session-state events не хукаются. keystone продолжает слать команды offline peer'ам.

**11. Sidecar shutdown timing** — ❌ Open. `server.close()` → `controller.stop()` без bounded таймаута.

**12. `readPump` использует `context.Background()`** — ❌ Open (маленький, но фиксить). Cancel-race между `closeCh` и `conn.Read`.

### UX / полнота протокола

**13. Structured error taxonomy** — ✅ **Done** (commit `f422bd8`)
7 `ErrorKind` + `sentinelByKind` map + `errors.Is` в handler'ах вместо regex по тексту. `handleDeleteDevice` различает `ErrKindNotFound` от `ErrKindUnreachable`.

**14. Commissioning progress events** — ✅ **Done** (commit `f422bd8`)
Sidecar эмитит real стадии commissioning'а из `peer.commission()` (discover / PASE / CASE / attest / provision). Эвристические таймеры в HTTP-хэндлере keystone убраны.

**15. Health / ping RPC** — ✅ **Done** (часть heartbeat из #7)
`ping()` RPC отвечает pong'ом.

**16. Cluster naming — вручную** — ✅ **Done** (commit `703e211`)
Генератор `matter_gen.go` из CSA data model — единая таблица id ↔ PascalCase name, автогенерация. Ни один новый device class не требует ручной правки whitelist'а.

**17. Multi-endpoint feature routing** — см. #4 (Partial).

**18. QR / raw payload commissioning** — ⚠️ Улучшено (BLE + Wi-Fi flow добавлен, commit `455df03`), но парсинг QR-кода (`MT:...` формат) явно не проверялся. matter.js `QrPairingCodeCodec.decode` возможно уже используется через BLE-flow, но требует проверки.

**19. Extended Discovery (BLE + mDNS)** — ✅ **Done для BLE** (commit `455df03`)
`CommissionableDeviceDiscovery` через BLE + Wi-Fi credentials работает для out-of-box устройств. Ручной ввод кода больше не единственный путь.

### Мелочи / cleanup

**20. `Adapter.Subscribe` без connected-signal подписчику** — ❌ Open. Полезен был бы event `TransportEventAdapterStatus{connected: bool}`.

**21. `defaultEndpoint = 1`** — см. #4 (partial).

**22. Rate-limit на sidecar** — ❌ Open. Chatty client может засыпать `readAttribute`.

**23. Тестовое покрытие StateStream-пайплайна** — ⚠️ Частично закрыто (появился `coverage_test.go` в matter adapter, 471 строка).

**24. Fabric hygiene** — ✅ **Done** (commit `27e387e`)
Fabric removal не полагается на stale flag'и; консистентное состояние при аварийных сценариях.

**25. Логи**: `matter subscriber slow, dropping event` — без device id, без ratelimit. ❌ Open, малозначимо.

---

## Coverage matrix — какие типы устройств поддерживаем

*Обновлено после bonus работы агентов Phase A (commits `6783641`, `e2a9770`, `703e211`).*

### Что домен знает

10 DeviceType: `light`, `switch`, `plug`, `sensor`, `motion_sensor`, `contact_sensor`, `thermostat`, `cover`, `lock`, `media_player`.

12+ Feature: `onoff`, `brightness`, `color`, `color_temp`, `temperature`, `humidity`, `motion`, `contact`, `power_meter`, `battery`, `lock`, `cover_position`, plus features из bonus работы (mode select, valve control, camera stream).

### Что реально смаплено

После агент-волны с CSA generator (`matter_gen.go`) и full device-type coverage (`6783641`):

- **Lights** (on/off/dim/ct) — full coverage
- **Plugs** (on/off, energy monitoring) — full coverage
- **Sensors** (temperature/humidity/motion/contact/battery) — full coverage
- **Thermostats** — closed через bonus work (`e2a9770`)
- **Fans / Air control** — Fan Control cluster + mode select (`e2a9770`)
- **Valves** — water/gas valves (`e2a9770`)
- **Covers** — WindowCovering (частично, через full-coverage commit)
- **Locks** — DoorLock (частично)
- **Cameras** — Matter 1.4 Camera cluster + WebRTC data plane (`e2a9770`)

### Что остаётся не покрыто

**Датчики:**
- IlluminanceMeasurement (0x0400) — ✅ вероятно закрыто в full-coverage commit, требует верификации
- PressureMeasurement (0x0403), FlowMeasurement (0x0404)
- Air quality: PM2.5, PM10, CO2, TVOC, Formaldehyde, NitrogenDioxide, Ozone, Radon
- SmokeCoAlarm (0x005C)

**Кнопки и сцены:**
- Switch (0x003B) — physical push-buttons (single/double/long taps). Специфичная семантика events, а не toggle state. Возможно частично закрыто в bonus, требует проверки.

**Media:**
- MediaPlayback, MediaInput, KeypadInput, LowPower, WakeOnLan, AudioOutput, ChannelInfo, TargetNavigator, ContentLauncher, ApplicationLauncher, AccountLogin

**Matter 1.2+ appliances:**
- RoboticVacuumCleaner
- LaundryWasher / LaundryDryer / Dishwasher / Refrigerator / Oven / MicrowaveOven / Cooktop
- WaterHeater, WaterValve (частично закрыто)
- ExtractorHood

**Matter 1.3 energy:**
- EVSE (0x0091)
- ElectricalPowerMeasurement / ElectricalEnergyMeasurement (новые кластеры)
- PowerTopology, DeviceEnergyManagement

### Даже в поддержанных кластерах — не все атрибуты

- **OnOff**: `OnTime`, `OffWaitTime`, `StartUpOnOff` (важно для UX «поведение после сбоя питания»)
- **LevelControl**: `MinLevel`/`MaxLevel`, `OnLevel`, `OnOffTransitionTime`
- **ColorControl**: `ColorMode`, `EnhancedColorMode`, `ColorLoopActive`
- **ElectricalMeasurement**: `Voltage`, `Current`, `PowerFactor`, `Frequency`, `ApparentPower`, `ReactivePower`
- **PowerSource**: `BatChargeLevel` (enum), `BatChargeState`, `BatVoltage`, `BatReplacementNeeded`

### Команды не задействованы полностью

- `OnOff.OnWithTimedOff` — «включи на 5 минут» атомарной командой
- `LevelControl.MoveToLevelWithOnOff` — если выключен, включит
- `LevelControl.Move`/`Step`/`Stop` — плавные диммеры, hold-to-dim
- `ColorControl.MoveToHueAndSaturation`, `MoveToColor` (X/Y), `StepHue`, `StepSaturation`, `MoveColor`, `StopMoveStep`
- `Thermostat.SetpointRaiseLower`

---

## Что делать дальше

Основные блокеры v1 API Matter'а закрыты. Оставшееся условно разделяется на:

**Верификация текущих**:
- Auto-subscribe (#2) — прогон WARMBLIXT
- Multi-endpoint routing (#4) — эмуляция или физический тест
- Initial state read (#5) — сценарий UI-без-«неизвестно»
- Snapshot после reconnect (#8) — kill sidecar → check state consistency

**Робастность гэпы**:
- seq/replay (#9) — нужен для v1 sidecar protocol'а, при переезде в отдельную плагин-репу
- NodeOnline/Offline real connectivity (#10)
- Sidecar bounded shutdown (#11)
- readPump context race (#12) — маленький, можно быстро закрыть

**Coverage расширения** (по мере необходимости):
- Верификация bonus работы: какие device-types реально работают на живом железе?
- Air quality — если появятся конкретные устройства пользователей
- Media, appliances, EVSE — post-v1

**Post-extraction (когда Matter станет плагином через plugin-manager)**:
- Sidecar Protocol v1 conformance — Matter sidecar как первый живой consumer
- Config Flow через Layer 2 (см. `plugin-ui-integration.md` §4) — заменяет текущий HTTP-стрим commissioning'а

Общая оценка: **~3-5 дней** довести Matter до полного v1-ready (верификация + robustness gaps + extraction). Post-v1 расширения — по мере роста экосистемы плагинов.
