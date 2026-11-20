# Casanomi‑paper redesign — Plan 4: Add flows & automations

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The «+» menu's three branches become real: **Add a device** is one screen that never mentions Zigbee or Bluetooth; **New scene** remembers what the devices are doing right now; **New automation** offers ready‑made rules first and then splits into schedule · timer · rule. The Automations tab becomes collapsible sections, and the timer gets a proper duration wheel.

**Architecture:** The screens already exist (`FindDevicesScreen`, `SceneForm`, `ScheduleForm`, `TimerForm`, `RuleEditor`) and speak the protocol correctly. We re‑shape them: `FindDevicesScreen` hides the transport; the automations tab swaps its chip row for collapsible sections; `TimerForm` swaps its minute presets for a wheel; a new `model/suggestions.ts` derives the ready‑made automations from what the home actually has.

**Tech Stack:** Expo 57 / RN 0.86 + react‑native‑web, TypeScript, vitest + @testing-library/react in jsdom.

**Spec:** `docs/superpowers/specs/2026-11-20-casanomi-paper-redesign-design.md`
**Mockups:** `casanomi-add-flows.html` (all seven screens)

## Global Constraints

- No firmware, protocol, or data‑model changes. `app/` only.
- Interface language stays **English**.
- The user is never shown the transport (Zigbee / Bluetooth / Matter). Those are the hub's business.
- Suggestions are **derived from the home** — no hardcoded list of rules that the devices do not support.
- The protocol calls do not change: the forms keep sending exactly what they send today.
- `expo export --platform web` must bundle; `tsc --noEmit` clean; the vitest suite stays green.

## Review Focus

- **One screen for adding a device, no radio jargon.** → Task 1 asserts the words *Zigbee*, *Bluetooth* and *Matter* are **not** on the screen, and that the QR path is still there.
- **A suggestion only appears if the home can support it.** → Task 2 asserts a home with no motion sensor and no light gets no motion suggestion.
- **Tapping a suggestion opens a filled‑in rule, only Save left.** → Task 2 asserts the draft is pre‑filled.
- **The Automations tab has collapsible sections with counts.** → Task 3 asserts the counts and that a tap collapses.
- **The timer is a duration wheel, not presets.** → Task 4 asserts the wheel and that it produces hours + minutes.

---

### Task 1: Add a device — one screen, no jargon

**Files:**
- Modify: `app/src/ui/FindDevicesScreen.tsx`
- Test: `app/test/ui-find.test.tsx` (extend)

**Interfaces:**
- Keeps its props (`home`, `commands`, `onClose`).
- The screen shows:
  - a **finder panel** — a dark warm panel (`#201a12`) with a spinner, «Looking for new devices…» and a countdown («1:52 left»);
  - a **Found** list — each device as a row: an icon tile, the name, and a human type («Light», «Sensor», «Plug») + when it appeared;
  - a **ghost button** at the bottom: «Scan a QR code» (the Matter path, the fallback).

- [ ] **Step 1: Write the failing test** — with the mock hub:
  - the screen says «Looking for new devices…»;
  - the words *Zigbee*, *Bluetooth* and *Matter* are **not** on the screen (`queryByText` → null);
  - «Scan a QR code» is present;
  - a device that appears is listed with a human type.

- [ ] **Step 2: Run, verify fail** — the current screen says «Press the pair button on the device» and shows the transport sections.

- [ ] **Step 3: Implement**:
  - keep every protocol call (`ble.start`/`ble.stop`, the join window, `matter.status`, the QR scan) — only the wording and the layout change;
  - the BLE‑found devices and the Zigbee‑joined ones merge into **one list** («Found»), sorted by arrival time;
  - the device's type comes from `deviceLabel(device)` in `model/devices.ts` («Light», «Sensor», «Plug», …) — never the raw profile id;
  - the countdown comes from the join window's remaining time (check `hooks/useJoinWindow.ts` for what it exposes).

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/FindDevicesScreen.tsx app/test/ui-find.test.tsx
git commit -m "feat(ui): one screen for adding a device, without the radio jargon"
```

---

### Task 2: Suggested automations

**Files:**
- Create: `app/src/model/suggestions.ts`
- Create: `app/src/ui/Suggestions.tsx`
- Modify: `app/src/ui/AutomationsScreen.tsx` (the suggestion banner) and the Add → Automation branch (Plan 2's `AddMenu`)
- Test: `app/test/ui-suggestions.test.tsx` (new), `app/test/automation.test.ts` (extend)

**Interfaces:**
- Produces:
  - `model/suggestions.ts`:
    - `interface Suggestion { id: string; title: string; detail: string; kind: 'rule' | 'schedule'; draft: RuleDraft | null; }`
    - `suggestionsFor(home: HomeState): Suggestion[]` — derives the ready‑made automations from the entities:
      1. **motion → light**: a motion sensor and a light in the same room → «Hallway light on when motion».
      2. **everything off at sunrise**: lights and switches → «Everything off at sunrise».
      3. **a plug that draws too much**: a switch with power → «Alert if the heater draws > 1.5 kW».
      4. **a lamp left on overnight** → «Turn the floor lamp off at 02:00».
    - Every suggestion must be **supported**: if the home has no motion sensor, there is no motion suggestion.
  - `ui/Suggestions.tsx`: `Suggestions({ items, onPick })` — the mockup's row: an icon tile in the gold wash, the title, the detail, and a round ink «+» button.

- [ ] **Step 1: Write the failing test**:
  - a home with a motion sensor and a light produces the motion suggestion;
  - a home without a motion sensor does **not**;
  - the suggestion row renders the title and the «+» button;
  - tapping «+» calls `onPick(suggestion)` with a **pre‑filled** draft (the trigger and the action are set).

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement**:
  - `suggestionsFor` works off `home.entities` and their attributes; it uses `attrOptions` / `primarySwitch` / `readingAttrs` from `model/devices.ts` and the draft helpers in `model/automation.ts` (`emptyDraft`, `draftRequest`).
  - the suggestion appears in two places, as the spec says:
    1. a slim banner at the top of the **Automations** tab;
    2. a **Suggested for your home** section at the top of the **New automation** screen, above *Or start from scratch*.
  - tapping a suggestion opens the rule editor with the draft filled in — only *Save* is left to press.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/model/suggestions.ts app/src/ui/Suggestions.tsx app/src/ui/AutomationsScreen.tsx app/test
git commit -m "feat(ui): automations the home can actually do"
```

---

### Task 3: The Automations tab — collapsible sections

**Files:**
- Modify: `app/src/ui/AutomationsScreen.tsx`
- Create: `app/src/ui/CollapsibleSection.tsx`
- Test: `app/test/ui-automation.test.tsx` (extend)

**Interfaces:**
- Produces:
  - `CollapsibleSection({ title, count, children, initiallyOpen })` — the mockup's header: an uppercase title, a count badge in the gold wash, a chevron that rotates; a tap toggles.
  - The tab's sections: **Scenes** (3), **Schedules** (2), **Timers** (1), **Rules** (4) — each with its count and its own body.
  - The scene chips move *into* the Scenes section (the mockup shows them as gold pills with a ▶).
  - The card body keeps the sentence format the model already produces (`describeSchedule`, `describeTimer`, `describeRule`).

- [ ] **Step 1: Write the failing test** — the four section headers are present with their counts; the Rules section is collapsed by default (its cards are not in the tree); tapping it opens them.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** — replace the chip row with the four `CollapsibleSection`s. The **Rules** section starts collapsed (the mockup shows Scenes and Schedules open, Timers and Rules closed); the others start open. The `Add` buttons inside each section stay.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/CollapsibleSection.tsx app/src/ui/AutomationsScreen.tsx app/test/ui-automation.test.tsx
git commit -m "feat(ui): the automations tab folds into sections"
```

---

### Task 4: The timer — a duration wheel

**Files:**
- Create: `app/src/ui/DurationWheel.tsx`
- Modify: `app/src/ui/AutomationForms.tsx` (`TimerForm`)
- Test: `app/test/ui-timer.test.tsx` (new), `app/test/automation.test.ts` (extend)

**Interfaces:**
- Consumes: `model/automation.ts`'s `timerRequest` — check its signature; the form must keep sending the same thing.
- Produces:
  - `DurationWheel({ hours, minutes, onChange })` — two columns (hours · minutes) that scroll; the selected row sits in the middle band (the mockup's gold band) with the value large and the unit small; the rows fade towards the edges.
  - `TimerForm` replaces its *After (minutes)* presets with the wheel; it keeps *Do* (the action chips) and *To* (the device), and shows the live countdown («Fires once, in 29:59, then forgets itself») using `formatCountdown` from `model/automation.ts`.

- [ ] **Step 1: Write the failing test**:
  - the wheel shows an hours column and a minutes column;
  - changing the minutes calls `onChange` with the new minutes;
  - `TimerForm` produces a timer request of the right length (the existing `timerRequest` test covers the request; add one that the form's wheel feeds it);
  - the countdown line appears.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** `DurationWheel`:
  - a `ScrollView` per column (vertical), snapping to the items (`snapToInterval`, `decelerationRate: 'fast'`), with the centre band drawn behind (a gold wash with hairlines top and bottom);
  - the selected value is the one at the band's centre — read it from the scroll offset (`onScroll` + `scrollEventThrottle`);
  - on the web the same wheel works (react‑native‑web's `ScrollView` supports snapping);
  - the fade at the edges: the mockup uses a CSS mask — in RN, render three stacked views (fading) or simply reduce the opacity of the rows away from the centre.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/DurationWheel.tsx app/src/ui/AutomationForms.tsx app/test/ui-timer.test.tsx app/test/automation.test.ts
git commit -m "feat(ui): the timer gets a duration wheel"
```

---

### Task 5: New scene — remember what the devices do now

**Files:**
- Modify: `app/src/ui/AutomationForms.tsx` (`SceneForm`)
- Test: `app/test/ui-automation.test.tsx` (extend)

**Interfaces:**
- `SceneForm` keeps its props; the body changes:
  - a note at the top: «A scene remembers what the chosen devices are doing **right now**, to bring back in one tap.»;
  - a **Name** field;
  - an **Include these devices** list: each device is a row with an icon tile, the name, and its current state («On · 70 %», «On», «Off») and a **checkbox** on the right (the gold check).

- [ ] **Step 1: Write the failing test** — the scene form shows the devices with their current state and checkboxes; picking two of them and saving sends the same request as today.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** — restyle only; the request stays `scene.save` with the same payload. The current state of a device comes from `model/devices.ts` (`primarySwitch`, `brightnessPercent`).

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/AutomationForms.tsx app/test/ui-automation.test.tsx
git commit -m "feat(ui): the new-scene form remembers the devices' current state"
```

---

### Task 6: The New automation screen — three kinds

**Files:**
- Create: `app/src/ui/AutomationChooser.tsx`
- Modify: `app/src/ui/AutomationForms.tsx` (the forms' headers), `app/src/ui/Home.tsx` (the Add → Automation branch)
- Test: `app/test/ui-automation.test.tsx` (extend)

**Interfaces:**
- Produces: `AutomationChooser({ onSchedule, onTimer, onRule, suggestions })` — the mockup's screen:
  - the question «What should trigger it?» at the top;
  - three rows: **Schedule** («At a time of day / sunrise / a date»), **Timer** («After a countdown»), **Rule** («When a device does something»);
  - above them, the **Suggested for your home** section (Task 2) and below *Or start from scratch*.

- [ ] **Step 1: Write the failing test** — the three rows are present; tapping *Schedule* calls `onSchedule`; the suggestions are above the rows.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** — the screen lives under the Add menu (Plan 2's `onAutomation`). The three rows open the existing `ScheduleForm` / `TimerForm` / `RuleEditor`. Their headers change to the mockup's (`‹ New schedule`, a *Save* on the right) — `FormShell` already has a title and a save button; check its props.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/AutomationChooser.tsx app/src/ui/AutomationForms.tsx app/src/ui/Home.tsx app/test/ui-automation.test.tsx
git commit -m "feat(ui): the new-automation screen — schedule, timer, rule"
```

---

## Notes for the executor

- `model/automation.ts` already builds every request (`scheduleRequest`, `timerRequest`, `draftRequest`, and the scene request). **Do not write new request builders** — the forms must keep sending what they send today.
- The rule editor already renders the blocks and the live sentence; Task 2 only *pre‑fills* it.
- The `Segmented` control arrives in Plan 2 — use it for the automations' kind chooser if it fits, otherwise the existing `Chip` row.
- `useCountdown` already ticks the timers; the wheel's countdown line must not start its own timer.
- After this plan the app must still work end to end against `npm run mock-hub`, and every add‑flow branch must reach a real protocol call.
