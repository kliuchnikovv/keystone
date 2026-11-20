# Casanomi‑paper redesign — Plan 3: Home & device widgets

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Home screen becomes a home, not a list: rooms as chips at the top, cards grouped under room headings with a summary line, **gold glow** on what is on, and a widget per device type that shows that type's main thing. The lamp gets a **card‑as‑dimmer**: the card's fill *is* the brightness, a swipe changes it, a tap toggles.

**Architecture:** `DevicesScreen` already groups by room and lays devices on a grid (`model/grid.ts`). We replace the grid with a **vertical list of cards per room** (the mockups show cards, not a tile grid), and rewrite `DeviceWidget` into a family of widgets chosen by device type. The brightness gesture lives in a new `src/ui/Dimmer.tsx`.

**Tech Stack:** Expo 57 / RN 0.86 + react‑native‑web, TypeScript, vitest + @testing-library/react in jsdom.

**Spec:** `docs/superpowers/specs/2026-11-20-casanomi-paper-redesign-design.md`
**Mockups:** `casanomi-widgets-board.html` (lamp variants + device types), `casanomi-screens-board.html` (screen 1)

## Global Constraints

- No firmware, protocol, or data‑model changes. `app/` only.
- Interface language stays **English**.
- Gold is **decoration**: a lit icon tile, a soft glow, a highlighted value. It is never a filled action button (that is ink, from Plan 1).
- The **primary** action stays ink; gold never becomes a background behind text that must be read.
- Every widget must work with the data the hub actually has: the firmware supports `on_off`, `brightness`, `color_temp`, `color_xy`, `temperature`, `humidity`, `battery`, `occupied`, `contact`, `setpoint`, power. Widgets for devices that do not exist yet (blinds, thermostats with modes) are drawn but never fake data.
- `expo export --platform web` must bundle; `tsc --noEmit` clean; the vitest suite stays green.

## Review Focus

- **What is on glows gold; what is off is quiet.** → Task 1 asserts the glow and the quiet state.
- **A sensor offline fades, it does not turn red.** → Task 2 asserts opacity, not a red label.
- **The lamp's fill equals its brightness.** → Task 3 asserts the fill width tracks the value.
- **Tap toggles, swipe dims, long press opens the sheet.** → Task 3 asserts the gesture split and the tap/swipe threshold.
- **A card shows its type's main thing** (a plug its watts, a thermostat its target, a contact open/closed). → Task 2 covers the per‑type widgets.

---

### Task 1: The card, the gold glow, and the room chips

**Files:**
- Modify: `app/src/ui/DevicesScreen.tsx` (room chips, room headings, card list)
- Modify: `app/src/ui/DeviceWidget.tsx` (the card shell + glow)
- Modify: `app/src/ui/theme.tsx` (glow tokens)
- Test: `app/test/ui-devices.test.tsx` (extend), `app/test/ui-visual.test.tsx` (extend)

**Interfaces:**
- Consumes: `groupByRoom`, `roomSummary`, `summaryText` from `model/rooms.ts` / `model/devices.ts`.
- Produces:
  - Theme tokens: `glowBorder: string` (the gold hairline of a lit card), `glowShadow: string` (the soft gold shadow), `glowFill: string` (the warm gold wash over a lit card's top).
  - `RoomChips({ rooms, active, onPick })` — the mockup's row of room pills above the rooms; *All* plus each room.
  - `DeviceWidget` keeps its props; the **shape** prop goes away (cards are all one size now) — check every call site.

- [ ] **Step 1: Write the failing test** — in `test/ui-devices.test.tsx`: a lit device's card carries the gold border; an unlit one carries the plain border; the room chips exist and *All* is there.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement**:
  - theme tokens (light): `glowBorder: 'rgba(207,159,51,0.55)'`, `glowShadow: '#cf9f33'`, `glowFill: '#fff8e8'`.
    (dark): `glowBorder: 'rgba(224,178,63,0.5)'`, `glowShadow: '#e0b23f'`, `glowFill: '#2f2617'`.
  - `DeviceWidget`: the card is `backgroundColor: on ? t.glowFill : t.card`, `borderColor: on ? t.glowBorder : t.border`, radius `t.cardRadius`, and — on native only — a gold shadow. RN's `boxShadow` is not universal; use `shadowColor`/`shadowOpacity`/`shadowRadius`/`elevation` guarded by `Platform.OS !== 'web'`, and on web a `borderColor` + a slightly stronger border (the mockup's glow is mostly a border + a warm tint).
  - the icon tile: `backgroundColor: on ? t.accent : t.fill` with the glyph in `t.onAccent` when on (the mockup's filled gold tile), `t.muted` when off.
  - `DevicesScreen`: a `RoomChips` row at the top (*All* + the rooms); the sections below are filtered by the active room; each room heading shows the room name and the summary line («2 on · 38 W») — `roomSummary`/`summaryText` already do this.
  - the device list becomes a **vertical stack of cards** (one per row), not the CSS grid — remove the `gridLayout` call and the `shape` prop.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/theme.tsx app/src/ui/DevicesScreen.tsx app/src/ui/DeviceWidget.tsx app/test
git commit -m "feat(ui): device cards with a gold glow; rooms as chips"
```

---

### Task 2: One widget per device type

**Files:**
- Create: `app/src/ui/widgets.tsx` (the per‑type widgets)
- Modify: `app/src/ui/DeviceWidget.tsx` (choose the widget by type)
- Test: `app/test/ui-widgets.test.tsx` (new)

**Interfaces:**
- Produces, in `widgets.tsx`:
  - `PlugWidget`, `LampWidget`, `ColorLampWidget`, `ClimateWidget`, `MotionWidget`, `ContactWidget`, `ThermostatWidget`, `ButtonWidget`, `PresenceWidget` — each takes `{ entity, onOpen, onToggle? }` and renders the card's body.
  - `widgetFor(entity): 'plug' | 'lamp' | 'colorLamp' | 'climate' | 'motion' | 'contact' | 'thermostat' | 'button' | 'presence' | 'generic'` — the chooser, exported and unit‑tested.
  - `batteryOf(entity): number | null`, `powerOf(entity): number | null` — the readings the widgets show.

- [ ] **Step 1: Write the failing test** — `widgetFor` maps the device types right, and each widget renders its main thing:
  - a plug shows its watts («38 W now»);
  - a motion sensor shows «Motion detected» / «No motion»;
  - a contact shows «Open» / «Closed»;
  - a thermostat shows the current temperature and the target;
  - a button shows when it was last pressed.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** the widgets per the mockup (`casanomi-widgets-board.html` → *Типы устройств и их плитки*):
  - **plug** — icon tile + an `ON`/`OFF` badge + «38 W now» under the name;
  - **lamp** — icon tile + the brightness percentage on the right (the dimmer card comes in Task 3);
  - **colour lamp** — icon tile + a colour dot (the current `color_xy` as a circle);
  - **climate** — a big reading «23.4° · 58 %» + the battery line;
  - **motion** — «Motion detected · just now»;
  - **contact** — «Open» / «Closed»;
  - **thermostat** — «22.5° → target 24°» plus `−`/`+` steps (the mockup puts the steps in the card's top row);
  - **button** — «Pressed 2 min ago»;
  - **presence (phone)** — the person card: avatar circle (gold ring when home), the name, «Home · since 18:40».

  Keep the existing `model/devices.ts` helpers (`attrOf`, `formatValue`, `primarySwitch`, `widgetValue`) as the source of truth — do not re‑read attributes in the widgets.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/widgets.tsx app/src/ui/DeviceWidget.tsx app/test/ui-widgets.test.tsx
git commit -m "feat(ui): a widget per device type"
```

---

### Task 3: The lamp as a dimmer (card fill = brightness)

**Files:**
- Create: `app/src/ui/Dimmer.tsx`
- Modify: `app/src/ui/widgets.tsx` (`LampWidget` uses it), `app/src/ui/DeviceWidget.tsx`
- Test: `app/test/ui-dimmer.test.tsx` (new)

**Interfaces:**
- Consumes: `brightnessPercent` from `model/format.ts`; the brightness attribute and `commands.send`.
- Produces:
  - `Dimmer({ value, on, onChange, onToggle, onOpen })` — `value` 0–100.
  - the gesture split: a **tap** toggles; a **horizontal swipe** over 12 px sets the brightness; a **long press** opens the sheet. A movement of more than 12 px cancels the tap (so a swipe never toggles).

- [ ] **Step 1: Write the failing test**:
  - the card's fill is `value` % wide;
  - a tap below the threshold calls `onToggle`;
  - a swipe of 30 px calls `onChange` with a new value and **not** `onToggle`;
  - a long press calls `onOpen`.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** `Dimmer`:
  - the fill: an absolutely positioned `View` with `width: `${value}%``, `backgroundColor: 'rgba(207,159,51,0.28)'` → a gradient from `0.28` to `0.12` (RN has no gradient without a library — use two stacked translucent views or a flat wash; **do not add a dependency**).
  - the edge: a 3 px gold line at the fill's right edge.
  - the grip: the percentage in gold ink on the right.
  - the gesture: `onStartShouldSetResponder` + `onResponderMove` tracking `dx`; when `|dx| > 12` the gesture becomes a drag and the tap is cancelled; `onResponderRelease` maps the drag to a new value (`start + dx / width * 100`), clamped 0–100, and sends it.
  - the long press: RN's `onLongPress` on the inner `Pressable`, with the drag cancelling it (the existing `usePressPhase` helper in `DeviceWidget.tsx` shows the pattern).
  - the colour dot (for colour lamps): a small circle in the corner; tapping it opens the colour sheet.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/Dimmer.tsx app/src/ui/widgets.tsx app/src/ui/DeviceWidget.tsx app/test/ui-dimmer.test.tsx
git commit -m "feat(ui): the lamp card is its own dimmer"
```

---

### Task 4: The thermostat ring, and the readings grid

**Files:**
- Create: `app/src/ui/ThermostatRing.tsx`
- Create: `app/src/ui/ReadingsGrid.tsx`
- Modify: `app/src/ui/widgets.tsx`
- Test: `app/test/ui-widgets.test.tsx` (extend)

**Interfaces:**
- Produces:
  - `ThermostatRing({ current, target, onTarget })` — a ring drawn with `react-native-svg` (already a dependency): the arc from the min to the current in gold, the rest in the track colour, the current in the middle and «target 24°» under it. Tapping the ring does not change anything; the `−`/`+` steps next to it do.
  - `ReadingsGrid({ readings })` — a 2×2 grid of small cells (label + value + unit), each cell coloured by its threshold: green under the threshold, amber over it. The thresholds live in `model/format.ts` or a new `model/thresholds.ts`.

- [ ] **Step 1: Write the failing test** — the ring renders the current and the target; the readings grid renders four cells; a cell over its threshold carries the amber token.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** — per the *непростые виджеты* board:
  - the ring: `react-native-svg`'s `Circle` with `strokeDasharray` for the gold arc; the inner circle is the card colour.
  - the grid: `flexDirection: 'row'`, `flexWrap: 'wrap'`, each cell `flexBasis: '48%'`.
  - the thresholds: temperature (normal 18–26 °C), humidity (30–70 %), CO₂ (0–1200 ppm), PM2.5 (0–35 µg/m³) — a new `model/thresholds.ts` with `toneFor(name, value): 'good' | 'warn' | 'bad'`.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/ThermostatRing.tsx app/src/ui/ReadingsGrid.tsx app/src/ui/widgets.tsx app/src/model/thresholds.ts app/test
git commit -m "feat(ui): thermostat ring and a readings grid"
```

---

### Task 5: The device sheet in the casanomi language

**Files:**
- Modify: `app/src/ui/DeviceSheet.tsx`
- Test: `app/test/ui-devices.test.tsx` (extend)

**Interfaces:**
- The sheet keeps its props; the body changes:
  - a header row: the icon tile + the name + «Living room · On»;
  - **Brightness**: the mockup's slider (reuse `src/ui/Slider.tsx`);
  - a `Segmented` **Color / White · warm–cool** for colour lamps (the `Segmented` control arrives in Plan 2);
  - a row of colour presets (the mockup's six circles);
  - a **menu** (rows with an icon): *Rename*, *Move to another room*, *Blink (find it)*, *Remove device* (the last one in red).

- [ ] **Step 1: Write the failing test** — the sheet shows the brightness slider for a lamp, the rename/move/blink/remove rows, and *Remove device* asks first.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** — reuse the existing handlers (rename, move, blink, remove are already in `DeviceSheet.tsx`); restyle them into the menu rows. Do not change the protocol calls.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/DeviceSheet.tsx app/test/ui-devices.test.tsx
git commit -m "feat(ui): the device sheet in the casanomi language"
```

---

## Notes for the executor

- `model/grid.ts` (`deviceShape`, `gridLayout`) becomes unused when the cards replace the tile grid — **delete it** and its test, or keep it if another screen still uses it. Check first.
- `react-native-svg` is already a dependency and is stubbed in the vitest config (`test/stubs/react-native-svg.tsx`) — the ring must still render in tests, so the stub has to draw *something* the test can see (the stub is checked in Task 4).
- No new dependencies: the gold wash is flat translucent views, not a gradient library.
- The mockup's «card» for a lamp is *wide* (a row), while a sensor's card shows its numbers *large*. Keep the sensor's big number — it is the point of the tile.
- After this plan the app must still work end to end against `npm run mock-hub`.
