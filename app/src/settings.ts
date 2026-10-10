// Where the app remembers which hub to talk to. The storage is injected (secure store
// on a phone, localStorage in a browser), so this logic is plain and testable.
import { normalizeFingerprint } from './protocol/fingerprint';

export interface Settings {
  url: string;
  token: string;
  /** SHA-256 of the hub's certificate to trust (wss:// to a self-signed hub), canonical "AA:BB:…". */
  fingerprint?: string;
}

export interface KeyValueStore {
  get(key: string): Promise<string | null>;
  set(key: string, value: string): Promise<void>;
  delete(key: string): Promise<void>;
}

const KEY = 'keystone.hub';

export type SettingsProblem = 'url' | 'token' | 'fingerprint' | null;

/** What is wrong with a connection form, if anything. */
export function validateSettings(s: Settings): SettingsProblem {
  if (!/^wss?:\/\/[^\s/]+(:\d+)?(\/\S*)?$/i.test(s.url.trim())) return 'url';
  if (s.token.length < 8) return 'token';
  if (s.fingerprint !== undefined && s.fingerprint !== '' && normalizeFingerprint(s.fingerprint) === null) return 'fingerprint';
  return null;
}

export function isSecure(url: string): boolean {
  return /^wss:\/\//i.test(url.trim());
}

export function isPlaintext(url: string): boolean {
  return /^ws:\/\//i.test(url.trim());
}

/** Accept "192.168.0.202", "hub.local:8080" or a full ws:// URL; fill in the rest. */
export function normalizeUrl(input: string): string {
  const s = input.trim();
  if (/^wss?:\/\//i.test(s)) return s;
  const hostPort = s.replace(/\/.*$/, '');
  const path = s.includes('/') ? s.slice(s.indexOf('/')) : '/ws';
  return `ws://${hostPort.includes(':') ? hostPort : hostPort + ':8080'}${path}`;
}

export function createSettingsStore(kv: KeyValueStore) {
  return {
    async load(): Promise<Settings | null> {
      try {
        const raw = await kv.get(KEY);
        if (!raw) return null;
        const v = JSON.parse(raw) as Partial<Settings>;
        if (typeof v.url !== 'string' || typeof v.token !== 'string') return null;
        const fp = typeof v.fingerprint === 'string' ? normalizeFingerprint(v.fingerprint) : null;
        return fp ? { url: v.url, token: v.token, fingerprint: fp } : { url: v.url, token: v.token };
      } catch {
        return null; // unreadable storage or a corrupt value: ask again
      }
    },
    async save(s: Settings): Promise<void> {
      const fp = s.fingerprint ? normalizeFingerprint(s.fingerprint) : null;
      await kv.set(KEY, JSON.stringify({ url: s.url.trim(), token: s.token, ...(fp ? { fingerprint: fp } : {}) }));
    },
    async clear(): Promise<void> {
      await kv.delete(KEY);
    },
  };
}
