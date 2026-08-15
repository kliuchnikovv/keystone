# Keystone — статус разработки

**Дата обновления:** 2026-08-13
**Как поддерживается:** актуализируется рядом с ключевыми коммитами.
**Что это:** единая точка правды о том, что реально сделано vs что описано в брифах. Брифы описывают план, этот док — реальность.

## 0. TL;DR

За последние двое суток вся сложная часть закрыта:

- **Matter-adapter конца-в-конец работает** — matter.js sidecar на TS, Go-адаптер, docker-compose, HTTP-эндпоинт commissioning. Осталось прогнать WARMBLIXT физически.
- **Frontend v0.2 — sprint 1-3 закрыты.** Mobile-first layout, EcosystemPicker + EcosystemGuide + SetupCodeField, Device Detail. Осталось интеграция с реальным Matter (Sprint 4) и полировка (Sprint 5).
- **Docker deployment готов** — `docker compose up -d` поднимает keystone + matter-sidecar.
- **DIRIGERA откачен**, папка в `_to_delete/`.

Дальше — практика с реальным устройством и тесты.

## 1. Backend — Go core + matter.js sidecar

### Core (Go)

| Компонент | Статус | Заметки |
|---|---|---|
| Doamin model (`internal/domain/`) | ✅ | Device / Feature / State / Event / Rule + polymorphic JSON |
| Registry с keyed locks | ✅ | Per-device serialization by design |
| Event bus | ✅ | Fan-out pub/sub с drop-oldest overflow |
| Rules engine | ✅ | 5 типов триггеров (time/state/threshold/event/state-equals), 3 действия, 2 условия |
| Virtual adapter | ✅ | Включая demo WARMBLIXT с onoff + brightness + color_temp |
| SQLite-alternative persistence | ✅ | JSON-файлы; интерфейс Repository готов под замену на SQLite |
| HTTP admin API | ✅ | Все endpoints ниже |
| WS-like state stream (NDJSON) | ✅ | `/stream` через `internal/api/ws` |
| gRPC API (build tag) | ✅ | `-tags=grpc`, proto контракт зафиксирован, testclient работает |
| Static UI mount | ✅ | `/ui/` из директории на диске |

### Matter adapter (`internal/adapters/matter/`)

| Компонент | Статус | Файл |
|---|---|---|
| Adapter (`ports.Adapter`) | ✅ | `adapter.go` (9.6K) |
| WebSocket JSON-RPC клиент | ✅ | `wsclient.go` (7K) + тесты |
| Protocol types | ✅ | `protocol.go` (4.5K) |
| Matter clusters → Feature mapping | ✅ | `mapping.go` (15.5K) + тесты |
| Config | ✅ | `config.go`, читает `-matter-sidecar ws://...` |
| **Реальный e2e тест с WARMBLIXT** | 🟡 | Нужен hardware pass — код готов |

### Matter sidecar (`sidecars/matter-server/`)

| Компонент | Статус |
|---|---|
| TypeScript project (package.json, tsconfig) | ✅ |
| `src/controller.ts` — обёртка над matter.js 0.17 | ✅ |
| `src/wsServer.ts` — WebSocket JSON-RPC server | ✅ |
| `src/protocol.ts` — типы message'ей | ✅ |
| `src/index.ts` — entrypoint | ✅ |
| `Dockerfile` | ✅ |
| `dist/` — скомпилированный JS | ✅ |
| Dev scripts (`scripts/*.mjs`, `.sh`) — commission, watch-events, toggle | ✅ |

### HTTP endpoints (main.go)

```
GET  /health
GET  /devices                       — список устройств
GET  /devices/{id}                  — одно устройство
POST /devices                       — упомянут через service, HTTP endpoint не выделен
POST /devices/commission            — streaming commissioning с setup-code ✅ NEW
POST /devices/sync                  — manual sync from adapters ✅ NEW
DELETE /devices/{id}                — remove device ✅ NEW
POST /devices/{id}/actions          — invoke action (turn_on/turn_off/set)
POST /devices/{id}/state            — write raw state
GET  /rules                         — CRUD правил
POST /rules
DELETE /rules/{id}
GET  /rules/runs                    — история срабатываний
GET  /stream                        — NDJSON стрим state changes и events
GET  /ui/*                          — static dashboard/landing
```

### Deployment

| Артефакт | Статус |
|---|---|
| `Dockerfile` (keystone) | ✅ |
| `docker-compose.yml` (host network mode) | ✅ |
| macOS arm64 бинарь в `bin/keystone` | ✅ (~10MB, свежая сборка 13-Aug) |
| systemd юниты | 🔲 не сделано (для reference-hardware) |

## 2. Frontend (`apps/web/`)

### Sprint 1 — Cleanup + Home mobile-first ✅ DONE

| Задача | Статус |
|---|---|
| Удалить Shell, LeftNav, RightRail, CommandPalette, IntentCard, Modal | ✅ |
| Удалить DeviceLabScreen + route `/lab` | ✅ |
| HomeScreen переписан в mobile-first | ✅ (screen → header → ConnectionBanner → main → NavBar + FAB) |

### Sprint 2 — Add Device redesign ✅ DONE

| Задача | Статус |
|---|---|
| AddDeviceScreen под 4 стадии (pick-ecosystem → enter-code → commissioning → success/error) | ✅ |
| `components/EcosystemPicker/` | ✅ |
| `components/EcosystemGuide/` | ✅ |
| `components/SetupCodeField/` (auto-format `1234-5678-901`) | ✅ |
| `lib/ecosystem-guides.ts` (тексты под Apple/Google/SmartThings/Alexa) | ✅ |
| `state/commissioningStore.ts` расширен под ecosystem + setupCode | ✅ |
| `api/discover.ts` → `openCommissionWithCode` | ✅ |

### Sprint 3 — Device Detail ✅ DONE

| Задача | Статус |
|---|---|
| `screens/DeviceDetailScreen.tsx` | ✅ |
| Route `/device/:id` | ✅ (в `App.tsx`) |
| Navigation с Home тап-на-body тайла | ✅ |

### Sprint 4 — Интеграция с Matter-бэком 🟡 IN PROGRESS

| Задача | Статус |
|---|---|
| Backend: `POST /devices/commission` HTTP + streaming | ✅ |
| Frontend: реальный вызов commissioning с 11-значным кодом | ✅ (api/discover.ts `openCommissionWithCode`) |
| End-to-end с виртуальным WARMBLIXT через virtual adapter | 🟡 не проверено вручную |
| **End-to-end с реальным WARMBLIXT** | 🔲 hardware test |

### Sprint 5 — Полировка 🔲 NOT STARTED

| Задача | Статус |
|---|---|
| Empty/loading/error states для всех экранов | 🟡 частично (EmptyState + LoadingSkeleton есть) |
| Motion tokens применены везде | 🟡 надо аудитить |
| Vitest tests | 🔲 tests не найдены (`apps/web/src/**/*.test.*` — пусто) |
| Финальная интеграция с макетами дизайнера | 🔲 макеты ещё не пришли |

## 3. Design (`docs/designer-brief.md`)

| Задача | Статус |
|---|---|
| Бриф написан + в проекте | ✅ |
| Финальные macros из Figma | 🔲 ждём от дизайнера |
| Design tokens от дизайнера в финальном виде | 🔲 сейчас placeholder |
| Иконки экосистем (Apple/Google/SmartThings/Alexa) | 🔲 сейчас glyph-символы |
| Иллюстрации инструкций multi-admin | 🔲 позже |

## 4. Тесты

| Слой | Что есть | Не хватает |
|---|---|---|
| Go — unit | `registry`, `rules`, `matter/mapping`, `matter/wsclient` | Coverage report; storage layer |
| Go — integration | 🔲 | Полный main + виртуал через HTTP; matter sidecar integration test |
| Frontend — unit | 🔲 | Ничего |
| Frontend — component | 🔲 | RTL для DeviceTile, EcosystemPicker, SetupCodeField |
| Frontend — e2e | 🔲 | Playwright — не в MVP |

**Итого — тестами не покрыто:** frontend (Vitest не подключён к CI, тестов нет), Go integration path.

## 5. Готовность к тесту с реальным WARMBLIXT

Чтобы физически прогнать WARMBLIXT — нужно:

1. **Hardware:** сам WARMBLIXT + Apple TV 4K / HomePod / Google Nest Hub (Thread Border Router). Пользователь → у тебя есть, но WARMBLIXT ещё не куплен?
2. **Software:** `docker compose up -d` в репо. Оба контейнера должны запуститься на host-network mode.
3. **Первичный pairing:** WARMBLIXT в родной экосистеме (Apple Home) через QR на коробке. Затем «Turn on Pairing Mode» — получить 11-значный код.
4. **Через web-UI:** `http://localhost:5173/add-device` (Vite dev) или `http://localhost:7777/ui/dashboard.html` (собранная сборка) → выбрать Apple Home → ввести код.
5. Убедиться что WARMBLIXT появился в `/devices`, поменять яркость/color_temp — увидеть физический эффект.

**Известные риски первого прогона:**
- Docker host-network на macOS Docker Desktop — требует 4.34+ и включённый в Settings → Resources → Network. Проверить.
- matter.js 0.17 API — если WARMBLIXT prompt'нет за что-то экзотическое, может быть неожиданное падение. Логи хорошо смотреть.
- Первый commissioning может подвиснуть на 30-60 сек — matter.js делает Thread joining через Border Router, это долго.

## 6. Что осталось в открытых вопросах брифов

### Frontend (§14)

| Вопрос | Статус | Резолюция |
|---|---|---|
| Скриншоты инструкций multi-admin | 🔲 open | Ждём дизайнера |
| Room-picker: hardcoded или API | 🟡 hardcoded | В коде уже hardcoded 4 варианта — оставили |
| Delete device UX | 🟡 через Device Detail | Backend endpoint `DELETE /devices/{id}` есть |
| Reset onboarding-флага | 🔲 open | Оставили на v1.1 |
| Ecosystem «Прочее» | 🟡 частично | Ecosystem picker есть, «Другой способ» кнопка на fallback |

### Matter-adapter (§9)

| Вопрос | Статус | Резолюция |
|---|---|---|
| Storage matter-fabric-state | ✅ `matter-data/` в repo, docker-volume | |
| QR или setup code | ✅ text-input | |
| Backup fabric | 🔲 open | Не приоритет |
| matter.js log format | 🔲 open | Проверить в бою |

## 7. Известные issues и tech debt

- **Frontend .js файлы рядом с .tsx** — похоже это результат `tsc` output где-то. Надо `tsconfig.build.json` или excludeFromEmit. Не блокер, но фейкает файл-листинг.
- **Frontend proto/gen/** — пустая директория (протос компилировался, но результат не там). Проверить `gen:proto` script.
- **Frontend без Vitest тестов** — ни одного. Скорее debt, а не блокер.
- **git status shows apps/ и docs/ как untracked** — фронт-код и брифы не в git! Только `cmd/`, `internal/`, `bin/keystone`, `proto/` в истории. Надо `git add apps/ docs/` и коммитить.

## 8. Suggested next steps (приоритетно)

1. **Закоммитить apps/ и docs/ в git.** Сейчас несколько тысяч строк работы не под контролем версий.
2. **Прогнать WARMBLIXT физически.** Купить лампу + запустить docker compose + пройти multi-admin flow. Живое подтверждение всей архитектуры.
3. **Vitest — базовый testsuite для фронта.** Хотя бы 5-6 тестов на критичные компоненты (SetupCodeField auto-format, DeviceTile states, EcosystemPicker selection).
4. **Финальный дизайн от дизайнера.** Приоритет 1 из designer-brief. Заменит placeholder-стили.
5. **Storybook опционально.** Если планируем расти команду фронта — нужен.

## 9. Кратко git history

```
887e53c feat(ui): commission + delete controls in the dashboard
62901fa feat(deploy): docker-compose brings up the whole stack
0b6b341 feat(matter): partial live-event subscription + watch-events helper
2bdc6ca feat(devices): HTTP commissioning + auto-sync + delete
4315d58 feat(matter): real read/write/invoke via endpoint.commandsOf + ops scripts
8aa6998 feat(matter): wire sidecar controller to real matter.js 0.17
404c68b feat(matter): WebSocket JSON-RPC client + wire into keystone daemon
4c9175a feat(matter): matter.js sidecar skeleton (Phase 1)
9ae351a feat(virtual): add color_temp feature to virtual lights
753e1de feat(matter): scaffold Matter transport adapter
ded434b chore: rename module, rollback DIRIGERA adapter, prep for Matter
```

11 коммитов за пару дней. Основная работа — Matter (7 из 11), плюс deploy и UI polish.
