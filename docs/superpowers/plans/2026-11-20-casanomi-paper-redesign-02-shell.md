# Casanomi‑paper redesign — Plan 2: Shell & Settings

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The app gets one dock with three tabs — **Home · Automations · Settings** — and a round ink **«+»** button next to it. Hub and History move *inside* Settings as sub‑screens. Settings gains a **Display** sub‑screen (theme / °C‑°F / home‑screen options) and a **Plugins** skeleton.

**Architecture:** The tab strip lives in `Home.tsx` today. We replace its four tabs with three, move the two moved screens (`HubScreen`, `HistoryScreen`) behind a Settings index, and add a small navigation stack (`useNavigation`-like local state) for sub‑screens. The «+» button opens a sheet with three branches (device · scene · automation) — the branches themselves arrive in Plan 4, so here the sheet's *entry points* are wired and the destination is a stub that Plan 4 fills.

**Tech Stack:** Expo 57 / RN 0.86 + react‑native‑web, TypeScript, vitest + @testing-library/react in jsdom.

**Spec:** `docs/superpowers/specs/2026-11-20-casanomi-paper-redesign-design.md`
**Mockups:** `casanomi-screens-board.html` (screens 2, 3, 4), `casanomi-tweaks.html` (Settings, Display)

## Global Constraints

- No firmware, protocol, or data‑model changes. `app/` only.
- Interface language stays **English**.
- The plan‑1 theme system is the palette: `THEMES.light` / `THEMES.dark`, `primary`/`onPrimary` for the ink action, gold only as decoration.
- The dock is a floating **pill** on every platform now (not a full‑width bar on Android/web) — the mockup shows one pill everywhere, with the «+» circle beside it.
- New user‑visible strings are English.
- `expo export --platform web` must bundle; `tsc --noEmit` clean; the vitest suite stays green (existing tests updated **on purpose** where the tab names changed).

## Review Focus

- **The dock has exactly three tabs and a separate «+»** — the FAB is not a fourth tab, and it is a *separate circle*, not inside the pill. → Task 1 asserts the labels and the separate button.
- **Hub and History are reachable from Settings, not from the dock.** → Task 2 asserts the Settings rows exist and open the right screen.
- **The theme picker moves from the Hub tab to Settings → Display.** → Task 3 asserts the Display screen owns it.
- **A sub‑screen has a way back.** → Task 2/3 assert a back control labelled «Settings» / «Back».
- **Nothing breaks when a setting is absent** (no hub, no temperature preference stored). → Task 3 asserts defaults.

---

### Task 1: The three‑tab dock and the «+» button

**Files:**
- Modify: `app/src/ui/Home.tsx` (tabs, dock, FAB)
- Modify: `app/src/ui/Glyph.tsx` (add the glyphs the new UI needs)
- Test: `app/test/ui-hub.test.tsx` (tab names change) and a new `app/test/ui-shell.test.tsx`

**Interfaces:**
- Consumes: `useTheme()` (`t.primary`, `t.onPrimary`, `t.accent`, `t.card`, `t.border`).
- Produces:
  - `type Tab = 'home' | 'automations' | 'settings'`
  - `HomeBody` props gain `onAdd?(): void` — the FAB's callback.
  - `HomeBody` renders `<Dock tab onTab onAdd />` (exported for tests).
  - **New glyphs** in `Glyph.tsx` (the union and the `icon()` switch together — the switch is exhaustive):
    - `settings` — the gear (the Settings tab; the mockup uses a sun‑in‑a‑circle, a gear reads better as a tab).
    - `chevronRight` — the `›` in the settings rows.
    - `trash` — the *Remove device* row.
    - `wifi` — the Wi‑Fi settings row (the closest existing glyph is `signal`, which is used for the rescan button — a separate `wifi` is cleaner).
    - `timer` — the *Timer* branch of the automation chooser.
    - `schedule` — a clock for the *Schedule* branch (`history` is a clock‑in‑a‑circle, but it means "history" in the tab bar; a plain clock is distinct).
    - `rule` — the sliders/pencil for the *Rule* branch (`pencil` exists but means "edit" elsewhere).
    - `scene` — the sparkle/star for the *Scene* branch.
    - `room` — the room chip / *Move to another room* row.
    - `qr` — the *Scan a QR code* button.
    - `bluetooth` / `matter` are **deliberately not added** — the transport is never shown to the user.

- [ ] **Step 1: Write the failing test** in `app/test/ui-shell.test.tsx`:

```tsx
// @vitest-environment jsdom
import './setup-dom';
import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { Dock } from '../src/ui/Home';

afterEach(cleanup);

describe('the dock', () => {
  it('has three tabs and a separate add button', () => {
    render(<Dock tab="home" onTab={() => {}} onAdd={() => {}} />);
    expect(screen.getByLabelText('Home')).toBeTruthy();
    expect(screen.getByLabelText('Automations')).toBeTruthy();
    expect(screen.getByLabelText('Settings')).toBeTruthy();
    expect(screen.queryByLabelText('Hub')).toBeNull();
    expect(screen.queryByLabelText('History')).toBeNull();
    // The add button stands beside the pill, not in it.
    expect(screen.getByLabelText('Add')).toBeTruthy();
  });
});
```

- [ ] **Step 2: Run, verify fail** — `cd app && npx vitest run test/ui-shell.test.tsx` → FAIL (`Dock` not exported).

- [ ] **Step 3: Implement** — extract the tab bar out of `Home.tsx` into an exported `Dock` component:
  - pill container: `backgroundColor: t.card` (light) / `t.card` (dark), `borderColor: t.border`, `borderRadius: 999`, height 58, `marginHorizontal: 14`, `bottom: 16`, absolute.
  - three `TabButton`s: label 10 px, glyph 21, active colour `t.accent` (gold is decoration), inactive `t.muted`.
  - the FAB: a separate 54×54 circle, `backgroundColor: t.primary`, label colour `t.onPrimary`, `accessibilityLabel="Add"`, sitting to the right of the pill with a 9 px gap.
  - the whole dock sits in a row: `<View style={styles.dock}>` with the pill `flex: 1` and the FAB fixed.
  - `Tab` becomes `'home' | 'automations' | 'settings'`; `HomeBody` keeps `finding` as an overlay (the Find‑devices screen still covers the tabs).
  - `HubScreen` and `HistoryScreen` are no longer tabs — they are rendered by the Settings screen (Task 2). For Task 1 the Settings tab renders a placeholder that Task 2 replaces; **do not delete the imports yet**, keep them referenced from `SettingsScreen` stub to avoid unused‑import lint noise.

- [ ] **Step 4: Run, verify pass** — `npx vitest run test/ui-shell.test.tsx`.

- [ ] **Step 5: Update the existing tests** that clicked the old tabs:
  - `test/ui-hub.test.tsx` — `click('Hub')` no longer exists. Until Task 2 lands, route the test through Settings → Hub & health.
  - `test/ui-automation.test.tsx`, `test/ui-visual.test.tsx`, `test/ui-devices.test.tsx`, `test/ui-history.test.tsx` — check for `click('Hub')` / `click('History')` and re‑route.
  - Run the whole suite and make it green.

- [ ] **Step 6: Commit**

```bash
git add app/src/ui/Home.tsx app/src/ui/Glyph.tsx app/test/ui-shell.test.tsx app/test
git commit -m "feat(ui): three-tab dock with a separate add button"
```

---

### Task 2: Settings index — Hub, History, Wi‑Fi, Remote access

**Files:**
- Create: `app/src/ui/SettingsScreen.tsx`
- Modify: `app/src/ui/Home.tsx` (the Settings tab renders `SettingsScreen`)
- Modify: `app/src/ui/HubScreen.tsx` (it becomes a sub‑screen: add a `title`/back header)
- Test: `app/test/ui-settings.test.tsx` (new)

**Interfaces:**
- Produces:
  - `SettingsScreen({ home, url, onChangeHub, commands, client, onTokenChange, onUseRelay, mode, onMode, onOpenDisplay, onOpenPlugins })`
  - A `SettingsRow({ glyph, label, value, onPress, tone })` shared row: icon + name + optional right‑hand value + chevron.

- [ ] **Step 1: Write the failing test** `app/test/ui-settings.test.tsx`:

```tsx
// @vitest-environment jsdom
import './setup-dom';
import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { SettingsScreen } from '../src/ui/SettingsScreen';

afterEach(cleanup);

describe('settings', () => {
  it('lists the hub, history, wifi and remote access', () => {
    render(<SettingsScreen {...minimalProps} />);
    expect(screen.getByLabelText('Hub & health')).toBeTruthy();
    expect(screen.getByLabelText('History')).toBeTruthy();
    expect(screen.getByLabelText('Wi-Fi')).toBeTruthy();
    expect(screen.getByLabelText('Remote access')).toBeTruthy();
    // The theme picker no longer lives on the Hub tab.
    expect(screen.queryByLabelText('Appearance')).toBeNull();
  });
});
```

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** `SettingsScreen`:
  - top: a hub summary card (icon tile + hub name + `Connected · firmware <fw>` with a green dot) — reuse `home.hub`.
  - an `App` section: a **Display** row (value = current theme name, e.g. `Light · °C`).
  - a `Hub` section: rows **Hub & health** (value `2 chips ok`), **History**, **Wi‑Fi** (value = the SSID if `home.hub` has one, else none), **Remote access**.
  - a `Device` section: **Firmware updates**, **Location**.
  - the rows open sub‑screens: Task 2 wires **Hub & health → `HubScreen`** and **History → `HistoryScreen`**; the rest show a "not here yet" note until later plans.
  - `HomeBody` keeps a sub‑screen stack: `const [sub, setSub] = useState<'none'|'hub'|'history'|'display'|'plugins'>('none')`. When `sub !== 'none'` the sub‑screen replaces the tab content and the dock is hidden.

- [ ] **Step 4: Give `HubScreen` and `HistoryScreen` a back header** — `ScreenHeader` with `left={<HeaderButton label="Settings" .../>}`; `HomeBody` passes an `onBack` callback.

- [ ] **Step 5: Run, verify pass** + the whole suite.

- [ ] **Step 6: Commit**

```bash
git add app/src/ui/SettingsScreen.tsx app/src/ui/Home.tsx app/src/ui/HubScreen.tsx app/src/ui/HistoryScreen.tsx app/test/ui-settings.test.tsx
git commit -m "feat(ui): Settings index; Hub and History move inside it"
```

---

### Task 3: Display sub‑screen (theme, °C/°F, home‑screen options)

**Files:**
- Create: `app/src/ui/DisplayScreen.tsx`
- Modify: `app/src/ui/SettingsScreen.tsx` (open it)
- Modify: `app/src/palette.ts` — add the app‑preferences store (temperature unit, home‑screen flags)
- Test: `app/test/ui-display.test.tsx` (new), `app/test/palette.test.ts` (extend)

**Interfaces:**
- Produces:
  - `app/src/palette.ts`: `createPrefsStore(kv)` → `{ load(): Promise<Prefs>; save(p: Partial<Prefs>): Promise<void> }`, `Prefs = { temperature: 'C' | 'F'; showScenes: boolean; groupByRoom: boolean }`, defaults `{ temperature: 'C', showScenes: true, groupByRoom: true }`, KEY `keystone.prefs`, unknown/old values → defaults, throwing store → defaults.
  - `app/src/model/temperature.ts`: the unit preference as a **model** concern, not a screen concern:
    - `type TemperatureUnit = 'C' | 'F'`;
    - `cToF(c: number): number`, `fToC(f: number): number`;
    - `formatTemperature(celsius: number, unit: TemperatureUnit): string` — «23.4 °C» / «74.1 °F»;
    - `setpointLabel(celsius: number, unit: TemperatureUnit): string`.
  - `formatValue(attr, value)` in `model/format.ts` gains an **optional** unit argument: `formatValue(a, v, unit?: TemperatureUnit)` — the default stays °C so every existing call site and test keeps working.
  - `DisplayScreen({ prefs, onPrefs, mode, onMode, onBack })`.
  - A shared `Segmented({ options, value, onChange })` control (the mockup's `.seg`).

- [ ] **Step 1: Write the failing tests**:
  - `test/palette.test.ts`: the prefs store round‑trips, and an old/unknown value falls back to the defaults.
  - `test/temperature.test.ts` (new): `cToF`/`fToC` round‑trip; `formatTemperature` renders both units; `formatValue` with the `F` unit renders the temperature in °F and leaves the other attributes alone.
  - `test/ui-display.test.tsx`: renders the three theme options, the °C/°F segment, and the two switches; tapping `°F` calls `onPrefs({ temperature: 'F' })`; tapping the `Dark` theme calls `onMode('dark')`.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement**:
  - `app/src/palette.ts`: `createPrefsStore` exactly as above.
  - `app/src/model/temperature.ts`: the conversions. The wire is always 0.1 °C (`i16`) — **the unit only changes how it is written**, never what goes on the wire.
  - `app/src/model/format.ts`: `formatValue` takes the optional unit; `temperature` and `setpoint` use it. The other attributes (humidity %, brightness %, kelvin) are untouched.
  - `app/src/model/automation.ts`: `SCALES` is **wire‑bound** (it converts what the user types into the wire value) — leave it alone. The unit preference changes the *label* of the field and the value shown, so `inputText`/`parseInput` need the unit too: give them an optional unit parameter as well, defaulting to °C.
  - `app/src/ui/controls.tsx`: add `Segmented` — a rounded track (`t.fill` background, 3 px padding) with the selected segment on a `t.card` pill. The mockup uses `#fff`; `t.card` is the theme's equivalent.
  - `app/src/ui/DisplayScreen.tsx`:
    - `Theme` label + a `Segmented` of System/Light/Dark, then **two theme preview cards** (light + dark) as in the mockup — the preview shows a small mock of the palette (page colour, a gold line, a card line).
    - `Temperature` label + `Segmented` of °C/°F.
    - `Home screen` label + two switch rows: *Show scenes on Home* (hint «A quick row above the rooms»), *Group by room* (hint «Off = one flat list»).
    - a `Switch` control (a 46×28 track with a 22 px knob) — add it to `controls.tsx`.
  - `Home.tsx` owns the prefs state (load on mount, save on change) and passes it down. **The unit must reach the screens that show a temperature** — `DeviceWidget`/`widgets.tsx` (Plan 3) read it from the same store.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/palette.ts app/src/ui/controls.tsx app/src/ui/DisplayScreen.tsx app/src/ui/SettingsScreen.tsx app/src/ui/Home.tsx app/test
git commit -m "feat(ui): Display screen — theme, temperature unit, home-screen options"
```

---

### Task 4: The «+» menu (Add → device · scene · automation)

**Files:**
- Create: `app/src/ui/AddMenu.tsx`
- Modify: `app/src/ui/Home.tsx` (the FAB opens it)
- Test: `app/test/ui-add-menu.test.tsx` (new)

**Interfaces:**
- Produces: `AddMenu({ onClose, onDevice, onScene, onAutomation })` — a bottom sheet with three rows, each with an icon tile, a name and a hint:
  - **Device** — «Find nearby or pair Zigbee / Matter»
  - **Scene** — «Save what the devices are doing now»
  - **Automation** — «A schedule, a timer, or a rule»

- [ ] **Step 1: Write the failing test** — the menu shows the three rows; tapping *Device* calls `onDevice`; tapping *Scene* calls `onScene`; tapping *Automation* calls `onAutomation`.

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** — reuse `Sheet.tsx`'s sheet (`Sheet` already exists: check its props and use it rather than a new one). `onDevice` opens the existing Find‑devices screen; `onScene` and `onAutomation` show a «coming in the next plan» note (Plan 4 fills them) — but they must be *reachable*, not dead ends.

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/AddMenu.tsx app/src/ui/Home.tsx app/test/ui-add-menu.test.tsx
git commit -m "feat(ui): the add menu — device, scene, automation"
```

---

### Task 5: Plugins skeleton (Browse / Installed)

**Files:**
- Create: `app/src/ui/PluginsScreen.tsx`
- Modify: `app/src/ui/SettingsScreen.tsx` (a *Plugins* row under a *More* section)
- Test: `app/test/ui-plugins.test.tsx` (new)

**Interfaces:**
- Produces: `PluginsScreen({ onBack })` — two tabs **Browse** / **Installed**, a search field, a one‑line scrolling tag row (All / Devices / Energy / Voice / Dashboards / Automations / Security), and a card grid of plugins with an *Install* button. The data is a **static local list** — the hub has no plugin mechanism yet, and the spec says this screen is a shell.

- [ ] **Step 1: Write the failing test** — the two tabs are present; *Browse* shows plugin cards with an *Install* button; tapping *Installed* shows an empty state («No plugins yet»).

- [ ] **Step 2: Run, verify fail.**

- [ ] **Step 3: Implement** — a local `PLUGINS` array (8–10 entries: name, author, description, rating, installs, tags, price badge). The *Install* button is local state only (it moves the card to *Installed* in the session, nothing is sent to the hub).

- [ ] **Step 4: Run, verify pass** + the whole suite.

- [ ] **Step 5: Commit**

```bash
git add app/src/ui/PluginsScreen.tsx app/src/ui/SettingsScreen.tsx app/test/ui-plugins.test.tsx
git commit -m "feat(ui): plugins store skeleton — Browse and Installed"
```

---

## Notes for the executor

- `ScreenHeader` already gives a centred title with left/right slots — reuse it, do not build a second header bar.
- The `Sheet` component already exists (`src/ui/Sheet.tsx`) — the Add menu and the new sheets must use it.
- The `ConfirmDialog` in `Sheet.tsx` is the delete confirmation; leave it alone.
- The dock's FAB must be a separate `Pressable`, **not** inside the pill's flex row, or the three tabs will not be equal width.
- Do not delete `HubScreen`/`HistoryScreen` — they move, they do not go away.
- After this plan the app must still work end to end against `npm run mock-hub`.
