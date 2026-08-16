# Keystone CLI — спецификация

**Статус:** v0.1
**Родительские документы:**
- [`smart-home-engine-brief.md`](../../../claude/smart-home-engine-brief.md) — продуктовая рамка.
- [`go-core-spec.md`](../../../claude/go-core-spec.md) — устройство ядра.
- [`plugin-store-architecture.md`](./plugin-store-architecture.md) — модель плагинов.

## 1. Мотивация

Три пользователя CLI, по возрастанию требований:

1. **Пользователь-энтузиаст.** Ставит keystone на Pi, апдейтит, ставит плагины, изредка чинит что-то. Ожидает опыт уровня `brew` / `apt`.
2. **Разработчик плагинов.** Инициализирует плагины из шаблона, тестит локально, публикует в registry. Ожидает опыт уровня `npm` / `cargo`.
3. **Агент (LLM, скрипт).** Программно управляет домом: считывает состояния, дёргает действия, редактирует правила. Ожидает опыт уровня AWS/gcloud CLI — предсказуемая структура, JSON-выхлоп, ясные exit codes.

Одна CLI должна закрывать все три случая. Дизайн-принципы:

- **Полное покрытие HTTP API.** Всё, что можно сделать через API, должно быть доступно через CLI. Это делает CLI ideal-точкой входа для агентов: не надо парсить HTTP endpoints, всё в одной команде.
- **JSON везде.** Каждая команда поддерживает `--output=json` (`-o json`). По умолчанию — human-friendly, но JSON — стабильный контракт.
- **Стабильные exit codes.** 0 = ok, 1 = generic error, 2 = validation, 3 = not-found, 4 = auth, 5 = timeout, 6 = plugin-error, 7 = conflict. Агент по коду понимает, что делать.
- **Streaming через stdout.** Long-running команды (`stream`, `logs`, `watch`) пишут NDJSON в stdout, каждое сообщение — валидный JSON на строке. Composable с `jq`, `grep`, etc.
- **Idempotent где возможно.** `keystone plugin install matter` дважды подряд — no-op, а не ошибка.

## 2. Общая структура

```
keystone [global-flags] <group> <verb> [args] [flags]
```

Group-первый (`git`-стиль), но с shortcut'ами верхнего уровня для самых частых операций (`brew`-стиль). Полный список групп:

| Группа | Что делает |
|---|---|
| `plugin` | Установка/управление плагинами |
| `device` | Устройства: list/get/state/action/rename/… |
| `rule` | Автоматизации |
| `room` | Комнаты / группировка |
| `scene` | Сцены |
| `system` | Статус, логи, backup |
| `config` | Настройки ядра |
| `store` | Магазин плагинов (browse/search/tap) |
| `secret` | Секреты (API-keys, tokens) |
| `stream` | Live-stream событий |
| `self` | Обновление самого бинаря keystone |

Shortcut'ы на верхнем уровне:

```
keystone install <plugin>       → keystone plugin install
keystone uninstall <plugin>     → keystone plugin uninstall
keystone update [target]        → keystone plugin update (или self если 'self')
keystone search <query>         → keystone store search
keystone status                 → keystone system status
```

## 3. Глобальные флаги

```
--host <url>            keystone HTTP endpoint (default: http://localhost:7777, env KEYSTONE_HOST)
--token <token>         аутентификация (env KEYSTONE_TOKEN)
--output, -o <fmt>      human | json | yaml (default: human, для non-tty stdout — json)
--verbose, -v           подробный вывод
--quiet, -q             только ошибки
--no-color              выключить ANSI
--config <path>         альтернативный ~/.keystone/config.yaml
```

Автодетект: если stdout не tty, дефолт `--output=json`. Агенты и pipe'ы получают JSON автоматически, интерактивные пользователи — human-form.

## 4. Референс команд

### 4.1 `plugin` — управление плагинами

```
keystone plugin list [--enabled|--disabled|--all]
keystone plugin info <name>
keystone plugin install <name>[@version] [--from <source>]
keystone plugin uninstall <name>
keystone plugin update [name]                 # без name — все
keystone plugin enable <name>
keystone plugin disable <name>
keystone plugin restart <name>
keystone plugin logs <name> [--tail] [--since=1h] [--follow]
keystone plugin config <name> get [key]
keystone plugin config <name> set <key> <value>
keystone plugin config <name> unset <key>
keystone plugin health <name>
keystone plugin search <query>                # alias для `store search`
keystone plugin new <name> [--template=go]    # DEV
keystone plugin build [path]                  # DEV
keystone plugin publish [path]                # DEV
keystone plugin validate [path]               # DEV — проверить манифест
```

Примеры:

```
$ keystone plugin list
NAME          VERSION   STATUS    TRUST       DEVICES
matter        1.2.0     running   core        3
ha-bridge     0.4.1     running   core        12
dirigera      0.9.0     disabled  verified    -

$ keystone plugin install matter@1.2.0
Installing matter@1.2.0 from official registry...
Verifying signature... ok
Downloading (18.3 MB)... done
Installing sidecars (matter-server: 145 MB)... done
Enabling... ok
Plugin `matter` running at http://localhost:7777/plugins/matter

$ keystone plugin install matter -o json
{"name":"matter","version":"1.2.0","status":"running","installed_at":"2026-08-15T21:04:11Z"}
```

### 4.2 `device` — устройства

```
keystone device list [--room=<room>] [--type=<type>] [--transport=<transport>]
keystone device get <id>
keystone device state <id> [feature[.key]]     # текущее состояние
keystone device set <id> <feature>.<key> <value>
keystone device do <id> <feature>.<action> [--param key=val ...]
keystone device rename <id> <name>
keystone device move <id> --room <room>
keystone device commission --transport=<t> [--code=<code>] [--name=<name>] [--type=<type>]
keystone device decommission <id>
keystone device watch <id>                     # NDJSON stream состояния
keystone device history <id> [--since=1h] [--feature=onoff]
keystone device sync                           # rescan адаптеров
```

Примеры:

```
$ keystone device list
ID                    NAME             TYPE    TRANSPORT   ROOM       STATE
fc9b357f-...          Kitchen ceiling  light   matter      Kitchen    on · 65% · 2700K
ab12cd34-...          Motion · hall    motion  matter      Hallway    occupied 3m ago

$ keystone device state fc9b357f-9fef7532641c1375
onoff.value: true
brightness.level: 65
color_temp.kelvin: 2700

$ keystone device state fc9b357f-9fef7532641c1375 -o json
{"onoff":{"value":true,"updated_at":"..."},"brightness":{"level":65,"updated_at":"..."}}

$ keystone device do fc9b357f-... onoff.toggle
$ keystone device set fc9b357f-... brightness.level 30
$ keystone device do fc9b357f-... color_temp.set --param kelvin=4500

$ keystone device commission --transport=matter --code=1234-5678-901 --name="Living lamp" --type=light
Starting commission via matter...
[discovering] поиск устройства
[pairing] устанавливаем связь
[verifying] проверяем возможности
[done] commissioned as 3f2e-... (WARMBLIXT)

$ keystone device watch fc9b357f-... -o json
{"ts":"2026-08-15T21:04:11Z","feature":"brightness","key":"level","value":30,"origin":"user"}
{"ts":"2026-08-15T21:04:12Z","feature":"onoff","key":"value","value":false,"origin":"device"}
...
```

### 4.3 `rule` — автоматизации

```
keystone rule list
keystone rule get <id>
keystone rule create --file <rule.yaml> | --stdin
keystone rule edit <id>                        # открывает $EDITOR
keystone rule delete <id>
keystone rule enable <id>
keystone rule disable <id>
keystone rule run <id>                         # forced fire для отладки
keystone rule history <id> [--since=1h]
keystone rule validate --file <rule.yaml>
```

Правила описываются YAML-декларативно (см. `go-core-spec.md`). Пример:

```yaml
# rule.yaml
name: Уютно вечером
when:
  - trigger: time
    at: sunset
    offset: +15m
then:
  - action: set
    device: living-lamp
    feature: brightness
    value: 30
  - action: set
    device: living-lamp
    feature: color_temp
    value: 2200
```

```
$ keystone rule create --file rule.yaml
Created rule d4e5f6... (Уютно вечером)

$ keystone rule list
ID        NAME                STATUS      LAST FIRED
d4e5f6... Уютно вечером       enabled     15m ago
a1b2c3... Утренний свет       disabled    never
```

### 4.4 `room` / `scene`

```
keystone room list
keystone room create <name>
keystone room rename <id> <newname>
keystone room delete <id>
keystone room devices <id>

keystone scene list
keystone scene create <name> --from-current [--rooms=<r1,r2>]
keystone scene apply <id>
keystone scene delete <id>
```

### 4.5 `system` — статус, логи, backup

```
keystone system status                          # health всего: ядро, плагины, устройства
keystone system info                            # версия, uptime, ресурсы
keystone system logs [--since=1h] [--component=matter] [--follow]
keystone system restart [component]             # component = plugin name или 'core'
keystone system backup [--output=<path>]        # snapshot всей БД
keystone system restore <backup-file>
keystone system doctor                          # диагностика: проверить целостность конфига, здоровье плагинов, connectivity
keystone system metrics                         # Prometheus-scrape для внешнего мониторинга
```

Пример `status`:

```
$ keystone system status
● keystone core       0.5.2      up 3d 14h     RAM 34MB / CPU 0.2%
├── matter            1.2.0      running       RAM 148MB     3 devices  online
├── zigbee2mqtt       0.9.1      running       RAM 62MB      7 devices  6 online, 1 offline
├── ha-bridge         0.4.1      running       RAM 287MB     12 devices connected
└── voice-agent       0.1.0      disabled      —

Devices        22 total, 21 online, 1 offline
Rules          8 enabled, 2 disabled
Rooms          5
Last event     2s ago (Kitchen ceiling → brightness=65)
```

### 4.6 `config` — настройки ядра

```
keystone config get [key]
keystone config set <key> <value>
keystone config unset <key>
keystone config edit                            # открыть в $EDITOR
keystone config validate                        # проверить YAML
```

### 4.7 `store` — магазин

```
keystone store browse                           # интерактивная TUI-витрина
keystone store search <query> [--category=<cat>] [--trust=core]
keystone store info <plugin>
keystone store update                           # обновить кэш каталога
keystone store tap <url>                        # добавить custom registry
keystone store untap <url>
keystone store taps                             # список подключённых источников
keystone store categories                       # список категорий
```

Пример:

```
$ keystone store search zigbee
NAME             VERSION   TRUST       DESCRIPTION
zigbee2mqtt      0.9.1     core        Zigbee coordinator via zigbee2mqtt bridge
dirigera         0.9.0     verified    IKEA DIRIGERA hub direct
zigate           0.3.0     experimental ZiGate USB dongle support
```

Внутри `ha-bridge` магазин HA-интеграций:

```
$ keystone plugin config ha-bridge integrations list
$ keystone plugin config ha-bridge integrations install yandex_alice
```

### 4.8 `secret` — секреты

```
keystone secret set <key>                       # интерактивный ввод, не в argv
keystone secret unset <key>
keystone secret list                            # только имена, не значения
keystone secret rotate <key>
```

Пример:

```
$ keystone secret set matter.fabric-key
Enter value (input hidden):
Confirm:
Stored as matter.fabric-key (encrypted, ~/.keystone/secrets.age)
```

### 4.9 `stream` — live-stream событий

```
keystone stream [--filter=<expr>] [--devices=<ids>] [--features=<f1,f2>] [--follow]
```

Всегда выдаёт NDJSON. Дефолтный формат:

```json
{"ts":"...","type":"state","device":"fc9b...","feature":"onoff","key":"value","value":true,"origin":"user"}
{"ts":"...","type":"event","device":"ab12...","event":"motion","data":{}}
{"ts":"...","type":"rule","rule":"d4e5...","stage":"triggered"}
{"ts":"...","type":"plugin","plugin":"matter","health":"ok"}
```

Композиция с `jq`:

```
$ keystone stream --filter='type=="state" and feature=="brightness"' | jq -r '"\(.ts) \(.device) → \(.value)%"'
2026-08-15T21:04:11Z fc9b357f-... → 65%
2026-08-15T21:04:12Z fc9b357f-... → 30%
```

### 4.10 `self` — обновление ядра

```
keystone self update                            # keystone core: pull latest release
keystone self version                           # версия ядра + плагинов
keystone self uninstall                         # полное удаление (с warning)
keystone self diagnostics [--output=<path>]     # zip'нутый bundle для баг-репортов
```

## 5. Взаимодействие с HTTP API

Каждая CLI-команда — тонкий wrapper над одним или несколькими HTTP-endpoint'ами:

| CLI | HTTP |
|---|---|
| `keystone device list` | `GET /devices` |
| `keystone device get <id>` | `GET /devices/{id}` |
| `keystone device do <id> onoff.turn_on` | `POST /devices/{id}/actions` `{feature:"onoff",action:"turn_on"}` |
| `keystone device set <id> brightness.level 30` | `POST /devices/{id}/state` `{feature:"brightness",key:"level",value:30}` |
| `keystone rule create --file r.yaml` | `POST /rules` `{...}` |
| `keystone stream` | `GET /stream` (NDJSON) |
| `keystone plugin install matter` | `POST /plugins/install` `{name:"matter"}` (NDJSON progress) |
| `keystone plugin logs matter --follow` | `GET /plugins/{name}/logs?follow=true` (NDJSON) |

Правило: **CLI не имеет логики, недоступной через API**. Это гарантирует что агент может выбрать любую точку входа, и они дают одинаковый результат.

Исключения — DEV-команды (`plugin new/build/publish`), они работают с локальной файловой системой и не имеют API-аналога.

## 6. Опыт для агентов

Ключевой use-case: LLM-агент управляет домом.

### 6.1 Discovery — что вообще есть

```bash
$ keystone device list -o json
[{"id":"fc9b...","name":"Kitchen","type":"light","features":[...],"state":{...}}, ...]

$ keystone device get fc9b357f-... -o json
{"id":"fc9b...","name":"Kitchen ceiling","type":"light","room":"Kitchen","features":[{"key":"onoff",...},{"key":"brightness",...}],"state":{...}}
```

По этому выхлопу агент понимает всю структуру дома: список устройств, что они умеют, где стоят, какое состояние.

### 6.2 Действие

```bash
$ keystone device do fc9b357f-... onoff.turn_on -o json
{"ok":true,"device":"fc9b...","feature":"onoff","action":"turn_on","completed_at":"..."}
```

Успех — код возврата 0 + JSON `{ok:true,...}`. Ошибка — non-zero exit + JSON `{ok:false,error:{code,message,retryable}}`. Агент по exit-code + `retryable` решает делать retry.

### 6.3 Наблюдение

```bash
$ keystone stream --devices=fc9b... --features=onoff,brightness -o json
{"ts":"...","type":"state","device":"fc9b...","feature":"onoff","value":true}
...
```

Агент подписывается на нужный срез, реагирует на изменения.

### 6.4 Правила

```bash
$ cat <<EOF | keystone rule create --stdin
name: Утренний свет
when:
  - trigger: time
    at: "07:30"
    days: [mon,tue,wed,thu,fri]
then:
  - action: set
    device: bedroom-lamp
    feature: brightness
    value: 40
EOF
```

Агент может создавать правила динамически.

### 6.5 Гарантии для агентов

- **Идемпотентность.** Повторный вызов `device do onoff.turn_on` — no-op если уже включено. Успешный exit.
- **Атомарность.** Одна команда = одна операция. Никаких скрытых side-effects.
- **Полнота state.** `device get -o json` включает всё известное состояние + метаданные — агенту не нужно дозапрашивать.
- **Timestamped.** Все ответы содержат `ts` — агент может строить временную модель.
- **Structured errors.** `{code:"device.not_found",message:"...",retryable:false}` — machine-readable, не regex по тексту.

## 7. Interactive vs. non-interactive

Некоторые команды имеют интерактивный fallback:

- `keystone device commission` без `--transport` — задаст вопросы TUI'ем
- `keystone rule create` без `--file` — откроет $EDITOR с шаблоном
- `keystone store browse` — TUI-браузер плагинов

Правило: если stdin — не tty, интерактивный режим отключается, команда требует полного набора флагов, иначе fail с валидационной ошибкой.

## 8. Autocomplete & shell integration

```
keystone completion bash > /etc/bash_completion.d/keystone
keystone completion zsh  > ~/.zsh/completions/_keystone
keystone completion fish > ~/.config/fish/completions/keystone.fish
```

Автодополнение подтягивает список плагинов, устройств, комнат, правил с локально запущенного ядра. Работает офлайн (кэш) с warning при устаревании.

## 9. Расширяемость через плагины

Плагины могут добавлять свои CLI-подкоманды через манифест:

```yaml
# в plugin.yaml
cli:
  commands:
    - name: fabric
      description: "Управление Matter fabric"
      exec: go/plugin-matter cli fabric $@
```

Тогда:

```
$ keystone matter fabric list        # прокидывается в плагин
$ keystone matter fabric reset
```

Позволяет плагинам иметь rich CLI без изменения ядра. Namespace плагина = его имя.

## 10. Distribution

CLI поставляется в двух формах:

- **Bundled с keystone core** — единый бинарь `keystone`, содержит и сервер и клиент. При `keystone daemon start` запускается ядро; остальные команды коммуницируют через HTTP.
- **Standalone client** — `keystone-cli` (тот же бинарь под другим именем), без ядра. Для удалённого управления домашним Pi с ноутбука.

Установка:

```
# Официальный tap (macOS, Linux)
$ brew install keystone/tap/keystone

# Один-shot install
$ curl -fsSL https://keystone.io/install.sh | sh

# Debian package
$ apt install keystone

# Ручная сборка
$ go install github.com/keystone/keystone/cmd/keystone@latest
```

## 11. Дорожная карта

- **Phase 1** (~1 неделя после plugin-manager MVP): базовая CLI. `plugin *`, `device *`, `system status`, `self version`, `stream`. Реализация — прямой wrapper над существующим HTTP-API.
- **Phase 2** (~1 неделя): полный набор команд по спеке. Interactive fallback'и, autocomplete, output=json на всех командах.
- **Phase 3** (~1 неделя): DEV-команды (`plugin new/build/publish`), интеграция с registry (Phase D plugin-store).
- **Phase 4** (~ongoing): плагин-command-extensions (§9), refined UX по фидбеку.

## 12. Открытые вопросы

- **Distribution**: делаем ли Homebrew tap с самого начала, или CLI-только `curl | sh` до v1.0?
- **Auth model**: локально — сокет с UID-проверкой (без токена). Remote — long-lived token или OAuth device-flow?
- **Command versioning**: как разбираемся, если пользователь на CLI v0.9 говорит с ядром v1.1?
- **Rich rule DSL**: YAML сейчас, но long-term может понадобиться свой mini-language ("если через 5 минут после motion никого нет — выключить"). CLI шов должен пережить этот переход.
- **Plugin CLI extensions namespace**: `keystone <plugin-name> <cmd>` (§9) — но что если plugin-name совпадает с встроенной группой? Резервируем имена?
