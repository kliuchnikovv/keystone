// The palette is a setting of the phone: it is remembered, and a broken or
// unknown value falls back to the default rather than to an empty screen.
import { describe, expect, it } from 'vitest';
import { createPaletteStore, isPaletteId } from '../src/palette';
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

describe('palette', () => {
  it('knows its names', () => {
    expect(isPaletteId('sandstone')).toBe(true);
    expect(isPaletteId('ember')).toBe(true);
    expect(isPaletteId('paper')).toBe(true);
    expect(isPaletteId('midnight')).toBe(false);
  });

  it('remembers the choice', async () => {
    const kv = memoryStore();
    const store = createPaletteStore(kv);
    expect(await store.load()).toBe('sandstone'); // nothing saved yet
    await store.save('ember');
    expect(await store.load()).toBe('ember');
    expect(kv.data.get('keystone.palette')).toBe('ember');
    await store.save('paper');
    expect(await store.load()).toBe('paper');
  });

  it('an unknown or unreadable value falls back to the default', async () => {
    expect(await createPaletteStore(memoryStore({ 'keystone.palette': 'midnight' })).load()).toBe('sandstone');
    const broken: KeyValueStore = {
      async get() {
        throw new Error('no keychain');
      },
      async set() {},
      async delete() {},
    };
    expect(await createPaletteStore(broken).load()).toBe('sandstone');
  });
});
