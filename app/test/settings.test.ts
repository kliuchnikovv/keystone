import { describe, expect, it } from 'vitest';
import { createSettingsStore, isPlaintext, normalizeUrl, validateSettings, type KeyValueStore } from '../src/settings';

function memoryStore(initial: Record<string, string> = {}, failing = false): KeyValueStore & { data: Record<string, string> } {
  const data = { ...initial };
  const boom = () => { if (failing) throw new Error('storage unavailable'); };
  return {
    data,
    async get(k) { boom(); return data[k] ?? null; },
    async set(k, v) { boom(); data[k] = v; },
    async delete(k) { boom(); delete data[k]; },
  };
}

describe('settings store', () => {
  it('saves, loads and clears', async () => {
    const kv = memoryStore();
    const st = createSettingsStore(kv);
    expect(await st.load()).toBeNull();
    await st.save({ url: ' ws://hub:8080/ws ', token: 'secret-token' });
    expect(await st.load()).toEqual({ url: 'ws://hub:8080/ws', token: 'secret-token' });
    await st.clear();
    expect(await st.load()).toBeNull();
  });
  it('a corrupt or foreign value, or a broken store, means "not configured", never a crash', async () => {
    expect(await createSettingsStore(memoryStore({ 'keystone.hub': 'not json' })).load()).toBeNull();
    expect(await createSettingsStore(memoryStore({ 'keystone.hub': '{"url":1,"token":2}' })).load()).toBeNull();
    expect(await createSettingsStore(memoryStore({ 'keystone.hub': '[]' })).load()).toBeNull();
    expect(await createSettingsStore(memoryStore({}, true)).load()).toBeNull();
  });
});

describe('connection form', () => {
  it('validates url and token', () => {
    expect(validateSettings({ url: 'ws://192.168.0.202:8080/ws', token: '12345678' })).toBeNull();
    expect(validateSettings({ url: 'wss://hub.local/ws', token: '12345678' })).toBeNull();
    expect(validateSettings({ url: 'http://hub/ws', token: '12345678' })).toBe('url');
    expect(validateSettings({ url: 'ws://', token: '12345678' })).toBe('url');
    expect(validateSettings({ url: 'ws://hub with space/ws', token: '12345678' })).toBe('url');
    expect(validateSettings({ url: 'ws://hub:8080/ws', token: 'short' })).toBe('token');
  });
  it('fills in a full URL from what people type', () => {
    expect(normalizeUrl('192.168.0.202')).toBe('ws://192.168.0.202:8080/ws');
    expect(normalizeUrl('hub.local:9000')).toBe('ws://hub.local:9000/ws');
    expect(normalizeUrl(' ws://x:1/ws ')).toBe('ws://x:1/ws');
    expect(normalizeUrl('wss://hub/ws')).toBe('wss://hub/ws');
    expect(normalizeUrl('10.0.0.5/custom')).toBe('ws://10.0.0.5:8080/custom');
  });
  it('knows when the link is unencrypted', () => {
    expect(isPlaintext('ws://hub:8080/ws')).toBe(true);
    expect(isPlaintext('wss://hub/ws')).toBe(false);
  });
});
