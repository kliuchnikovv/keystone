// Where the app remembers which hub to talk to. The storage is injected (secure store
// on a phone, localStorage in a browser), so this logic is plain and testable.
export interface Settings {
  url: string;
  token: string;
}

export interface KeyValueStore {
  get(key: string): Promise<string | null>;
  set(key: string, value: string): Promise<void>;
  delete(key: string): Promise<void>;
}

const KEY = 'keystone.hub';

export type SettingsProblem = 'url' | 'token' | null;

/** What is wrong with a connection form, if anything. */
export function validateSettings(s: Settings): SettingsProblem {
  if (!/^wss?:\/\/[^\s/]+(:\d+)?(\/\S*)?$/i.test(s.url.trim())) return 'url';
  if (s.token.length < 8) return 'token';
  return null;
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
        return { url: v.url, token: v.token };
      } catch {
        return null; // unreadable storage or a corrupt value: ask again
      }
    },
    async save(s: Settings): Promise<void> {
      await kv.set(KEY, JSON.stringify({ url: s.url.trim(), token: s.token }));
    },
    async clear(): Promise<void> {
      await kv.delete(KEY);
    },
  };
}
