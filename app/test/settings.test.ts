import { describe, expect, it } from 'vitest';
import { cloudUrl, createSettingsStore, isCloudUrl, isPlaintext, isRelayUrl, isSecure, normalizeUrl, relayClientUrl, validateSettings, type KeyValueStore } from '../src/settings';

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
  it('keeps the security of an address copied from a browser', () => {
    // https:// is what the browser shows - the WebSocket has to stay on TLS.
    expect(normalizeUrl('https://relay.example.com/client/home')).toBe('wss://relay.example.com/client/home');
    expect(normalizeUrl('http://relay.example.com/client/home')).toBe('ws://relay.example.com/client/home');
  });
  it('a relay address without a scheme means TLS and the default port', () => {
    expect(normalizeUrl('relay.example.com/client/home')).toBe('wss://relay.example.com/client/home');
    expect(normalizeUrl('keystone-esp-production.up.railway.app/client/home')).toBe(
      'wss://keystone-esp-production.up.railway.app/client/home',
    );
  });
  it('recognises relay URLs', () => {
    expect(isRelayUrl('wss://relay.example.com/client/my-hub')).toBe(true);
    expect(isRelayUrl('ws://relay:9090/client/my-hub')).toBe(true);
    expect(isRelayUrl('ws://192.168.0.202:8080/ws')).toBe(false);
    expect(isRelayUrl('wss://hub.local/ws')).toBe(false);
    expect(isRelayUrl('')).toBe(false);
  });
  it('turns the hub\'s tunnel address into the address for the app', () => {
    expect(relayClientUrl('wss://relay.example.com/tunnel/home')).toBe('wss://relay.example.com/client/home');
    expect(relayClientUrl('ws://relay:9090/tunnel/my-hub')).toBe('ws://relay:9090/client/my-hub');
    // Nothing to derive from an address without a tunnel.
    expect(relayClientUrl('https://relay.example.com')).toBe('');
  });
  it('validates relay URLs', () => {
    expect(validateSettings({ url: 'wss://relay.example.com/client/my-hub', token: '12345678' })).toBeNull();
    expect(validateSettings({ url: 'ws://relay:9090/client/my-hub', token: '12345678' })).toBeNull();
  });
  it('knows when the link is unencrypted', () => {
    expect(isPlaintext('ws://hub:8080/ws')).toBe(true);
    expect(isPlaintext('wss://hub/ws')).toBe(false);
  });
  it('recognises cloud URLs', () => {
    expect(isCloudUrl('https://relay.example.com/hub/abc123')).toBe(true);
    expect(isCloudUrl('http://localhost:9090/hub/home-5ef894')).toBe(true);
    expect(isCloudUrl('wss://relay.example.com/client/my-hub')).toBe(false);
    expect(isCloudUrl('ws://hub:8080/ws')).toBe(false);
    expect(isCloudUrl('')).toBe(false);
  });
  it('builds a cloud URL from server and hub id', () => {
    expect(cloudUrl('relay.example.com', 'abc123')).toBe('https://relay.example.com/hub/abc123');
    expect(cloudUrl('https://relay.example.com/', 'abc123')).toBe('https://relay.example.com/hub/abc123');
    expect(cloudUrl('http://localhost:9090', 'home')).toBe('http://localhost:9090/hub/home');
  });
  it('validates cloud URLs', () => {
    expect(validateSettings({ url: 'https://relay.example.com/hub/abc123', token: '12345678' })).toBeNull();
  });
});

describe('certificate fingerprint in settings', () => {
  const colons = 'D7:C0:8E:E0:28:AA:C6:DE:7C:D8:F2:90:62:5F:08:A7:84:34:42:AD:D9:20:8F:A9:27:6A:71:9D:AC:96:B7:71';
  it('is validated, stored canonically and survives a reload', async () => {
    expect(validateSettings({ url: 'wss://hub:8443/ws', token: '12345678', fingerprint: colons })).toBeNull();
    expect(validateSettings({ url: 'wss://hub:8443/ws', token: '12345678', fingerprint: colons.toLowerCase().replace(/:/g, ' ') })).toBeNull();
    expect(validateSettings({ url: 'wss://hub:8443/ws', token: '12345678', fingerprint: 'abc' })).toBe('fingerprint');
    expect(validateSettings({ url: 'wss://hub:8443/ws', token: '12345678', fingerprint: '' })).toBeNull(); // empty = not pinned
    const st = createSettingsStore(memoryStore());
    await st.save({ url: 'wss://hub:8443/ws', token: '12345678', fingerprint: colons.toLowerCase() });
    expect(await st.load()).toEqual({ url: 'wss://hub:8443/ws', token: '12345678', fingerprint: colons });
  });
  it('an unusable stored fingerprint is dropped rather than trusted', async () => {
    const kv = memoryStore({ 'keystone.hub': JSON.stringify({ url: 'wss://h/ws', token: '12345678', fingerprint: 'garbage' }) });
    expect(await createSettingsStore(kv).load()).toEqual({ url: 'wss://h/ws', token: '12345678' });
  });
  it('knows which URLs are secure', () => {
    expect(isSecure('wss://hub:8443/ws')).toBe(true);
    expect(isSecure('ws://hub:8080/ws')).toBe(false);
  });
});
