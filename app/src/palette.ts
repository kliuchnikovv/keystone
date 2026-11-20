// The palette is a setting of the phone, not of the hub: it lives in the same
// key-value store as the hub address (secure store on a phone, localStorage in
// a browser), under its own key.
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
