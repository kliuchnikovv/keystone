// Which palette the app wears. It is a setting of the phone, not of the hub:
// it lives in the same key-value store as the hub address (secure store on a
// phone, localStorage in a browser), under its own key.
import type { KeyValueStore } from './settings';
import type { PaletteId } from './ui/theme';

const KEY = 'keystone.palette';

export function isPaletteId(v: unknown): v is PaletteId {
  return v === 'sandstone' || v === 'ember' || v === 'paper';
}

export function createPaletteStore(kv: KeyValueStore) {
  return {
    async load(): Promise<PaletteId> {
      try {
        const raw = await kv.get(KEY);
        return isPaletteId(raw) ? raw : 'sandstone';
      } catch {
        return 'sandstone'; // unreadable storage: the default is fine
      }
    },
    async save(id: PaletteId): Promise<void> {
      await kv.set(KEY, id);
    },
  };
}
