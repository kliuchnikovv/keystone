// The theme mode is a setting of the phone, not of the hub: it lives in the
// same key-value store as the hub address (secure store on a phone,
// localStorage in a browser), under its own key.
import type { KeyValueStore } from './settings';
import type { ThemeMode } from './ui/theme';

const KEY = 'keystone.theme';

export function isThemeMode(v: unknown): v is ThemeMode {
  return v === 'system' || v === 'light' || v === 'dark';
}

export function createThemeStore(kv: KeyValueStore) {
  return {
    async load(): Promise<ThemeMode> {
      try {
        const raw = await kv.get(KEY);
        // An old palette id ('sandstone', 'ember', 'paper') or anything else
        // unknown: fall back to the default rather than to an empty screen.
        return isThemeMode(raw) ? raw : 'system';
      } catch {
        return 'system'; // unreadable storage: the default is fine
      }
    },
    async save(mode: ThemeMode): Promise<void> {
      await kv.set(KEY, mode);
    },
  };
}

// ---- the app's other preferences -------------------------------------------------------------

const PREFS_KEY = 'keystone.prefs';

/** The units the app speaks. */
export type TemperatureUnit = 'C' | 'F';

export interface Prefs {
  /** The unit a temperature is shown in. */
  temperature: TemperatureUnit;
  /** The scene pills above the rooms on Home. */
  showScenes: boolean;
  /** The devices grouped under room headings, not one flat list. */
  groupByRoom: boolean;
}

export const DEFAULT_PREFS: Prefs = {
  temperature: 'C',
  showScenes: true,
  groupByRoom: true,
};

function isTemperatureUnit(v: unknown): v is TemperatureUnit {
  return v === 'C' || v === 'F';
}

/** The saved preferences, or the defaults for whatever is missing or odd. */
export function readPrefs(raw: unknown): Prefs {
  if (typeof raw !== 'object' || raw === null) return { ...DEFAULT_PREFS };
  const o = raw as Record<string, unknown>;
  return {
    temperature: isTemperatureUnit(o.temperature) ? o.temperature : DEFAULT_PREFS.temperature,
    showScenes: typeof o.showScenes === 'boolean' ? o.showScenes : DEFAULT_PREFS.showScenes,
    groupByRoom: typeof o.groupByRoom === 'boolean' ? o.groupByRoom : DEFAULT_PREFS.groupByRoom,
  };
}

export function createPrefsStore(kv: KeyValueStore) {
  return {
    async load(): Promise<Prefs> {
      try {
        const raw = await kv.get(PREFS_KEY);
        if (raw === null || raw === undefined) return { ...DEFAULT_PREFS };
        return readPrefs(JSON.parse(raw));
      } catch {
        return { ...DEFAULT_PREFS }; // unreadable storage: the defaults are fine
      }
    },
    async save(prefs: Prefs): Promise<void> {
      await kv.set(PREFS_KEY, JSON.stringify(prefs));
    },
  };
}
