# keystone-esp — обзор проекта и сводный бэклог

Карта: что это, что уже работает, что осталось. Пишется на русском, канонические
документы остаются на английском — ссылки проставлены.

- **Что строим и зачем:** [SPEC.md](../SPEC.md) §1
- **Инварианты архитектуры** (нарушать нельзя): [SPEC.md](../SPEC.md) §3
- **Текущий статус по фичам, с датами и верификацией:** [CLAUDE.md § Status](../CLAUDE.md)
- **Домениые бэклоги:** [brain](brain-backlog.md), [matter](matter-backlog.md), [zigbee](zigbee-backlog.md), [app-protocol](app-protocol.md), [cross-cutting](backlog.md)
- **Роадмап и приоритеты:** [roadmap.md](roadmap.md) — единственный источник правды о порядке работ

---

## 1. Что это

Локальный мульти-чиповый ESP32-хаб умного дома на C++/ESP-IDF. Переизобретает
*ядро* Home Assistant — шину событий, реестр сущностей со стейт-машиной,
rule engine — **без** его интеграций, истории, template-движка и web-UI. Хаб
не отдаёт свой UI: отдельное приложение (Expo, iOS/Android/web) ходит к нему
по WebSocket+JSON с токеном и пиннингом сертификата
([docs/app-protocol.md](app-protocol.md)).

**Централизованный хаб, не распределённая mesh:** все «мозговые» чипы в одной
коробке, связаны проводами. Распределены только *устройства* на радио. Это
осознанная цена: один SPOF взамен сильно проще систему (нет распределённого
консенсуса, правила выполняются в одном месте).

Явные non-goals (SPEC §1): распределённое ядро, ESP-NOW между нашими платами,
Matter-controller в масштабе, Thread border router, полноценная БД истории.

### Инварианты (SPEC §3 — не ломаем)

1. Транспорт — абстракция: ядро говорит с `Bus` (`publish`/`subscribe`), не с UART/SPI.
2. Протокол независим от топологии: `chip_id` живёт в заголовке даже на point-to-point.
3. Brain отделим от радио-стеков: ядро должно мигрировать S3 → P4 → Linux без изменений.
4. Один дескриптор возможностей кормит и внутренний реестр, и Matter-мост — всё в форме Matter-кластеров с первого дня.
5. У ядра нет радио.

### Карта кода (SPEC §8)

| Директория | Что | Переносимость |
|---|---|---|
| [`libhubproto/`](../libhubproto/) | COBS+CRC16 фрейминг, 8-байтный header, кодеки payload | C++17 портативный, host-тестируемый |
| [`core/`](../core/) | Bus, EntityRegistry, StateMachine, RuleEngine, CommandDispatcher, Reconciler, Timers/Schedules/Scenes, OTA, app-protocol | C++17 без радио, host-тестируемый |
| [`firmware/brain-s3/`](../firmware/brain-s3/) | ESP-IDF, brain (ESP32-S3-Zero), Wi-Fi + SNTP + WebSocket-сервер | — |
| [`firmware/zigbee-c6/`](../firmware/zigbee-c6/) | ESP-IDF, Zigbee координатор (ESP32-C6, on-chip 802.15.4) | — |
| [`firmware/thread-c6/`](../firmware/thread-c6/) | ESP-IDF+esp-matter, Matter Bridge над on-chip Thread (ESP32-C6, chip 0x02) | Docker image |
| [`app/`](../app/) | Expo client + мок-хаб + инструменты | TS, iOS/Android/web |
| [`firmware/recorder-cam/`](../firmware/recorder-cam/) | ESP-CAM, SD-recorder — пока stub (Phase 7) | — |

---

## 2. Текущее состояние по подсистемам

Это сжатая выжимка. Полный лог со ссылками на коммиты, тесты и hardware-verify
живёт в [CLAUDE.md § Status](../CLAUDE.md).

### 2.1 Транспорт и ядро (`libhubproto` + `core`) — ✅ готово

- Фрейминг COBS+CRC16, header-codec, payload-кодеки (DECLARE/STATE/COMMAND/CMD_ACK + OTA 0x50–0x58).
- `UartTransport`, `EntityRegistry`, `RadioCoordinator` (boot_id resync на любом сообщении), `StateMachine`, `CommandDispatcher`, `RuleEngine`, `ChipMonitor`, `DesiredState`+`Reconciler`, `TimerTable`, `ScheduleTable`, `SceneTable`, `EntityNames`, `EventRing`.
- Приложен слой: `json.hpp`, `app_protocol.hpp`, `app_requests.hpp`, `sun.hpp` (NOAA, сверен с `astral`).
- OTA с stop-and-wait, SHA-256, confirm-gate с откатом.
- ~349 host-тестов.

### 2.2 Brain (`firmware/brain-s3`) — MVP готов

Запущено и проверено на железе:
- Wi-Fi STA + SNTP, пароль Wi-Fi в NVS, 30-секундный trial при смене сети с автооткатом, `wifi.scan` для приложения.
- Персистентные правила (с окном времени, доп. условием и cooldown), расписания (включая once-on-date и sunrise/sunset ±180 мин), таймеры, сцены со стабильным id, имена сущностей, desired-state + reconciler.
- Chip-мониторинг (silent 20 с, flapping ≥3 перезагрузок за 5 мин), `self`-entities (uptime/heap/boot_id/rx-tx/CRC) каждого чипа.
- OTA обоих C6 с проверкой образа и откатом (`ota <chip> <url> <sha256>`, `ota status`).
- WebSocket-сервер wss://:8443 (TLS 1.2 ECDHE-ECDSA/AES-GCM), токен в NVS, пининг через SHA-256 сертификата, mDNS `keystone.local` + `_keystone._tcp`.
- Management-операции по сокету: `name.set/clear`, `rule.add/del`, `schedule.add/del`, `timer.add/cancel`, `scene.put/del/apply`, `location.set/clear`, `wifi.set`/`wifi.scan`, `pair`/`unpair`/`identify`.
- Setup-режим: пустой хаб поднимает WPA2 AP `Keystone-XXXX`, принимает только `wifi.scan`+`setup.apply`. **Проверено по LAN; сам AP и телефонный флоу не прогонялись.**
- Event log: типизированные события в кольце на 256 штук, push в приложение каждые 250 мс, `history {before, limit, entity}`, console `events`.
- Консоль USB-Serial-JTAG: `home`, `get`, `on|off|toggle`, `in`, `at`, `rule`, `scene`, `name`, `desired`, `chips`, `events`, `ota`, `wifi_*`, `webtoken`, `webplain`, `heap`.

Открыто: см. бэклог ниже.

### 2.3 Zigbee (`firmware/zigbee-c6`) — ✅ координатор работает

- Нативный ESP-IDF 5.5, `esp-zigbee-sdk`, on-chip 802.15.4.
- Переписан с «только switch» на таблично-управляемую модель: по кластерам определяется тип устройства ([`core/zcl_profile.hpp`](../core/include/core/zcl_profile.hpp)).
- Поддерживается: On/Off, Level Control, Color Control (xy + CT), Electrical Measurement (power_w), Metering (energy_wh), Temperature/Humidity/Illuminance/Occupancy/Contact (через IAS Zone enrol), Thermostat, Generic Switch (кнопки — 0.4 с pulse), Battery на non-mains.
- NVS: сеть, устройства по IEEE+endpoint; boot-time ZDO NWK-Address для short-address updates; retry back-off (30 с → 60 с → 120 с → 5 мин).
- Pair/unpair/identify из консоли и приложения (unpair на реальном устройстве **не прогонялся**).
- `ReadGuard`: ответ на старое чтение в пределах 5 с после COMMAND дропается и перечитывается через 1.5 с.
- OTA своей прошивки — работает (667 KB за ~78 с).

**На живом железе проверено только с IKEA INSPELNING plug.** Остальные типы устройств написаны и host-тестированы, но на реальных Zigbee-устройствах не прогонялись.

### 2.4 Matter (`firmware/thread-c6`) — bridge-mode готов

- Полноценный Matter Bridge: aggregator endpoint (0x000E) + динамические bridged endpoints через `app_bridge_*`, персистируется в NVS.
- Выбор типа endpoint-а по атрибутам ([`core/matter_plan.hpp`](../core/include/core/matter_plan.hpp)): On/Off, Dimmable, CT-Light, Extended Color Light, Temp/Humidity/Light/Occupancy/Contact Sensor, Thermostat, Generic Switch.
- Доп. кластеры: EPM (0x0090, ActivePower mW) и EEM (0x0091, CumulativeEnergyImported mWh) на endpoint-ах с метерингом.
- Apple Home-ком: iPhone поднимает хаб по BLE с тестовым DAC (`3497-0112-332`, discriminator 3840, PIN 20202021), затем Thread. Каждый toggle в Home → реальный плаг через Matter → S3 → Zigbee → CmdAck → STATE обратно → Matter attribute update.
- Unpair удаляет bridged endpoint на хабе (`Undeclare` 0x04, `app_bridge_remove_device` под stack lock). Собрано и прошито; на реальной отвязке не прогонялось.
- Минимальная консоль на UART0: `factoryreset`, `reboot`.
- Build only in Docker: `espressif/esp-matter:release-v1.6_idf_v5.5.5` (3 route-попытки — см. [matter-backlog.md](matter-backlog.md) §Known blockers).

### 2.5 App (`app/`) — четыре экрана работают

- Стек: Expo (React Native) + web, mock-хаб для тестов (`npm run mock-hub`), 322 vitest.
- Экраны: Setup (новый хаб через AP), Connect (адрес + токен + fingerprint; список найденных по mDNS), Devices (карточки с switch/яркостью/живыми значениями, Add device, Edit, Remove, Blink), Automations (schedules, timers с live-countdown, rules с окном/условием/cooldown, Scenes с редактированием), Hub (chips, Wi-Fi, Location, скоро — Apple Home bridge), History (события по дням, фильтр по устройству, Live push).
- Нативный модуль `app/modules/keystone-net`: Swift (`URLSessionWebSocketTask` + сверка SHA-256 сертификата, `NetServiceBrowser`) и Kotlin (OkHttp pin, NsdManager).
- Веб-экспорт работает: `expo export --platform web`.
- iOS-сборка: симулятор запущен, нативный Swift впервые скомпилировался на Xcode 26.6; **на физическом iPhone не запускался**. Android-сборка не прогонялась.

---

## 3. Сводный бэклог

> ⚠️ **Приоритеты живут в [roadmap.md](roadmap.md)** — это единственный источник правды
> о порядке работ. Таблица ниже — срез на момент её написания (29.10.2026), не источник:
> часть пунктов уже сделана или переехала в роадмап.

Приоритизация моя, обоснования короткие. Три яруса: Now = завершить то, что уже
начато или блокирует MVP; Next = нужно скоро, но не блокер; Later = в идее.

### 3.1 Now — довести до hardware-verify

| # | Что | Почему сейчас | Источник |
|---|---|---|---|
| N1 | ✅ **Карточка Apple Home bridge на Hub-вкладке** приложения: большой код `3497-0112-332`, кнопка «скопировать», инструкция. | Финальный шаг first-time setup, без него человек не поймёт, как добавить хаб в Apple Home. | [app/src/ui/HubScreen.tsx](../app/src/ui/HubScreen.tsx) |
| N2 | **Прогон iOS-билда на физическом iPhone**: Local Network permission, три pin-проверки (правильный, неверный, подменённый сертификат), discovery через mDNS. | Swift-модуль ни разу не выполнялся на устройстве; симулятор не воспроизводит LAN-permission. | [docs/ios-build.md](ios-build.md), [app-protocol.md](app-protocol.md) §10 |
| N3 | **Setup через AP на телефоне**: виден ли `Keystone-XXXX`, отвечает ли 192.168.4.1:8443 по WPA2, Swift/Kotlin `connectLearning`. | Полный setup-флоу не прогонялся — Pi в стенде одноинтерфейсный. | [app-protocol.md §11](app-protocol.md), [brain-backlog.md §3](brain-backlog.md) |
| N4 | **Unpair на реальном устройстве**: сам Leave + HELLO + Matter-сторона (Undeclare→`app_bridge_remove_device`). | Код написан и прошит, но проверка снимает единственный плаг — нужен второй или готовность переподключать. | [zigbee-backlog.md §6](zigbee-backlog.md), [matter-backlog.md](matter-backlog.md) |
| N5 | **Подтвердить фикс thread-c6 `energy_wh` crash** в прод-run (ScopedChipStackLock). | Собрано, прошито, но runtime path с реальным накоплением энергии не прогонялся. | [CLAUDE.md § Status](../CLAUDE.md) |
| N6 | **Проверить ZDO rejoin / retry back-off** на реальном возврате устройства. | Три шага back-off в логе видны, но возвращение плага и re-arm по retry не ловились. | [CLAUDE.md § Status](../CLAUDE.md) |
| N7 | **Персистентный sink для event log** (батченые записи в LittleFS / SD раз в минуту + на `boot`/`panic`). | Кольцо на 256 событий пустеет после ребута; событий `boot` без истории теряется половина пользы. | [brain-backlog.md §7](brain-backlog.md) |
| N8 | **Live push на телефоны** — APNs/FCM через наш relay. | Пока хаб уходит в фон на iOS, подписка на push закрывается; на историю смотрят только когда открывают приложение. | [backlog.md § Mobile app](backlog.md), [brain-backlog.md §8](brain-backlog.md) |

### 3.2 Next — расширить

**Zigbee на реальных устройствах** ([zigbee-backlog.md](zigbee-backlog.md))
- Прогон IAS Zone (контакт/motion/дым/протечка) на реальных сенсорах.
- Прогон термостата и Generic Switch (кнопок) на реальных устройствах.
- Multi-endpoint (power-strip, bulb с CT, 3-кнопочный switch): switch-table уже keyed on `(short_addr, endpoint)`, нужен DECLARE-per-endpoint с discovery.
- Zigbee Groups+Scenes (0x0004/0x0005) — один радиокадр на группу вместо N COMMAND-ов.
- Zigbee OTA (0x0019) — IKEA регулярно шлёт прошивки.
- Network ops: `chan <N>` (channel change), `dump` (routing/neighbor/RSSI/LQI), Touchlink.
- Radio counters per device (LQI/RSSI/retries) в `chips`, frequent-flapper detection.

**Matter**
- Тестирование Dimmable/CT/XY/Thermostat/Sensors с реальным контроллером (Apple Home) на реальных устройствах.
- NVS-restore cluster set: EEM/EPM добавляются только при fresh create (`g_pending_has_metering/energy`), после restore endpoint теряет метеринг — нужно пробросить флаги через NVS или воссоздать из DECLARE.
- Multi-fabric (Apple+Google+Alexa одновременно).
- Matter OTA самого C6#2.

**Brain**
- Rich rules: OR, несколько доп. условий, несколько действий на правило.
- `after_deadline: apply | drop | ask` на overdue таймерах после ребута.
- Room tag, favorite flag, icon на сущностях.
- Console v2: tab completion по именам сущностей и id правил, `watch` (STATE одного entity до Ctrl-C), `log <tag> on|off` (per-component), историй команд в NVS.
- Multi-bus тесты `ChipRouter`, rule-тесты с инъекцией sunset, console-command тесты.

**Железо**
- EN+BOOT линии со S3 на каждый C6 через MOSFET: даст recovery бэкдор (sealed box без расковыривания) и позволит реализовать brain-side flasher через ROM-bootloader.
- Многофактор: нужно спроектировать до пайки «финальной» ревизии.

**Приложение**
- Android-билд (Kotlin-модуль написан, никогда не компилировался).
- Экран локации: кнопка «взять с телефона» ([`src/native/position.ts`](../app/src/native/position.ts) реализована, на устройстве не прогонялось).
- Remote access: сейчас только LAN. Либо relay у нас, либо «удалённо = Apple Home».

**Безопасность**
- Flash encryption для TLS-ключа и Wi-Fi пароля (сейчас в NVS в открытом виде).
- Per-user identity / ACLs поверх общего токена ([brain-backlog.md §9](brain-backlog.md)).
- Подпись OTA-образов (сейчас неподписанные, открытый OTA-endpoint = любой в LAN может перепрошить).

### 3.3 Later — к обсуждению

**Нотификации и внешний мир**
- Telegram / MQTT / HA webhook на интересные события.
- Статусный LED на WS2812 GPIO21 (зелёный/жёлтый/красный).

**Платформенное**
- Миграция brain на ESP32-P4 → Linux SBC (SPEC §3 invariant 3 — ядро готово).
- ESP-CAM recorder как Phase 7 sink (мотион → события), либо вместо него LittleFS на brain.

**Zigbee**
- Zigbee Green Power (ZGP, cluster 0x0021, endpoint 242) — batteryless кнопки, свой фрейминг.
- Manufacturer-specific clusters (`0xfc7c`/`0xfc85` на IKEA).

**Разгрузка на C6-coprocessors** ([backlog.md § Use idle memory](backlog.md))
- Локальный event-buffer на C6 (replay на link-resume).
- Device-side attribute cache, debounce, unit conversion.
- Local rule fast-path для чисто-Zigbee правил.
- NB: ядро должно рассуждать о доме и когда coprocessor молчит — что переносим «вниз», должно быть cache-visible или reproducible, не authoritative.

**Rule engine**
- DSL (Berry/Lua) — когда упрёмся в табличные правила. Пока не упёрлись.

**Матерная сертификация**
- DAC/PAI/CD, CSA membership ($7000/год), factory provisioning — out of scope, пока это не продукт.

**Thread Border Router**
- Сейчас только Matter-over-Thread accessory; реальный BR с IPv6-роутингом в LAN — отдельная история (`esp-thread-br`).

**Протокол**
- Shared bus вместо point-to-point UART (SPEC §3 invariant 2 держит эту дверь открытой: `chip_id` в хедере).
- Compression delta-encoding на STATE-бурстах — только когда увидим шторма.

---

## 4. Что проверять перед релизом

Короткая checklist для «можно ли это кому-то дать в руки».

- [ ] Full iOS-билд на реальном iPhone (N2) — three pinning checks из [ios-build.md](ios-build.md).
- [ ] Setup через AP на телефоне (N3): без консоли человек поставить хаб не может.
- [ ] Apple Home bridge карточка (N1) + полный флоу «купил коробку → поставил в Apple Home».
- [ ] Unpair с живого устройства (N4) без ghost-а в Apple Home.
- [ ] Прогон не-plug устройств по Zigbee (IAS Zone, лампа, термостат).
- [ ] Flash encryption для TLS-ключа и Wi-Fi пароля.
- [ ] Подпись OTA-образов.
- [ ] EN/BOOT линии: без них sealed-коробка небезопасна (нет recovery).
- [ ] Persistent event log sink: без него каждый ребут съедает историю.
