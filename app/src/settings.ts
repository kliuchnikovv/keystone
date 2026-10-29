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
  const u = s.url.trim();
  if (isCloudUrl(u) ? !/^https?:\/\//i.test(u) : !/^wss?:\/\/[^\s/]+(:\d+)?(\/\S*)?$/i.test(u)) return 'url';
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

export function isRelayUrl(url: string): boolean {
  return /\/client\/[^\s/]+\s*$/i.test(url.trim());
}

export function isCloudUrl(url: string): boolean {
  return /^https?:\/\/[^\s/]+(:\d+)?\/hub\/[^\s/]+\s*$/i.test(url.trim());
}

/**
 * The certificate to pin for this address, or undefined when there is none to pin.
 *
 * A hub presents its own self-signed certificate, and pinning that fingerprint is
 * what tells the app it is talking to *its* hub. A relay presents its own
 * certificate - a CA-signed one that the platform checks the usual way - and the
 * hub's certificate never appears on the wire. Pinning the hub's fingerprint
 * there would refuse every connection, so through a relay the hub's identity
 * rests on its token instead (and the relay is a trusted party: it sees the
 * traffic - see docs/relay-security.md).
 */
export function pinnedFingerprintFor(url: string, fingerprint?: string): string | undefined {
  if (!fingerprint) return undefined;
  if (isRelayUrl(url)) return undefined;
  return fingerprint;
}

export function cloudUrl(server: string, hubId: string): string {
  const s = server.trim().replace(/\/+$/, '');
  const base = /^https?:\/\//i.test(s) ? s : `https://${s}`;
  return `${base}/hub/${hubId.trim()}`;
}

/** The address the app uses to reach the hub through a relay: the same host, but the
 * client side of the tunnel (the hub's own address ends in /tunnel/<id>). Empty when
 * the address is not of that shape - there is nothing to derive then. */
export function relayClientUrl(tunnelUrl: string): string {
  const m = tunnelUrl.trim().match(/^(.*)\/tunnel\/([^\s/]+)\s*$/i);
  return m ? `${m[1]}/client/${m[2]}` : '';
}

/** Accept "192.168.0.202", "hub.local:8080" or a full ws:// URL; fill in the rest.
 *
 * A relay lives outside the LAN, so it is talked to over TLS on the standard
 * port - and an address copied from a browser (https://...) keeps its security
 * instead of silently losing it. */
export function normalizeUrl(input: string): string {
  const s = input.trim();
  if (/^wss?:\/\//i.test(s)) return s;
  if (/^https:\/\//i.test(s)) return `wss://${s.slice('https://'.length)}`;
  if (/^http:\/\//i.test(s)) return `ws://${s.slice('http://'.length)}`;
  const slash = s.indexOf('/');
  const hostPort = slash >= 0 ? s.slice(0, slash) : s;
  const path = slash >= 0 ? s.slice(slash) : '/ws';
  // A relay URL without a scheme: TLS and the default port, not port 8080.
  if (isRelayUrl(path)) return `wss://${hostPort}${path}`;
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
