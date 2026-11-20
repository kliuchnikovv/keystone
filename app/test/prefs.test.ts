// The app's preferences: the units it speaks and what the Home screen shows.
import { describe, expect, it } from 'vitest';
import { DEFAULT_PREFS, createPrefsStore, readPrefs } from '../src/palette';
import type { KeyValueStore } from '../src/settings';

function memoryStore(initial: Record<string, string> = {}): KeyValueStore & { data: Map<string, string> } {
  const data = new Map(Object.entries(initial));
  return {
    data,
    async get(k) {
      return data.get(k) ?? null;
    },
    async set(k, v) {
      data.set(k, v);
    },
    async delete(k) {
      data.delete(k);
    },
  };
}

describe('preferences', () => {
  it('the defaults are Celsius, scenes on, rooms on', () => {
    expect(DEFAULT_PREFS).toEqual({ temperature: 'C', showScenes: true, groupByRoom: true });
  });

  it('remembers the choice', async () => {
    const kv = memoryStore();
    const store = createPrefsStore(kv);
    expect(await store.load()).toEqual(DEFAULT_PREFS); // nothing saved yet
    await store.save({ temperature: 'F', showScenes: false, groupByRoom: true });
    expect(await store.load()).toEqual({ temperature: 'F', showScenes: false, groupByRoom: true });
  });

  it('an odd or old value falls back to the defaults', () => {
    expect(readPrefs({ temperature: 'K' })).toEqual(DEFAULT_PREFS);
    expect(readPrefs({ temperature: 'F' })).toEqual({ ...DEFAULT_PREFS, temperature: 'F' });
    expect(readPrefs('nonsense')).toEqual(DEFAULT_PREFS);
    expect(readPrefs(null)).toEqual(DEFAULT_PREFS);
  });

  it('a store that throws falls back to the defaults', async () => {
    const broken: KeyValueStore = {
      async get() {
        throw new Error('no keychain');
      },
      async set() {},
      async delete() {},
    };
    expect(await createPrefsStore(broken).load()).toEqual(DEFAULT_PREFS);
  });
});
