# Единый поток создания автоматизации — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Заменить выбор одного из четырёх типов автоматизаций единым потоком «Когда → сделать»: вход через примеры → фраза-конструктор, один список «Автоматизации», Scenes отдельно.

**Architecture:** Чисто фронтенд в `app/`. Новый единый черновик `AutomationDraft` с видом триггера (device/timeOfDay/date/delay) маршрутизируется функцией `automationRequest` в существующие вызовы `rule.add` / `schedule.add` / `timer.add` (их request-builder'ы уже написаны и переиспользуются). Ядро, прошивка и протокол не меняются.

**Tech Stack:** TypeScript, React Native (Expo) + react-native-web, vitest, jsdom.

**Spec:** [docs/superpowers/specs/2026-11-18-automation-create-flow-design.md](../specs/2026-11-18-automation-create-flow-design.md)

## Global Constraints

- **Ядро/протокол/прошивку не трогаем.** Только `app/`. (spec §2, §3.1)
- Каждый вид триггера ложится ровно на один вызов: device→`rule.add`, timeOfDay/date→`schedule.add`, delay→`timer.add`. (spec §3.1)
- `and` / `between` (условие, окно времени) допустимы **только** при `kind: 'device'`; для time/date/delay их нет в ядре и они не должны уходить на провод. (spec §3.2, §5.4)
- Значения — в единицах человека через существующие `parseInput` / `SCALES` (яркость %, температура °C). (spec §3.4)
- Переиспользовать существующие `draftRequest`, `scheduleRequest`, `timerRequest`, `draftToSentence`, `ConditionDraft`, `ActionDraft`, `AttrOption`, `parseInput` — не дублировать. (spec §5.1, §5.2, §10)
- Scenes остаются отдельным разделом; сцена доступна как «сделать». (spec §5.6)
- Не уронить текущие vitest (491 на момент спеки); добавить ~20–30. (spec §7)
- Копирайт: виды названы действиями пользователя, не «rule/schedule/timer»; формулировки присутствия из `presenceValueText`. (spec §6)
- Типы из `src/protocol/messages.ts`: `Days = 'daily'|'weekdays'|'weekends'|Weekday[]`, `Action = 'on'|'off'|'toggle'`, `CmpOp`, `Scene`. `Commands.request(op, args?) => Promise<Record<string,unknown>|null>`, `Commands.busy`, `Commands.notify(text)`.

## Review Focus

- **Смена вида триггера после частичного заполнения** (device→timeOfDay и обратно): запрос не должен нести чужие поля (например `and` от правила в расписание). → тест в Task 1.
- **«Сделать = сцена», когда сцен нет или сцена удалена**: конструктор не предлагает сцену при пустом списке; пресет со сценой при отсутствии сцен не ломается. → тесты в Task 2 (пресет) и Task 3 (UI).
- **time/date/delay с остаточными `and`/`between`**: `automationRequest` обязана их игнорировать для не-device видов. → тест в Task 1.
- **Прошедшая/битая дата у вида `date`**: понятная ошибка (через `scheduleRequest`), а не падение. → тест в Task 1.
- **Пресет с незаполненным устройством**: сохранение блокируется с «pick a device», без краша. → тест в Task 2 (черновик валиден-частичен) и Task 3 (UI показывает ошибку).

---

### Task 1: Единая модель черновика и маршрутизация

**Files:**
- Modify: `app/src/model/automation.ts` (добавить рядом с существующими; ничего не удалять)
- Test: `app/test/automation.test.ts`

**Interfaces:**
- Consumes: существующие `ConditionDraft`, `ActionDraft`, `draftRequest`, `scheduleRequest`, `timerRequest`, `draftToSentence`, `describeTimePoint`, `formatCountdown`, `Parsed<T>`, `Built`; типы `Days`, `Action`, `CmpOp`.
- Produces:
  - `type TriggerKind = 'device' | 'timeOfDay' | 'date' | 'delay'`
  - `interface AutomationDraft { kind: TriggerKind; trigger: ConditionDraft | null; conditions: ConditionDraft[]; window: { from: string; to: string } | null; cooldown_s: number | null; time: string; days: Days; date: string | null; minutes: string; action: ActionDraft | null }`
  - `function emptyAutomationDraft(kind: TriggerKind): AutomationDraft`
  - `function automationRequest(d: AutomationDraft): Parsed<{ op: 'rule.add' | 'schedule.add' | 'timer.add'; args: Record<string, unknown> }>`
  - `function automationToSentence(d: AutomationDraft): string`

- [ ] **Step 1: Write the failing tests** in `app/test/automation.test.ts`:

```ts
// routing: each kind → its op + args
test('device kind routes to rule.add', () => {
  const d = { ...emptyAutomationDraft('device'),
    trigger: { option: boolOpt /* occupied, entity 1 */, op: '==', value: 'on' },
    action: { option: switchOpt /* on_off, entity 2 */, value: 'on', scene: null } };
  const r = automationRequest(d);
  expect(r).toEqual({ ok: true, value: { op: 'rule.add', args: { when: {...}, then: {...} } } });
});
test('timeOfDay kind routes to schedule.add with days', () => { /* time 22:00, days daily, action off device */ });
test('date kind routes to schedule.add with date', () => { /* time 19:00, date 2026-12-31 */ });
test('delay kind routes to timer.add with delay_s', () => { /* minutes "10" → delay_s 600 */ });
test('scene as the action works for every kind', () => { /* action.scene set → args carry scene, no entity */ });

// Review Focus: stale fields never leak to non-device ops
test('timeOfDay ignores leftover conditions and window', () => {
  const d = { ...emptyAutomationDraft('timeOfDay'), time: '22:00', days: 'daily',
    action: { option: switchOpt, value: 'off', scene: null },
    conditions: [{ option: boolOpt, op: '==', value: 'on' }], window: { from: '1', to: '2' } };
  const r = automationRequest(d);
  expect(r.ok).toBe(true);
  expect(r.ok && 'and' in r.value.args).toBe(false);
  expect(r.ok && 'between' in r.value.args).toBe(false);
});

// Review Focus: bad date surfaces a clear error, not a throw
test('date in the past is rejected with a message', () => {
  const d = { ...emptyAutomationDraft('date'), time: '19:00', date: '2001-01-01',
    action: { option: switchOpt, value: 'off', scene: null } };
  expect(automationRequest(d).ok).toBe(false);
});
test('device kind with two conditions is rejected', () => { /* conditions.length 2 → ok:false */ });

// sentences
test('automationToSentence reads each kind', () => {
  expect(automationToSentence(deviceDraft)).toMatch(/^When /);
  expect(automationToSentence(timeDraft)).toContain('22:00');
  expect(automationToSentence(dateDraft)).toContain('2026');
  expect(automationToSentence(delayDraft)).toMatch(/10 min/);
});
test('automationToSentence handles a half-built draft', () => {
  expect(automationToSentence(emptyAutomationDraft('device'))).toContain('…');
});
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd app && npx vitest run test/automation.test.ts`
Expected: FAIL — `automationRequest is not defined` (and siblings).

- [ ] **Step 3: Implement the four exports in `app/src/model/automation.ts`.**

`emptyAutomationDraft(kind)` returns the struct with kind-neutral defaults (`time: ''`, `days: 'daily'`, `date: null`, `minutes: ''`, empty trigger/conditions/action). `automationRequest` switches on `kind`: `device` reuses `draftRequest` (build a `RuleDraft` from the shared fields) and wraps as `{ op: 'rule.add', args }`; `timeOfDay`/`date` call `scheduleRequest` with `action`→`entity+action` or `scene`, `days` vs `date`; `delay` calls `timerRequest`. For non-device kinds build the schedule/timer form **only from their own fields**, so `and`/`between`/`cooldown` cannot leak. `automationToSentence` dispatches by kind: device delegates to the existing `draftToSentence` phrasing; time/date/delay compose from `describeTimePoint`/`describeDate`/`formatCountdown` + the shared action tail (extract an `actionTail(action)` helper used by all four).

- [ ] **Step 4: Run to verify they pass**

Run: `cd app && npx vitest run test/automation.test.ts`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/model/automation.ts app/test/automation.test.ts
git commit -m "feat(app): unified AutomationDraft + automationRequest routing"
```

---

### Task 2: Таблица примеров (пресетов)

**Files:**
- Modify: `app/src/model/automation.ts`
- Test: `app/test/automation.test.ts`

**Interfaces:**
- Consumes: `AutomationDraft`, `TriggerKind`, `emptyAutomationDraft`, `automationRequest`.
- Produces:
  - `interface PresetAutomation { id: string; icon: string; label: string; make(): AutomationDraft }`
  - `const PRESET_AUTOMATIONS: PresetAutomation[]` — the six from spec §5.5 (`night-off`, `light-on-arrive`, `off-after-delay`, `react-to-sensor`, `scene-on-time`, plus `scratch` handled by the screen, not the table).

- [ ] **Step 1: Write the failing tests:**

```ts
test('every preset makes a draft of its declared kind', () => {
  for (const p of PRESET_AUTOMATIONS) expect(p.make().kind).toBeTruthy();
});
test('night-off prefills time and off, leaves the device blank', () => {
  const d = PRESET_AUTOMATIONS.find(p => p.id === 'night-off')!.make();
  expect(d.kind).toBe('timeOfDay');
  expect(d.time).toBe('23:00');
  expect(d.action?.option).toBeNull();          // device is the person's to pick
  expect(d.action?.value).toBe('off');
});
test('a preset draft that still needs a device is rejected with a clear error', () => {
  const d = PRESET_AUTOMATIONS.find(p => p.id === 'off-after-delay')!.make();
  const r = automationRequest({ ...d, minutes: '5' });
  expect(r.ok).toBe(false);                      // no device yet → "pick a device", no throw
});
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd app && npx vitest run test/automation.test.ts`
Expected: FAIL — `PRESET_AUTOMATIONS is not defined`.

- [ ] **Step 3: Implement `PresetAutomation` + `PRESET_AUTOMATIONS`.** Each `make()` returns `emptyAutomationDraft(kind)` with only device-independent fields set (time/days/kind, and `action.value`/`scene` hints with `option: null`), per spec §5.5. No scene is prefilled unless the preset's kind is scene-based; a scene preset leaves `action.scene` as a name only if scenes exist is the screen's concern — the table just sets `action: { option: null, value: '', scene: null }` and marks kind.

- [ ] **Step 4: Run to verify they pass**

Run: `cd app && npx vitest run test/automation.test.ts`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/model/automation.ts app/test/automation.test.ts
git commit -m "feat(app): preset automations table (examples entry)"
```

---

### Task 3: Конструктор — фраза со слотами (экран B)

**Files:**
- Create: `app/src/ui/AutomationBuilder.tsx`
- Test: `app/test/ui-automation.test.tsx`

**Interfaces:**
- Consumes: `AutomationDraft`, `TriggerKind`, `automationRequest`, `automationToSentence`, `emptyAutomationDraft`, `attrOptions`, `opsFor`, `inputUnit`, `presenceValueText`, `COMPARISONS`; `Entity`, `Scene`; controls `Button`, `Chip`, `ChipRow`, `Input`, `ErrorText`; reuse the `Choice` chip-sheet pattern from `RuleEditor.tsx`.
- Produces:
  - `interface AutomationBuilderProps { entities: Entity[]; scenes: Scene[]; hasLocation: boolean; initial: AutomationDraft; busy: boolean; onSubmit(op: string, args: Record<string, unknown>): Promise<boolean>; onCancel(): void }`
  - `export function AutomationBuilder(props): JSX.Element`

- [ ] **Step 1: Write the failing UI tests** (react-native-web + jsdom), rendering with sample entities/scenes:

```tsx
test('the live sentence updates as slots are filled', () => { /* fill trigger → sentence contains it */ });
test('choosing the time-of-day kind shows a time slot and no condition slot', () => {
  // render with initial kind 'timeOfDay'; "Only while"/"More options" is absent
});
test('the device kind shows the "more options" (condition / between / cooldown)', () => {});
test('save calls onSubmit with the routed op', async () => {
  // fill a valid timeOfDay+off+device → onSubmit called with 'schedule.add'
});
test('a scene is offered as the action only when scenes exist', () => {
  // scenes: [] → no "A scene" chip; scenes: [x] → chip present
});
test('saving a preset draft with no device shows "pick a device"', async () => {
  // initial = off-after-delay preset + minutes filled, no device → ErrorText shown, onSubmit not called
});
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd app && npx vitest run test/ui-automation.test.tsx`
Expected: FAIL — cannot find `AutomationBuilder`.

- [ ] **Step 3: Implement `AutomationBuilder`.** One screen: a `KIND` row (four `Choice`/cards opening the kind), then kind-specific slots (device: the existing `ConditionBlock` shape; timeOfDay: time + days chips; date: date + time inputs; delay: minutes input), then the `СДЕЛАТЬ` block (device via `attrOptions(entities,{writable:true})` or a scene chip-row when `scenes.length>0`), then the live `automationToSentence` block, then "More options" **only when `kind==='device'`** (window + cooldown + one condition), then Cancel/Save. `save()` calls `automationRequest(draft)`, on `ok:false` sets error, on `ok:true` calls `onSubmit(op, args)`. Reuse wording/units helpers; do not re-implement parsing.

- [ ] **Step 4: Run to verify they pass**

Run: `cd app && npx vitest run test/ui-automation.test.tsx`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/AutomationBuilder.tsx app/test/ui-automation.test.tsx
git commit -m "feat(app): AutomationBuilder — sentence editor with adaptive slots"
```

---

### Task 4: Экран примеров (вход C → B)

**Files:**
- Modify: `app/src/ui/AutomationBuilder.tsx` (add the preset-picker phase) or Create: `app/src/ui/NewAutomation.tsx` (wrapper owning the two phases)
- Test: `app/test/ui-automation.test.tsx`

**Interfaces:**
- Consumes: `PRESET_AUTOMATIONS`, `emptyAutomationDraft`, `AutomationBuilder`.
- Produces:
  - `interface NewAutomationProps { entities: Entity[]; scenes: Scene[]; hasLocation: boolean; busy: boolean; onSubmit(op: string, args: Record<string, unknown>): Promise<boolean>; onCancel(): void }`
  - `export function NewAutomation(props): JSX.Element` — phase 1 lists `PRESET_AUTOMATIONS` + "С чистого листа"; picking one opens `AutomationBuilder` with that `initial` draft (scratch → a kind-chooser then an empty draft).

- [ ] **Step 1: Write the failing tests:**

```tsx
test('the examples list shows every preset plus "from scratch"', () => {});
test('picking an example opens the builder with its fields prefilled', () => {
  // tap "night-off" → builder shows 23:00
});
test('"from scratch" opens an empty builder', () => {});
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd app && npx vitest run test/ui-automation.test.tsx`
Expected: FAIL — cannot find `NewAutomation`.

- [ ] **Step 3: Implement `NewAutomation`** as the two-phase wrapper (examples → builder), passing the chosen preset's `make()` as `initial`. "С чистого листа" first asks the kind (reuse the kind `Choice`), then `emptyAutomationDraft(kind)`.

- [ ] **Step 4: Run to verify they pass**

Run: `cd app && npx vitest run test/ui-automation.test.tsx`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/AutomationBuilder.tsx app/src/ui/NewAutomation.tsx app/test/ui-automation.test.tsx
git commit -m "feat(app): examples entry (C) that opens the builder prefilled"
```

---

### Task 5: Слитый список «Автоматизации» + единый вход; Scenes отдельно

**Files:**
- Modify: `app/src/ui/AutomationsScreen.tsx`
- Remove: `app/src/ui/RuleEditor.tsx` (and its import) once unreferenced; `AutomationForms.tsx` `ScheduleForm`/`TimerForm` become unused — delete the unused exports if nothing else imports them (grep first).
- Test: `app/test/ui-automation.test.tsx`

**Interfaces:**
- Consumes: `NewAutomation`, existing `describeRule`, `describeSchedule`, `describeTimer`, `useCountdown`, `Commands`.
- Produces: an Automations screen with two sections — **Автоматизации** (merged) and **Scenes** (unchanged) — and one "Создать автоматизацию" entry opening `NewAutomation`.

- [ ] **Step 1: Write the failing tests:**

```tsx
test('rules, schedules and timers appear in one Automations list', () => {
  // home with one of each → three rows under "Automations", not four tabs
});
test('deleting a row calls the right op for its source', () => {
  // schedule row → 'schedule.del'; timer row → 'timer.cancel'; rule row → 'rule.del'
});
test('Scenes stay in their own section', () => {});
test('Create automation opens the examples screen', () => {});
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd app && npx vitest run test/ui-automation.test.tsx`
Expected: FAIL.

- [ ] **Step 3: Rework `AutomationsScreen`.** Replace the four section chips with two: **Автоматизации** and **Scenes**. Build the merged list by mapping `home.rules`/`home.schedules`/`home.timers` to rows `{ key: ('rule'|'sched'|'timer', index), title, detail, onDelete }` using the existing `describe*` (do not reword them); keep one-shots readable via their existing wording (countdown / "Once"). One "Создать автоматизацию" button opens `NewAutomation` (its `onSubmit(op,args)` calls `commands.request(op,args)` and closes on success — reuse the existing `add()` open-form id guard). Scenes section unchanged. Remove `RuleEditor` usage.

- [ ] **Step 4: Run tests**

Run: `cd app && npx vitest run test/ui-automation.test.tsx`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add -A app/src/ui app/test/ui-automation.test.tsx
git commit -m "feat(app): one Automations list + single create flow; Scenes separate"
```

---

### Task 6: Полный прогон и визуальная проверка

**Files:** none (verification only)

- [ ] **Step 1: Types + whole suite**

Run: `cd app && npx tsc --noEmit && npx vitest run`
Expected: tsc clean (кроме известной ошибки `expo-camera`, если присутствует); vitest — всё зелёное, +~20–30 новых.

- [ ] **Step 2: Web bundle**

Run: `cd app && npx expo export --platform web`
Expected: собирается без ошибок.

- [ ] **Step 3: Визуальная проверка в браузере против mock-хаба** (как в других app-итерациях): открыть Automations, пройти оба потока — пример → конструктор и «с чистого листа»; создать по одной автоматизации каждого вида (устройство / время / дата / через столько-то) и одну со сценой как действием; убедиться, что все появляются в одном списке и удаляются. Приложить скриншоты.

- [ ] **Step 4: Commit any fixes**

```bash
git add -A && git commit -m "test(app): verify unified automation flow (suite + web + visual)"
```

---

## Self-Review

**Spec coverage:** §5.1 модель → Task 1; §5.2 маршрутизация → Task 1; §5.3 фраза → Task 1; §5.4 конструктор/адаптивные опции → Task 3; §5.5 примеры → Tasks 2,4; §5.6 слитый список + Scenes → Task 5; §6 копирайт → Tasks 1,3 (переиспование `presenceValueText`, человеческие названия видов); §7 тесты → во всех; §9 порядок сборки → задачи идут этим порядком. Покрыто.

**Review Focus → owning task:** смена вида/утечка полей → Task 1; сцена без сцен → Tasks 2,3; `and`/`between` на не-device → Task 1; битая дата → Task 1; пресет без устройства → Tasks 2,3. Все пять имеют тест.

**Type consistency:** `AutomationDraft`, `TriggerKind`, `automationRequest` (возвращает `{op,args}`), `automationToSentence`, `PRESET_AUTOMATIONS`/`PresetAutomation`, `AutomationBuilderProps`/`NewAutomationProps` согласованы между задачами; `onSubmit(op,args)` одинаков в Task 3/4/5.

**Proportion:** план соразмерен спеке — тела функций не расписаны, даны сигнатуры, имена тестов и ассерты; алгоритм маршрутизации описан словами, не кодом.
