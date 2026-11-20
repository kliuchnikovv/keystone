// The theme mode is a setting of the phone: it is remembered, and a broken or
// unknown value falls back to the default rather than to an empty screen.
import { describe, expect, it } from 'vitest';
import { createThemeStore, isThemeMode } from '../src/palette';
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
    expect(await s.load()).toBe('system'); // nothing saved yet
    await s.save('dark');
    expect(await s.load()).toBe('dark');
    expect(kv.data.get('keystone.theme')).toBe('dark');
  });

  it('an old palette value or a throwing store falls back to system', async () => {
    expect(await createThemeStore(memoryStore({ 'keystone.theme': 'sandstone' })).load()).toBe('system');
    const broken: KeyValueStore = {
      async get() {
        throw new Error('x');
      },
      async set() {},
      async delete() {},
    };
    expect(await createThemeStore(broken).load()).toBe('system');
  });
});
