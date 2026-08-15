# Keystone — запуск локально

**Аудитория:** dev-инженеры и все, кто хочет пощупать keystone у себя.
**Что покрывает:** обе платформы (macOS для разработки, Linux/Raspberry Pi для целевого железа), три режима запуска, диагностика типичных проблем.

## 0. TL;DR — какой режим выбрать

| Ты на… | Хочешь потестить… | Рекомендуемый режим |
|---|---|---|
| macOS | UI + виртуальные устройства (без Matter) | **A. Только keystone нативно** |
| macOS | Реальные Matter-устройства (WARMBLIXT и т.п.) | **B. Всё нативно на Mac** |
| Linux / Raspberry Pi | Что угодно | **C. Docker Compose** |

**Почему на macOS не работает C:** Docker Desktop запускает контейнеры в Linux-VM, и mDNS multicast (нужен для Matter discovery) **не пробрасывается** до физической сети, где живёт Thread Border Router. Даже с `network_mode: host` и Docker Desktop 4.34+ host networking — LAN broadcast не доходит. Это принципиальное ограничение среды, не фикс кодом. Native — единственный работающий путь на Mac.

## 1. Требования

Один раз поставить:

- **Go 1.23+** (для сборки keystone бинаря)
- **Node.js 20+** (для matter.js sidecar)
- **Yarn** (для React frontend, `apps/web/`)
- **Docker Desktop 4.34+** (только для режима C или если тестишь Linux-сборку)

Проверка:

```sh
go version   # go1.23.x или выше
node -v      # v20.x.x или выше
yarn -v      # 1.22+ или 4.x+
```

## 2. Режим A — только keystone (быстрое пощупать)

Ничего не commissioning'им, тестируем UI на виртуальных устройствах. Демо-сцена автоматически засеет виртуальный WARMBLIXT + пару розеток.

Один терминал:

```sh
cd ~/go/src/github.com/kliuchnikovv/keystone
./bin/keystone        # или: go run ./cmd/keystone
```

Ожидаемые строки в логе:

```
storage opened          dir=./keystone-data
devices rehydrated      count=0
virtual adapter started
virtual device commissioned  name=Kitchen kettle...
device commissioned          name=Kitchen kettle
...
rules engine started
ui served              dir=./site url=http://:7777/ui/dashboard.html
http listening         addr=:7777
```

Открой в браузере **`http://localhost:7777/ui/dashboard.html`** — vanilla-панель с виртуальными устройствами. Toggle работает, чайник саутутом гоняет ватты каждые 2 сек, правило «>1.5кВт → лампу в спальне» отработает через ~15 сек.

Готово — можно щупать UI.

## 3. Режим B — Matter через ecosystem (macOS)

Реальный сценарий: WARMBLIXT (или любое Matter-устройство) уже в Apple Home / Google Home, ты commissioning'ишь его вторым админом через keystone.

### 3.1 Пред-условия

- **Устройство в Matter-экосистеме**: WARMBLIXT добавлен в Apple Home / Google Home / SmartThings / Alexa обычным способом (QR с коробки).
- **Thread Border Router в сети**: Apple TV 4K / HomePod mini (2nd gen+) / Google Nest Hub 2nd gen. Без него Matter-over-Thread не поедет.
- **Mac подключён к той же Wi-Fi**, где TBR (не на VPN, не через WireGuard, не на гостевой сети).

### 3.2 Запуск

**Терминал 1 — matter sidecar (нативно, важно):**

```sh
cd ~/go/src/github.com/kliuchnikovv/keystone/sidecars/matter-server
npm install               # если ещё не ставил
node dist/index.js        # или: npm start
```

Дождись строки:

```
{"msg":"ws server listening","host":"0.0.0.0","port":5580}
```

В логах ниже должны появиться mDNS-интерфейсы с реальным IP твоей сети:

```
Initialize multicast address: 192.168.1.X:5353 interface: en0
```

где `192.168.1.X` — твой Mac в LAN. **Если видишь `192.168.64.x` или `192.168.65.x` — это Docker-VM, значит sidecar случайно взят из докера.** Останови контейнер.

**Терминал 2 — keystone:**

```sh
cd ~/go/src/github.com/kliuchnikovv/keystone
./bin/keystone -matter-sidecar ws://localhost:5580
```

Ожидаем:

```
matter ws client connected  url=ws://localhost:5580
matter adapter started      sidecar=ws://localhost:5580
```

**Терминал 3 — React MVP (опционально):**

```sh
cd ~/go/src/github.com/kliuchnikovv/keystone/apps/web
yarn install     # если node_modules пустая
yarn dev
```

Дождись `VITE ready · Local: http://localhost:5173/`.

### 3.3 Commissioning WARMBLIXT

1. В **приложении Apple Home** (или Google Home / SmartThings / Alexa):
   - Открой карточку WARMBLIXT.
   - Долгий тап → «Настройки» → «Turn on Pairing Mode».
   - Появится **11-значный код** типа `1234-5678-901`. Скопируй.
2. В **keystone UI** — `http://localhost:5173/add-device` (или `:7777/ui/dashboard.html` → Add):
   - Выбери экосистему.
   - Введи 11-значный код.
   - Имя: `Kitchen light` (или своё).
   - Тип: `light`.
   - Тап «Подключить».
3. Ждать ~30–60 сек. В логах sidecar видно discovery, pairing, verifying стадии.
4. Успех → устройство в списке. Toggle/яркость/цветовая температура должны физически менять WARMBLIXT.

## 4. Режим C — Docker Compose (Linux / Raspberry Pi)

На Linux `network_mode: host` работает нативно без VM-прослойки, поэтому Docker вариант — самый чистый на целевом железе.

**⚠️ На macOS этот путь работает только для non-Matter случаев** (виртуальные устройства + UI). Реальный commissioning в Docker на Mac не пойдёт — см. раздел 0.

### 4.1 Запуск

```sh
cd ~/go/src/github.com/kliuchnikovv/keystone
docker compose up -d
docker compose logs -f keystone matter-sidecar
```

Оба контейнера должны быть Running. Логи должны показать:

- keystone: `matter adapter started` (не «continuing without matter»).
- matter-sidecar: `ws server listening 0.0.0.0:5580`, интерфейсы `eth0` с адресом хоста.

Открой `http://localhost:7777/ui/dashboard.html`.

### 4.2 Остановка

```sh
docker compose down
```

С сохранением volumes (`matter-fabric`, `keystone-data`) — они pesist'ятся между запусками. Если нужен clean state — `docker compose down -v` (удалит volumes).

## 5. Проверка что всё работает

Три быстрых пинга в отдельном терминале:

```sh
# keystone жив
curl -sS http://localhost:7777/health
# → {"status":"ok"}

# список устройств
curl -sS http://localhost:7777/devices | jq '.devices[] | {name, type, transport}'

# что предлагает sidecar (только в режиме B/C, при работающем sidecar)
curl -sS http://localhost:7777/devices/sync -X POST
# → {"added": [...]}
```

## 6. Типичные проблемы

### «localhost refused to connect» на порту :7777 или :5173

Соответствующий процесс не запущен. Проверь:

```sh
lsof -iTCP:7777 -sTCP:LISTEN     # должен быть keystone
lsof -iTCP:5173 -sTCP:LISTEN     # должен быть node (vite)
```

Если пусто — стартуй процесс, см. соответствующий раздел выше.

### «no adapter registered for transport 'matter'»

Устаревший бинарь без retry-логики. Пересобери:

```sh
cd ~/go/src/github.com/kliuchnikovv/keystone
go build -o bin/keystone ./cmd/keystone
```

### `matter sidecar not connected yet — the reconnect loop is retrying in the background`

Sidecar лежит или не завёлся. Проверь:

- В режиме B: активен ли `node dist/index.js`?
- В режиме C: `docker compose logs matter-sidecar` — что происходит?

Retry-loop сам подхватит sidecar как только он взлетит — не нужно перезапускать keystone.

### Sidecar висит на `Initiating discovery of node discovery`

Discovery не может найти устройство. Возможные причины:

- **macOS Docker:** sidecar в контейнере, LAN недостижим. Перейти на нативный запуск (режим B).
- **Не в pair-mode:** окно «Turn on Pairing Mode» истекло (~60 сек), сгенерируй код заново.
- **Другая Wi-Fi сеть:** Mac и Thread Border Router в разных сетях (гостевая VLAN, iPhone-hotspot).
- **IPv6 выключен на роутере:** Thread mandates IPv6. Проверь в настройках роутера.

### Демо-сцена не засеяна (0 виртуальных устройств)

При запуске в докере `-demo=true` может не пробрасываться через Dockerfile CMD. Работает при нативном запуске (режим A/B) без флагов — там default `true`.

### `apps/web/node_modules` пустая → `yarn dev` падает

Просто `yarn install` в `apps/web/`. Первая установка ~30–60 сек.

## 7. Reset и clean state

Полная зачистка данных без переустановки:

```sh
cd ~/go/src/github.com/kliuchnikovv/keystone

# keystone data — устройства, правила, история
rm -rf keystone-data

# matter fabric — сохранённые Matter fabric-ключи (после этого WARMBLIXT
# придётся заново multi-admin'ить)
rm -rf matter-data
rm -rf sidecars/matter-server/data

# Docker volumes (если использовал compose)
docker compose down -v
```

После reset — заново стартуй по разделу 2, 3 или 4.

## 8. Порядок процессов (matrix)

Быстрая справка «что от чего зависит»:

```
Режим A:
  keystone (:7777) → virtual adapter (in-process)

Режим B (macOS Matter):
  matter-sidecar (:5580, node) → Matter-устройства в LAN
              ↑
              WebSocket
              ↓
  keystone (:7777) → virtual + matter adapters
              ↑
              proxy
              ↓
  vite dev (:5173) → React UI

Режим C (Docker Compose):
  matter-sidecar container (:5580 host-net)
              ↑
              WS localhost
              ↓
  keystone container (:7777 host-net)
```

## 9. Порт-таблица

| Порт | Что | Кто слушает |
|---|---|---|
| 5173 | React dev-сервер | `vite dev` в `apps/web/` |
| 5353 | mDNS UDP | matter-sidecar (Matter discovery) |
| 5540 | Matter UDP unicast | matter-sidecar |
| 5580 | JSON-RPC WebSocket | matter-sidecar (общение с keystone) |
| 7777 | keystone HTTP API + `/ui/*` | Go бинарь |
| 7778 | keystone gRPC (опционально) | Go бинарь с `-grpc-addr` |

## 10. Что дальше

Когда MVP-flow с одной лампой пройдёт end-to-end — вернуться к:

- Добавить больше Matter-устройств (multi-admin flow тот же).
- Прогнать rules engine с реальным устройством (например «если motion → включить WARMBLIXT»).
- Vitest тесты фронта.
- systemd юниты для reference-hardware (вместо docker-compose).
