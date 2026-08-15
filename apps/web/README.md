# @keystone/web

Keystone Web MVP на React + TypeScript. Скоуп: `Home` + `AddDevice` (skeleton).
Архитектура сделана RN-совместимой — при портировании в `apps/mobile/` меняется только рендер-слой (`<div>` → `<View>`) и стили (CSS Modules → `StyleSheet`).

## Запуск (dev)

```bash
# в корне репо
cd apps/web
yarn install
yarn dev              # vite на :5173, проксирует /devices,/stream,/rules,/discover на :7777
```

В соседнем терминале — бэкенд:

```bash
go run ./cmd/keystone -addr :7777
```

Открой <http://localhost:5173>.

Переопределить бэкенд-хост можно через `VITE_KEYSTONE_HOST` (например, `VITE_KEYSTONE_HOST=http://192.168.0.10:7777 yarn dev`).

## Скрипты

| Скрипт | Что делает |
|---|---|
| `yarn dev` | Vite dev server |
| `yarn build` | Прод-сборка → `dist/` |
| `yarn typecheck` | tsc без эмита |
| `yarn lint` | ESLint |
| `yarn format` | Prettier write |
| `yarn test` / `yarn test:watch` | Vitest |
| `yarn gen:proto` | `buf generate` → `src/proto/gen/*.ts` (ts-proto) |

## Структура

```
src/
├── App.tsx / main.tsx             # роутер, providers
├── screens/                       # HomeScreen, AddDeviceScreen
├── components/                    # DeviceTile, Toggle, BrightnessSlider,
│                                  # ColorTempSlider, Chip, Button, NavBar,
│                                  # SectionHeader, EmptyState, LoadingSkeleton
├── api/                           # client, devices, stream, types
├── state/                         # zustand stores
├── hooks/                         # useDevices, useLiveStream
├── theme/                         # tokens.ts (shared с RN) + theme.module.css
├── lib/                           # format, colorTemp
└── proto/gen/                     # ts-proto artifacts (после yarn gen:proto)
```

## Типы API

Пока `src/api/types.ts` — ручные типы, зеркалящие текущий JSON от Go (CapitalCase-поля, см. `RawDevice` → `toDevice()`).

Когда бэкенд поправит json-теги или мы захотим Connect — `yarn gen:proto` сгенерит `src/proto/gen/*.ts` из `../../proto/keystone/v1/*.proto`. Требуется `protoc` (`brew install protobuf`) или запуск через `buf` (уже включён в devDependencies).

## Что ждём от бэка

- `POST /devices/commission` (NDJSON-стрим прогресса) — для AddDevice.
- `GET /discover?transport=matter` (NDJSON-стрим кандидатов) — для live-списка.
- Camel- или snake_case в JSON (сейчас `ID`/`Name`/... — работаем через маппер).
- CORS для случая отдельного хоста (пока dev через vite proxy — не нужен).

## Live stream

`GET /stream` (NDJSON поверх long-lived HTTP). Reconnect с экспоненциальным backoff `[1s, 3s, 6s, 12s, 30s]`. См. [api/stream.ts](src/api/stream.ts) и [hooks/useLiveStream.ts](src/hooks/useLiveStream.ts).

Референс vanilla-реализации — [site/dashboard.html](../../site/dashboard.html).
