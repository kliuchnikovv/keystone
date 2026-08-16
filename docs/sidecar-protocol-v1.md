# Sidecar Protocol v1

**Статус:** v0.1
**Родительские документы:**
- [`plugin-store-architecture.md`](./plugin-store-architecture.md) — модель плагинов.
- [`go-core-spec.md`](../../../claude/go-core-spec.md) — устройство ядра.

## 1. Цель

Единый транспортный контракт между keystone-core и любым плагином. Заменяет ad-hoc WS JSON-RPC, который сейчас использует `matter-server`. Один протокол — все плагины (Matter, Zigbee, DIRIGERA, HA-Bridge, voice-agent, ML) говорят на нём.

## 2. Свойства

- **Bidirectional streaming**: core может звать методы плагина, плагин может пушить события в core.
- **Language-agnostic**: JSON поверх Unix socket, реализуется на любом языке за час.
- **Debuggable**: `socat UNIX-CONNECT:/run/keystone/plugin.sock -` — просмотр трафика в реальном времени.
- **Versioned**: handshake с protocol version negotiation. Плагины и core живут независимо в мажорах.
- **Resilient**: seq-numbers + replay на переподключении, backpressure через TCP-окно, heartbeat.
- **Typed errors**: structured error codes с флагом `retryable`.
- **JSON Schema валидация**: каждый method/topic имеет JSON Schema, доступную обеим сторонам.

## 3. Транспорт

- **Socket:** Unix domain socket, путь передаётся плагину через env var `$KEYSTONE_PLUGIN_SOCKET`.
- **Framing:** newline-delimited JSON (NDJSON). Каждое сообщение — один валидный JSON-объект на строке, разделитель `\n`.
- **Encoding:** UTF-8.
- **Direction:** full-duplex — обе стороны могут инициировать сообщения в любой момент.

## 4. Message shape

Три категории сообщений: **request**, **response**, **push**.

### 4.1 Request

Инициирует одна сторона, ожидает ответа от другой.

```json
{
  "type": "request",
  "id": "01HKQZ...",
  "method": "readState",
  "params": {
    "deviceRef": "matter:3",
    "feature": "onoff",
    "key": "value"
  }
}
```

- `id` — ULID или любой уникальный string. Используется для сопоставления response.
- `method` — имя вызова (см. §7).
- `params` — payload, произвольный JSON, определяется методом.

### 4.2 Response

Одна на request, идентифицируется тем же `id`.

Успех:
```json
{
  "type": "response",
  "id": "01HKQZ...",
  "result": true
}
```

Ошибка:
```json
{
  "type": "response",
  "id": "01HKQZ...",
  "error": {
    "code": "device.not_found",
    "message": "Node 3 not found in fabric",
    "retryable": false,
    "details": { "nodeId": "3" }
  }
}
```

Ровно одно из `result` / `error` присутствует.

### 4.3 Push

Асинхронное сообщение от одной стороны к другой без ожидания ответа.

```json
{
  "type": "push",
  "topic": "state.matter:3.onoff.value",
  "seq": 12847,
  "ts": "2026-08-15T21:04:11.123Z",
  "payload": {
    "value": true,
    "origin": "device_report"
  }
}
```

- `topic` — иерархическое имя (dot-separated). См. §8.
- `seq` — монотонно растущий uint64 на стороне отправителя. Начинается с 1, растёт на 1 на каждый push.
- `ts` — RFC3339, момент возникновения события.
- `payload` — произвольный JSON, определяется topic.

## 5. Handshake

После connect обе стороны обязаны обменяться hello до первого method-вызова.

### 5.1 Sequence

```
plugin → core:  { "type": "request", "id": "...", "method": "hello", "params": {
    "protocol": "v1",
    "plugin":   "matter",
    "version":  "1.2.0",
    "capabilities": ["realtime.state-stream", "commission.setup-code", "commission.qr-code"]
}}

core → plugin:  { "type": "response", "id": "...", "result": {
    "protocol": "v1",
    "core":     "0.5.2",
    "session":  "sess_...",
    "features": ["seq.replay", "topic.filtering"]
}}
```

### 5.2 Version compatibility

- `protocol` — major protocol version. `v1` в этом документе. Несовместимые изменения — новый major (`v2`).
- В рамках одного major допустимы только backward-compatible изменения (новые методы, новые опциональные поля).
- `plugin.version` / `core.version` — SemVer, только для логов и user-facing UI.

Если core не поддерживает protocol плагина — возвращает `error.code: "protocol.unsupported"` в hello-response и разрывает соединение.

## 6. Session lifecycle

```
connect → hello handshake → active → { command/push traffic } → close
                                    ↓
                             disconnect / crash
                                    ↓
                             reconnect → hello + resume (§10)
```

### 6.1 Heartbeat

- Core шлёт `{ "type": "request", "method": "ping" }` каждые **10 секунд**.
- Плагин обязан ответить `{ "result": { "pong": true } }` в течение **30 секунд**.
- Отсутствие ответа → core закрывает socket и запускает restart-политику плагина.

### 6.2 Graceful shutdown

Core перед остановкой плагина шлёт `shutdown` request с параметром `deadline: <ISO-timestamp>`. Плагин обязан завершить in-flight работы и закрыть socket до deadline. По истечению deadline — SIGTERM, потом SIGKILL.

## 7. Method inventory (core → plugin)

Все методы адаптера, которые core может вызвать.

### 7.1 Lifecycle

| Method | Params | Result | Описание |
|---|---|---|---|
| `hello` | HelloRequest | HelloResponse | Handshake, обязателен первым |
| `ping` | `{}` | `{ pong: true }` | Heartbeat |
| `shutdown` | `{ deadline: ISOTime }` | `{}` | Graceful shutdown request |
| `getHealth` | `{}` | HealthStatus | Диагностика |

### 7.2 Device operations

| Method | Params | Result | Описание |
|---|---|---|---|
| `discover` | DiscoverRequest | DiscoveredDevice[] | Список известных устройств |
| `commission` | CommissionRequest | CommissionResult | Добавить новое устройство |
| `decommission` | `{ deviceRef }` | `{}` | Убрать устройство |
| `readState` | AttrRef | StateValue | Прочитать атрибут |
| `writeState` | AttrRef + value | `{}` | Записать атрибут |
| `invokeAction` | ActionRequest | `{}` или action-result | Выполнить command |

### 7.3 Subscriptions

| Method | Params | Result | Описание |
|---|---|---|---|
| `subscribe` | SubscribeRequest | `{ subscribed: true }` | Подписаться на push'и по фильтру |
| `unsubscribe` | `{}` | `{ unsubscribed: true }` | Отписаться от всех push'ей |
| `resume` | `{ since_seq }` | `{ resumed: true, sending: N }` | Замена subscribe при reconnect с replay |

### 7.4 Config flow

| Method | Params | Result | Описание |
|---|---|---|---|
| `configFlow` | `{ step, data }` | ConfigFlowStep | Шаг мастера настройки (см. `plugin-ui-integration.md` §Layer 2) |
| `getConfig` | `{}` | ConfigObject | Текущая конфигурация плагина |
| `setConfig` | ConfigObject | `{}` | Применить новую конфигурацию (может триггерить reload) |

## 8. Push topics (plugin → core)

Иерархические имена, разделитель `.`. Плагин пушит, core фильтрует.

### 8.1 Формат

```
<category>.<scope>.<detail>[.<subdetail>]
```

- `state.<deviceRef>.<feature>.<stateKey>` — атрибут изменился
- `event.<deviceRef>.<eventName>` — событие сработало (motion detected, button pressed)
- `lifecycle.<deviceRef>.online` — устройство появилось
- `lifecycle.<deviceRef>.offline` — устройство пропало
- `lifecycle.<deviceRef>.removed` — устройство снято с fabric'а по инициативе peer'а
- `commissioning.<sessionId>.progress` — прогресс commissioning'а (см. `plugin-ui-integration.md` §Layer 2)
- `plugin.health.<component>` — плагин рапортует своё состояние
- `plugin.log.<level>` — structured log entry
- `plugin.metrics` — периодическая метрика

### 8.2 Wildcard subscriptions

Core подписывается через `subscribe`:

```json
{
  "method": "subscribe",
  "params": {
    "topics": [
      "state.**",              // все state-изменения всех устройств
      "event.matter:3.*",      // все события конкретного устройства
      "lifecycle.**",
      "plugin.log.error"       // только error-логи
    ]
  }
}
```

- `*` — один сегмент
- `**` — любое количество сегментов (только на конце)
- буквальное совпадение иначе

Плагин отбрасывает push'и, чьи topics не матчат подписки. Экономит IPC-трафик.

### 8.3 Payload schemas

**state.\*** payload:
```json
{
  "value": <any>,
  "unit": "celsius|percent|watts|...",
  "origin": "device_report|user_write|adapter_derived",
  "confidence": 1.0    // optional
}
```

**event.\*** payload:
```json
{
  "data": { <event-specific> }
}
```

**lifecycle.\*** payload:
```json
{
  "deviceRef": "matter:3",
  "reason": "session_active|session_timeout|removed_by_peer|..." // optional
}
```

**plugin.health.\*** payload:
```json
{
  "status": "ok|degraded|failing",
  "message": "human-readable detail",
  "counters": { "peers": 3, "sessions_open": 3, ... }
}
```

Полные JSON Schemas — в `keystone/protocol` репозитории.

## 9. Error taxonomy

Все errors имеют форму:
```json
{
  "code": "namespace.identifier",
  "message": "Human-readable text",
  "retryable": true|false,
  "details": { ... }
}
```

### 9.1 Стандартные namespaces

| Namespace | Значение | Retryable |
|---|---|---|
| `protocol.*` | Ошибки протокола (unsupported, malformed) | false |
| `device.not_found` | Устройство не найдено | false |
| `device.offline` | Устройство недоступно сейчас | true |
| `device.unsupported` | Устройство не поддерживает эту операцию | false |
| `feature.unsupported` | Feature/action не поддерживается | false |
| `plugin.not_ready` | Плагин ещё не инициализировался | true |
| `plugin.busy` | Плагин занят (rate limit, in-progress operation) | true |
| `plugin.internal` | Внутренняя ошибка плагина | true |
| `auth.invalid_credentials` | Учётные данные вендора неверны | false |
| `auth.expired` | Token нужно обновить | true |
| `network.timeout` | Timeout на upstream (cloud API, hub, ...) | true |
| `network.unavailable` | Нет соединения с upstream | true |
| `permission.denied` | Действие не разрешено (secretstore, filesystem) | false |
| `validation.bad_params` | Некорректные параметры вызова | false |

### 9.2 Обработка

Core решает retry-политику по `retryable`:
- `retryable: true` → exponential backoff, до maxAttempts из manifest
- `retryable: false` → surface user'у, retry запрещён

## 10. Reconnect и replay

При обрыве TCP-соединения плагин **не теряет буфер push'ей**. При reconnect'е:

1. Handshake `hello` заново
2. Плагин отсылает `subscribe` с теми же topics что раньше (или persistent-subscription hint'ом hello — TBD)
3. Core высылает `resume` со своим `last_received_seq`:
   ```json
   { "method": "resume", "params": { "since_seq": 12847 } }
   ```
4. Плагин ре-пушит все события с `seq > 12847` из своего ring buffer
5. Отвечает `{ resumed: true, sending: N }` — сколько backlog'ов будет отправлено
6. После drain'а buffer'а — переход к live-режиму

### 10.1 Ring buffer size

Плагин обязан держать последние **N = 1000** push-сообщений в памяти. При переполнении — старые дропаются, seq растёт монотонно (так что core видит gap — можно решить, что делать: fetch state заново или accept loss).

### 10.2 Gap detection

Если core на receive'е видит `seq > expected_next_seq` — часть push'ей потеряна (плагин слишком быстро перезаписал buffer или seq-drift). Core логирует warning и опционально запускает full re-sync (`discover` + `readState` всех устройств).

## 11. Backpressure

- Push'и — плагин пишет в socket, TCP-окно естественно тормозит если core не читает.
- Плагин может опционально drop non-critical push'и (например, повторные state-обновления одного и того же attribute) — политика на стороне плагина.
- `plugin.health.status` рапортует `degraded` если drop-rate > threshold.

## 12. Полный пример сессии

```
[connect]

plugin → core: {"type":"request","id":"1","method":"hello","params":{"protocol":"v1","plugin":"matter","version":"1.2.0","capabilities":["realtime.state-stream","commission.setup-code"]}}

core → plugin: {"type":"response","id":"1","result":{"protocol":"v1","core":"0.5.2","session":"sess_abc","features":["seq.replay","topic.filtering"]}}

core → plugin: {"type":"request","id":"2","method":"subscribe","params":{"topics":["state.**","lifecycle.**","event.**"]}}

plugin → core: {"type":"response","id":"2","result":{"subscribed":true}}

core → plugin: {"type":"request","id":"3","method":"discover","params":{}}

plugin → core: {"type":"response","id":"3","result":[{"deviceRef":"matter:2","type":"light","name":"Kitchen","features":[...]}]}

plugin → core: {"type":"push","topic":"state.matter:2.onoff.value","seq":1,"ts":"2026-08-15T21:04:11.123Z","payload":{"value":true,"origin":"device_report"}}

core → plugin: {"type":"request","id":"4","method":"invokeAction","params":{"deviceRef":"matter:2","feature":"onoff","action":"turn_off"}}

plugin → core: {"type":"response","id":"4","result":{}}

plugin → core: {"type":"push","topic":"state.matter:2.onoff.value","seq":2,"ts":"2026-08-15T21:04:12.234Z","payload":{"value":false,"origin":"device_report"}}

... [long-running] ...

core → plugin: {"type":"request","id":"heartbeat-1","method":"ping","params":{}}
plugin → core: {"type":"response","id":"heartbeat-1","result":{"pong":true}}

... [tcp drop, reconnect] ...

plugin → core: {"type":"request","id":"5","method":"hello","params":{...}}
core → plugin: {"type":"response","id":"5","result":{...}}
core → plugin: {"type":"request","id":"6","method":"resume","params":{"since_seq":2}}
plugin → core: {"type":"response","id":"6","result":{"resumed":true,"sending":3}}
plugin → core: {"type":"push","topic":"state.matter:2.brightness.level","seq":3,...}
plugin → core: {"type":"push","topic":"state.matter:2.brightness.level","seq":4,...}
plugin → core: {"type":"push","topic":"event.matter:5.motion","seq":5,...}
```

## 13. Схемы

Полные JSON Schemas для всех Request/Response/Push payload'ов публикуются в `github.com/keystone/protocol/schemas/v1/`. Клиентские SDK (`sdk-go`, `sdk-node`, `sdk-python`) генерируются из них.

Валидация опциональна в production (для перф.), обязательна в CI (для регрессий).

## 14. Открытые вопросы

- **Auth между core и плагином.** Сейчас — Unix socket с UID-проверкой, доступ имеет только пользователь keystone. Достаточно ли для v1? Если пойдём в multi-user — нужен per-plugin token.
- **Streaming binary data.** Плагины типа camera-bridge могут захотеть пушить thumbnail'ы. JSON base64 — плохо. Альтернатива: side-channel Unix socket или отдельный tcp-стрим на HTTP endpoint ядра. TBD.
- **Bulk operations.** Сейчас каждый readState — отдельный round-trip. Если Rules engine хочет прочитать 20 attributes одного устройства — 20 request'ов. Ввести `readStateBatch`?
- **Priority queue для push'ей.** Важные события (lifecycle.offline, motion) vs. фоновые (energy meter каждую секунду) — стоит ли разделять каналы? Пока полагаемся на топики + core-side prioritization.
- **Persistent subscription hint в hello.** Плагин может сохранять последний subscribe filter и авто-применять после reconnect — избежать re-subscribe round-trip'а.
- **Schema evolution в рамках v1.** Как добавлять optional fields? Стратегия: только additive, обе стороны игнорят unknown fields (loose parsing).
