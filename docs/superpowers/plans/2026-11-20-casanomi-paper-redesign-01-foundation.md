# Casanomi‑paper redesign — Plan 1: Foundation (theme system) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the app one coherent casanomi‑paper look in two themes — a warm light "paper" and a warm dark — selectable as System / Light / Dark, with Geist as the type, so every existing screen is restyled by its tokens without changing behaviour.

**Architecture:** The app already themes through a single `Theme` token object consumed by `useTheme()`; screens read tokens, they do not hard‑code colours. We replace the three palettes (`sandstone`/`ember`/`paper`) with two casanomi‑paper `Theme` objects (`paperLight`, `paperDark`) and swap the "which palette" setting for a "which mode" setting (`system|light|dark`). We add two tokens (`primary`/`onPrimary`) so the primary button is near‑black ink (casanomi) instead of the gold accent, and load Geist through `expo-font`. No screen logic changes; only tokens, the primary button, the theme store, and the thin wiring in `App.tsx`/`Home.tsx`/`HubScreen.tsx`.

**Tech Stack:** Expo 57 / React Native 0.86 + react‑native‑web, TypeScript, vitest (+ @testing-library/react in jsdom), `expo-font` + `@expo-google-fonts/geist`.

**Spec:** `docs/superpowers/specs/2026-11-20-casanomi-paper-redesign-design.md`
**Status:** **сделано 20.11** — коммиты `b8fab28`, `12be017`, `f69e6aa`, `8d2ac99`, `2ba5d01`.
Из плана изменено осознанно: базовый шрифт сделан через `src/ui/kit.tsx`
(обёртка `Text`/`TextInput`), а не через `defaultProps` — React **заменяет**
`style` из `defaultProps`, а не сливает его, и у 252 из 261 текстов есть свой
`style`. Поэтому Geist не дошёл бы дальше девяти текстов.

## Global Constraints

- No firmware, protocol, or data‑model changes. `app/` only.
- Interface language stays **English**; no user‑visible strings are translated.
- Two themes only, in one casanomi language; `sandstone` and `ember` are removed.
- Palette tokens (verbatim from the spec, light theme):
  page `#f4efe4`, card `#fffdf8`, line `#e8e1d0`, ink/text `#1a1813`,
  muted `#746d5d`, accent (gold) `#cf9f33`, accentInk `#9a7117`,
  accentSoft `rgba(207,159,51,0.16)`, good `#2f7d4f`, cardRadius `22`.
- Dark theme is **warm**, not grey: ground `#1c1813`, cards a touch lighter,
  accent gold `#e0b23f`. Exact secondary tokens are derived by the builder to
  keep text/line contrast ≥ WCAG AA on both themes.
- The **primary** action is near‑black ink (`primary`), not the gold accent;
  gold is decoration only.
- `expo export --platform web` must still bundle; `tsc --noEmit` clean
  (the pre‑existing `expo-camera` type error is the only allowed exception);
  existing vitest suite stays green except where a test is updated here on purpose.
- Tests render real components (react‑native‑web in jsdom). Colour assertions
  read tokens from `THEMES`; they must use the new token names.

## Review Focus

- **System mode follows the OS and reacts to a change** — `mode: 'system'` must
  resolve to dark when `useColorScheme()` is `'dark'` and light otherwise.
  → Task 1 tests both resolutions.
- **An old or unreadable stored value must not blank the app** — a previously
  saved `'sandstone'`/`'ember'`/`'paper'`, an unknown string, or a throwing
  store all fall back to `'system'`. → Task 2 tests migration + throw.
- **Dark‑theme legibility** — text on card and the hairline must stay visible in
  the warm dark theme (not ink‑on‑ink). → Task 1 asserts dark `text` is light
  and `card` ≠ `text`.
- **Fonts failing to load must not block the UI** — if Geist is not ready the
  app still renders (system fallback), never a permanently blank screen.
  → Task 4 gates on a ready flag with a fallback, verified by a render smoke.
- **The primary button is ink, every kind still legible** — primary bg =
  `primary`, fg = `onPrimary`; secondary/plain/danger unchanged.
  → Task 3 asserts the resolved colours.

---

### Task 1: Two casanomi‑paper themes + mode resolver

**Files:**
- Modify: `app/src/ui/theme.tsx` (replace palettes & provider; keep `Theme`
  interface, `useTheme`, `radius`, `space`)
- Test: `app/test/theme.test.tsx` (rewrite)

**Interfaces:**
- Produces:
  - `type ThemeMode = 'system' | 'light' | 'dark'`
  - `interface Theme` — same as today **plus** `primary: string` and
    `onPrimary: string`; `cardRadius` now `22` for both.
  - `const THEMES: Record<'light' | 'dark', Theme>` — `{ light: paperLight, dark: paperDark }`
  - `const MODES: { id: ThemeMode; name: string }[]` — System / Light / Dark, for a picker
  - `function ThemeProvider({ mode, children }: { mode: ThemeMode; children: ReactNode })`
  - `function useTheme(): Theme` (unchanged), `radius` (with `card: 22`), `space` (unchanged)
- Consumes: `useColorScheme` from `react-native`.

- [x] **Step 1: Write the failing tests** in `app/test/theme.test.tsx` (replace the file)

```tsx
// @vitest-environment jsdom
// mock react-native useColorScheme per-test via vi.mock as today.
describe('casanomi-paper themes', () => {
  it('light wears the spec colours', () => {
    const l = THEMES.light;
    expect(l.bg).toBe('#f4efe4');
    expect(l.card).toBe('#fffdf8');
    expect(l.border).toBe('#e8e1d0');
    expect(l.text).toBe('#1a1813');
    expect(l.accent).toBe('#cf9f33');       // gold, not terracotta
    expect(l.good).toBe('#2f7d4f');
    expect(l.cardRadius).toBe(22);
  });
  it('primary action is ink, not the accent', () => {
    expect(THEMES.light.primary).toBe('#1a1813');
    expect(THEMES.light.accent).not.toBe(THEMES.light.primary);
  });
  it('dark is warm and legible', () => {
    const d = THEMES.dark;
    expect(d.bg).toBe('#1c1813');
    expect(d.accent).toBe('#e0b23f');
    expect(d.text.toLowerCase()).not.toBe(d.card.toLowerCase()); // not ink-on-ink
  });
  it('system follows the OS: dark', () => {            // useColorScheme → 'dark'
    render(<ThemeProvider mode="system"><Probe/></ThemeProvider>);
    expect(screen.getByTestId('probe').textContent).toContain(THEMES.dark.bg);
  });
  it('light mode stays light even when the OS is dark', () => {
    render(<ThemeProvider mode="light"><Probe/></ThemeProvider>);
    expect(screen.getByTestId('probe').textContent).toContain(THEMES.light.bg);
  });
});
```

(`Probe` renders `${t.bg}` into a `testID="probe"` Text, as in the old file.)

- [x] **Step 2: Run the tests, verify they fail**

Run: `cd app && npx vitest run test/theme.test.tsx`
Expected: FAIL — `ThemeMode`/`MODES`/`primary` not defined, old palette gone.

- [x] **Step 3: Rewrite `theme.tsx`**

Define `paperLight` and `paperDark` with the Global‑Constraints values; add
`primary`/`onPrimary` to the `Theme` interface (`primary` = ink `#1a1813` on
light, a warm near‑white ink surface on dark; `onPrimary` = the cream
`#fdf7ea` on light). Derive the remaining secondary/glass/good tokens from the
spec in the casanomi language (gold accentSoft `rgba(207,159,51,0.16)`, quiet
button = outline on light / filled on dark, as today). Export `ThemeMode`,
`MODES`, `THEMES` keyed `light|dark`, and `ThemeProvider({ mode })` resolving
`mode === 'system' ? (useColorScheme()==='dark'?paperDark:paperLight) : mode==='dark'?paperDark:paperLight`. Set `radius.card = 22`.

- [x] **Step 4: Run the tests, verify they pass**

Run: `cd app && npx vitest run test/theme.test.tsx`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add app/src/ui/theme.tsx app/test/theme.test.tsx
git commit -m "feat(ui): casanomi-paper light+dark themes with System/Light/Dark mode"
```

---

### Task 2: Theme‑mode setting (replace palette store)

**Files:**
- Modify: `app/src/palette.ts` (becomes the theme‑mode store)
- Test: `app/test/palette.test.ts` (rewrite)

**Interfaces:**
- Consumes: `ThemeMode` from `./ui/theme`, `KeyValueStore` from `./settings`.
- Produces:
  - `function isThemeMode(v: unknown): v is ThemeMode`
  - `function createThemeStore(kv: KeyValueStore): { load(): Promise<ThemeMode>; save(m: ThemeMode): Promise<void> }`
  - KEY `'keystone.theme'`, default `'system'`.

- [x] **Step 1: Write the failing tests** in `app/test/palette.test.ts` (replace)

```ts
describe('theme mode', () => {
  it('knows its names', () => {
    expect(isThemeMode('system')).toBe(true);
    expect(isThemeMode('light')).toBe(true);
    expect(isThemeMode('dark')).toBe(true);
    expect(isThemeMode('ember')).toBe(false);
  });
  it('remembers the choice under keystone.theme', async () => {
    const kv = memoryStore();
    const s = createThemeStore(kv);
    expect(await s.load()).toBe('system');
    await s.save('dark');
    expect(await s.load()).toBe('dark');
    expect(kv.data.get('keystone.theme')).toBe('dark');
  });
  it('an old palette value or a throwing store falls back to system', async () => {
    expect(await createThemeStore(memoryStore({ 'keystone.theme': 'sandstone' })).load()).toBe('system');
    const broken = { async get(){ throw new Error('x'); }, async set(){}, async delete(){} };
    expect(await createThemeStore(broken).load()).toBe('system');
  });
});
```

- [x] **Step 2: Run, verify fail**

Run: `cd app && npx vitest run test/palette.test.ts`
Expected: FAIL — `isThemeMode`/`createThemeStore` not defined.

- [x] **Step 3: Rewrite `palette.ts`** with `isThemeMode`, `createThemeStore`,
  KEY `keystone.theme`, default `'system'`, try/catch → `'system'`.

- [x] **Step 4: Run, verify pass**

Run: `cd app && npx vitest run test/palette.test.ts`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add app/src/palette.ts app/test/palette.test.ts
git commit -m "feat(ui): store theme mode (system/light/dark) in place of palette id"
```

---

### Task 3: Primary button = ink

**Files:**
- Modify: `app/src/ui/controls.tsx` (`Button` colour resolution)
- Test: `app/test/ui-controls.test.tsx` (create if absent, else add a case)

**Interfaces:**
- Consumes: `Theme.primary`, `Theme.onPrimary` (Task 1).
- Produces: no signature change — `Button` renders primary with ink bg.

- [x] **Step 1: Write the failing test** — render `<Button label="Go" kind="primary" .../>` inside `ThemeProvider mode="light"`, assert the pressable's resolved `backgroundColor` is `THEMES.light.primary` and the label colour is `THEMES.light.onPrimary`. Add one case asserting `kind="secondary"` still uses `secondaryBg`/`text`.

- [x] **Step 2: Run, verify fail**

Run: `cd app && npx vitest run test/ui-controls.test.tsx`
Expected: FAIL — bg currently resolves to `t.accent`.

- [x] **Step 3: Change `Button`** in `controls.tsx`: `const bg = kind === 'primary' ? t.primary : kind === 'secondary' ? t.secondaryBg : 'transparent';` and `const fg = kind === 'primary' ? t.onPrimary : …` (rest unchanged).

- [x] **Step 4: Run, verify pass** — same command, Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add app/src/ui/controls.tsx app/test/ui-controls.test.tsx
git commit -m "feat(ui): primary button uses ink, not the gold accent"
```

---

### Task 4: Load Geist, app‑wide base type

**Files:**
- Modify: `app/package.json` (deps), `app/App.tsx` (load fonts, gate render,
  set base family)
- Create: `app/src/ui/fonts.ts` (font map + a `applyBaseFont()` helper)
- Test: `app/test/fonts.test.ts`

**Interfaces:**
- Produces:
  - `app/src/ui/fonts.ts`: `const GEIST_FONTS` (the `useFonts` map:
    `Geist_400Regular`, `Geist_500Medium`, `Geist_600SemiBold`,
    `Geist_700Bold`, `GeistMono_500Medium`), and `function familyForWeight(w?: string): string`
    mapping a RN `fontWeight` to the right Geist family, system fallback for unknown.
- Consumes: `expo-font` `useFonts`, `@expo-google-fonts/geist`.

- [x] **Step 1: Write the failing test** `app/test/fonts.test.ts`

```ts
describe('geist fonts', () => {
  it('maps weights to Geist families and falls back', () => {
    expect(familyForWeight('700')).toBe('Geist_700Bold');
    expect(familyForWeight('600')).toBe('Geist_600SemiBold');
    expect(familyForWeight(undefined)).toBe('Geist_400Regular');
  });
  it('exposes the four weights + mono to useFonts', () => {
    expect(Object.keys(GEIST_FONTS)).toEqual(
      expect.arrayContaining(['Geist_400Regular','Geist_600SemiBold','Geist_700Bold','GeistMono_500Medium']),
    );
  });
});
```

- [x] **Step 2: Run, verify fail**

Run: `cd app && npx vitest run test/fonts.test.ts`
Expected: FAIL — module/exports missing.

- [x] **Step 3: Install deps and implement**

```bash
cd app && npx expo install expo-font @expo-google-fonts/geist
```
Create `app/src/ui/fonts.ts` with `GEIST_FONTS` and `familyForWeight`. In
`App.tsx`: call `useFonts(GEIST_FONTS)`; while not loaded render the existing
`<Loading/>`/null gate (never block permanently — a fallback timer or the
`error` from `useFonts` lets the app render with the system font). Apply Geist
as the base by setting `(Text as any).defaultProps` / `(TextInput as any).defaultProps`
style `{ fontFamily: 'Geist_400Regular' }` once at module load in `fonts.ts`’
`applyBaseFont()`, called from `App.tsx`. Keep `Menlo`/monospace where
`ConnectScreen`/`SetupScreen` already set a mono family.

- [x] **Step 4: Verify** — fonts unit test passes, and the app renders with
  fonts not yet ready.

Run: `cd app && npx vitest run test/fonts.test.ts && npx tsc --noEmit`
Expected: fonts test PASS; tsc clean except the known `expo-camera` error.

- [x] **Step 5: Commit**

```bash
git add app/package.json app/package-lock.json app/src/ui/fonts.ts app/App.tsx app/test/fonts.test.ts
git commit -m "feat(ui): load Geist and use it as the app's base type"
```

---

### Task 5: Rewire mode through App / Home / HubScreen; drop palette picker UI

**Files:**
- Modify: `app/App.tsx`, `app/src/ui/Home.tsx`, `app/src/ui/HubScreen.tsx`
- Test: existing `app/test/ui-hub.test.tsx` (update the appearance case)

**Interfaces:**
- Consumes: `ThemeMode`, `MODES`, `THEMES` (Task 1); `createThemeStore` (Task 2).
- Produces: `Home`/`HubScreen`/`HomeBody` props rename `palette → mode: ThemeMode`,
  `onPalette → onMode?(m: ThemeMode): void`. `AppearanceCard` renders the three
  `MODES` (System/Light/Dark) with `THEMES.light` / `THEMES.dark` preview swatches.

- [x] **Step 1: Update the failing test** — in `app/test/ui-hub.test.tsx`, the
  appearance test renders `HubScreen` with `mode="system"` and asserts the three
  options **System / Light / Dark** are present and tapping "Dark" calls `onMode('dark')`.

- [x] **Step 2: Run, verify fail**

Run: `cd app && npx vitest run test/ui-hub.test.tsx`
Expected: FAIL — props/options still the old palette names.

- [x] **Step 3: Rewire**
  - `App.tsx`: `createThemeStore`, `const [mode, setModeState] = useState<ThemeMode>('system')`,
    `setMode` saves + sets, `<ThemeProvider mode={mode}>`, pass `mode`/`onMode` down.
  - `Home.tsx`: rename the two props and the pass‑through to `HomeBody`/`HubScreen`.
  - `HubScreen.tsx`: `AppearanceCard({ mode, onMode })` maps `MODES`; the System
    row shows both swatches (light+dark), Light/Dark show their own. (This card
    moves to a Display sub‑screen in Plan 2; here it just works on the Hub tab.)

- [x] **Step 4: Run the whole suite + web bundle**

Run: `cd app && npx vitest run && npx tsc --noEmit && npx expo export --platform web`
Expected: vitest green (one network `tls.test.ts` may fail as before), tsc clean
except known `expo-camera`, web export completes.

- [x] **Step 5: Commit**

```bash
git add app/App.tsx app/src/ui/Home.tsx app/src/ui/HubScreen.tsx app/test/ui-hub.test.tsx
git commit -m "feat(ui): choose System/Light/Dark; remove sandstone/ember palette picker"
```

---

## Notes for the executor

- The `Theme` interface keeps its existing token names so the ~dozen screens
  that read them keep compiling; you are changing **values**, adding two tokens,
  and changing the primary button. Do not rename existing tokens in this plan.
- Grep for stray `'sandstone'|'ember'|'paper'|PaletteId|PALETTES` after Task 5
  (`cd app && grep -rn "PaletteId\|PALETTES\|'sandstone'\|'ember'" src App.tsx`);
  the only matches left should be the migration guard in `palette.ts`.
- Dark‑theme token values are yours to finish from the spec's direction; keep
  contrast AA and the warmth (no pure grey). The light theme values are fixed
  by Global Constraints.
