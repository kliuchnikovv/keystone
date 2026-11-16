# Промпт для новой сессии (F2: хаб оборачивает и Zigbee, и Matter)

Репозиторий keystone-esp, ветка `matter/bridge-on-thread-c6`, общаемся на русском.
SPEC.md — источник истины, CLAUDE.md — статус.

> **Разобрано 15.11.2026 (вечер).** Симптом «DECLARE датчика не дошёл» **не
> воспроизводится**: в реестре мозга есть `0x0300 (office Tinfoil) sensor chip3`,
> на сопроцессоре мост держит `endpoint 3 -> entity 0x0300` (таблица 2 из 20).
> Причина, по которой выглядело как «не дошёл»: строка `bridge table: N of 20`
> печатается **при старте моста** (снимок NVS), а «первый DECLARE от незнакомого
> чипа всегда отбрасывается» ⇒ `home`, сделанный сразу после строки «node … →
> N entities», показывал 3 сущности вместо 4. Оба молчаливых бага закрыты:
> чип-псевдо 0x03 **объявляет себя HELLO** перед первым DECLARE, а буфер DECLARE
> в прошивке был **на байт меньше** нужного (`3 + attrs*3` вместо `4 + attrs*3` ⇒
> ровно 8 атрибутов ⇒ DECLARE терялся без единого слова). Подробности:
> [docs/matter-backlog.md](matter-backlog.md) § 10 «The Matter devices are inside
> the hub». Инструмент: `scripts/flash-thread-c6-matter.sh --brain-reset`.

## Цель архитектуры (главное, что нужно помнить)

**Хаб — единая обёртка.** Apple Home (и любой Matter-контроллер) не должен знать,
что физически подключено к хабу: он видит **одно устройство — хаб** (агрегатор), а
внутри него — bridged-эндпоинты: розетку (Zigbee, 0x01xx), датчик TIMMERFLOTTE
(Matter, 0x03xx) и всё, что появится позже. Именно поэтому мозг зеркалит свои
Matter-сущности на сопроцессор: мост хаба строит для них эндпоинты.

Раскладка ролей:
- **мозг** (brain-s3): реестр, правила, сцены, CHIP-контроллер/комиссионер.
  Держит Matter-устройства, которые сам скомиссионил (entity 0x03xx, чип-псевдо 0x03).
- **сопроцессор** (thread-c6, чип 0x02): Thread-сеть хаба + Border Router +
  **Matter-сервер** (root + агрегатор) + **мост** (bridged-эндпоинты).
- BLE у сопроцессора выключен намеренно (−426 КБ флеша), поэтому телефон входит
  в fabric хаба **вторым администратором**.

## Что сделано и проверено на железе (14–15.11.2026)

1. **Разобран coredump паники сопроцессора** (docs/matter-backlog.md § 10, «The
   crash of 14.11.2026»): CPU прыгал по мусорному адресу (`mcause=2`,
   `mtval=0x8484` — сжатая `c.fsd`), т.е. стек задачи `@CHIP` был затерт перед
   вызовом `SimpleSubscriptionResumptionStorage::Init`.
2. **Питание USB на Pi5** — настоящая причина падений: в `dmesg` было
   `hwmon hwmon3: Undervoltage detected!` + парные `usb 1-2 / usb 3-2: USB
   disconnect`. Пользователь добавил `usb_max_current_enable=1` в
   `/boot/firmware/config.txt` и перезагрузил Pi: **0 undervoltage-событий, краш
   `@CHIP` исчез, обе C6 по 0 ребутов за 5 мин.** Чип зависал на просадке, его
   сбрасывал watchdog (`rst:0x8 (TG1_WDT_HPSYS)`) — это выглядело как краш
   прошивки.
3. **Thread-сеть не переформируется при ребуте** (ложная тревога): меняется
   `otThreadGetPartitionId()` (id форк-раздела), а идентичность стабильна
   (`ext pan id f3bc70d87aae6127`, `ml prefix fda60447:3582:7fd3::/64`).
4. **Stage B: мост живёт на сопроцессоре** (`firmware/thread-c6/main/matter_bridge.cpp`,
   порт из `git show b9e8897^:firmware/thread-c6/main/main.cpp`): DECLARE →
   bridged-эндпоинт, STATE → `attribute::update()`, UNDECLARE → удаление, запись
   контроллера → COMMAND мозгу. Образ 0x1bf4b0 (134 КБ в слоте свободно).
5. **Apple Home**: хаб добавился вторым администратором (окно открывает сам
   сопроцессор: `matter_share 300`, код тест-DAC `3497-0112-332`), переключатель
   розетки работает: `chip[ZCL]: Toggle ep2 on/off` → `matter_bridge: COMMAND ->
   entity=0x0100 attr=0x01 value=0` → в журнале мозга `command matter 0x0100
   on_off = off`.
6. **Мозг не моделирует собственный мост** (`firmware/brain-s3/main/matter_bridge.cpp`):
   узел с эндпоинтом-агрегатором (device type 0x000E) → его части не заводим
   (они — зеркала 0x01xx), узел в таблице остаётся; id сущностей выдаются из
   одного пространства (пути «комиссионинг» и «restore» нумеруют слоты по-разному
   ⇒ была коллизия 0x0300 с датчиком), флаг `modelled` — чтобы узел без сущностей
   не читался по кругу.
7. **Инструменты**: `scripts/kcon.py` переживает перезагрузку (переоткрывает порт и
   по обрыву, и по простою) + `--follow-seconds`; `scripts/flash-thread-c6-matter.sh` —
   `--chip-cmd`, `--reboot-log` (ловит лог загрузки, порт ищется по MAC), `--nvs`,
   `--brain`, `--brain-flash`; `scripts/nvs-inspect.py` — разбор NVS-раздела.
   431/431 хост-тестов.

## Что в работе и НЕ доделано (главное для новой сессии)

**Цель:** датчик TIMMERFLOTTE (entity 0x0300, узел Matter 0x1234) должен
появиться **внутри хаба** в Apple Home, как bridged-эндпоинт.

**Сделано в мозге (прошит 15.11, образ 0x1ee0a0 — в слоте 0x1f0000 осталось
8032 Б, впритык):**
- `matter_bridge::set_mirror()` + `publish_mirror()` — DECLARE/STATE Matter-сущностей
  дополнительно уходят **в UART сопроцессора** (`ChipRouter::publish()` → только
  UART, НЕ через fan-out: иначе реестр ядра переприсвоит владельца сущности и
  заберёт её у чипа-псевдо 0x03).
- COMMAND от сопроцессора маршрутизируется **по владельцу сущности** (старший
  байт entity id): 0x01xx → Zigbee, 0x03xx → Matter-псевдо 0x03.
- На HELLO сопроцессора мозг перепубликовывает DECLARE Matter-сущностей
  (`matter_bridge::republish()`), чтобы сопроцессор (он мог подняться позже) их
  увидел.

**Симптом (не решено):**
- Мост мозга моделирует датчика: лог `matter_bridge: node 0x0000000000001234 -> 1
  entities, first 0x0300`; узел хаба распознан верно: `node
  0x0000000000000002 is the hub's own Matter bridge: ... nothing to model`.
- Но **в реестре ядра сущности 0x0300 нет** (`home` показывает 3 сущности: два
  чипа + розетка 0x0100) ⇒ DECLARE от моста до реестра не дошёл.
- И **эндпоинта датчика на сопроцессоре нет**: `bridge table: 1 of 20 slots
  used` (только розетка endpoint 2 → entity 0x0100).
- То есть DECLARE датчика не дошёл **ни** в ядро, **ни** в сопроцессор — хотя
  `publish_declare()` вызывался (лог «1 entities» печатается сразу после этого).

**Главные зацепки (проверить в первую очередь):**
1. `core/radio_coordinator.hpp` (строки ~118–134): **первое** сообщение от
   незнакомого чипа (или с новым boot_id) → `reg_.clear_chip(chip)` + `send_query(chip)`
   + `return` — **сам DECLARE отбрасывается**. Для чипа-псевдо 0x03 это значит:
   первый DECLARE всегда теряется, а восстановление идёт через QUERY →
   `MatterLink::publish()` (Query) → `matter_bridge::republish()`. Проверить, что
   этот путь реально срабатывает (лог `matter_bridge`/`core`).
2. Мост мозга публикует через `g_sink` → `bridge_sink()` → `ChipRouter::inject_inbound()`
   → fan-out. Проверить, что fan-out доходит до координатора для `chip_id = 0x03`.
3. Зеркало: `bridge_mirror()` → `ChipRouter::publish()` → UART2. Проверить, что
   кадр физически уходит (счётчики `tx_frames` сопроцессора / лог UART).
4. `matter_bridge::republish()` — что он реально публикует (все узлы или только
   «моделированные»)? Подозрение: он может не трогать узлы, у которых
   `entity_count == 0`, или id-ы уже заняты.
5. Взглянуть на `core/radio_coordinator.hpp` целиком: там же `clear_chip` по
   chip_id — если у чипа 0x03 меняется boot_id (`kBootId` в мосте), координатор
   будет постоянно стирать его сущности.

**Проверка в железе:** после починки — на сопроцессоре должно появиться
`matter_bridge: bridged: entity 0x0300 (chip 0x03) -> Matter endpoint 3`, а в
`home` мозга — снова 4 сущности. В Apple Home датчик должен появиться **внутри
хаба**.

## Дальше по плану

1. Доделать обёртку Matter-устройств (выше).
2. **Stage C** — setup code/QR для fabric'а хаба: `core/matter_onboarding.hpp`
   уже есть; печать на консоли мозга, потом в приложении.
3. Коммиты — **делает пользователь** (git add/commit/push заблокированы правилами
   доступа агента). Незакоммичены: `firmware/brain-s3/main/{matter_bridge.cpp,matter_bridge.hpp}`,
   `firmware/brain-s3/main/main.cpp`, `scripts/{kcon.py,flash-thread-c6-matter.sh,nvs-inspect.py}`,
   `.gitignore`, `CLAUDE.md`, `docs/matter-backlog.md`, этот файл.

## Грабли (проверено)

- Правила доступа: работают только `bash scripts/flash-thread-c6-matter.sh:*`,
  `scripts/matter-build.sh:*`, `docker run`, `python3 *`, `cmake`, `ctest`.
  `git add/commit/push`, правка `/boot`, `/dev` — **заблокированы**.
- Порты USB меняются при переподключении — искать по MAC: мозг `10:51:DB:5E:F8:94`,
  thread-c6 `AC:EB:E6:DA:FC:08`, zigbee-c6 `AC:EB:E6:DA:D2:C4`.
- Прошивка C6: пользователь жмёт **BOOT+RESET** → `bash
  scripts/flash-thread-c6-matter.sh --force --no-reset` → пользователь жмёт
  **RESET** без BOOT. Программные DTR/RTS на этой плате не срабатывают.
- `scripts/matter-build.sh thread-c6` чистит build (≈20 мин); `… thread-c6 --keep`
  продолжает прерванную сборку. Для мозга — `scripts/matter-build.sh brain-s3`.
- `build/` принадлежит root'у контейнера — из хоста в него не записать, поэтому
  дампы (coredump, NVS) лежат в `firmware/thread-c6/` (в `.gitignore`).
- Слой Matter в слоте образа почти впритык: мозг 0x1ee0a0 из 0x1f0000 (8032 Б),
  сопроцессор 0x1bf4b0 из 0x1e0000 (134 КБ).
- Повторный комиссионинг поверх существующей фабрики заканчивается
  `CHIP_ERROR_FABRIC_EXISTS` (0x7E), а без окна аксессуар на PASE не отвечает
  (только `StandaloneAck`). Порядок: `matter_share 300` на сопроцессоре →
  `matter_pair` на мозге; с нуля — `factoryreset` на сопроцессоре (сеть Thread
  выживает: датасет читается до стирания и кладётся обратно).
