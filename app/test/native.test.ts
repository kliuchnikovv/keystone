import { describe, expect, it } from 'vitest';
import { HubClient } from '../src/protocol/client';
import { startDiscovery, hubUrl } from '../src/native/discovery';
import { createNativeSocketFactory } from '../src/native/nativeSocket';
import { chooseSocketFactory } from '../src/native/platform';
import type { KeystoneNetModule, NativeHub, NativeSocketEvent } from '../src/native/types';

const PIN = 'D7:C0:8E:E0:28:AA:C6:DE:7C:D8:F2:90:62:5F:08:A7:84:34:42:AD:D9:20:8F:A9:27:6A:71:9D:AC:96:B7:71';
const OTHER = 'AA:C0:8E:E0:28:AA:C6:DE:7C:D8:F2:90:62:5F:08:A7:84:34:42:AD:D9:20:8F:A9:27:6A:71:9D:AC:96:B7:71';
const URL = 'wss://192.168.0.202:8443/ws';

// A native module that does nothing by itself: the test plays the platform.
function fakeModule() {
  const calls: string[] = [];
  const socketListeners = new Set<(e: NativeSocketEvent) => void>();
  const foundListeners = new Set<(h: NativeHub) => void>();
  const lostListeners = new Set<(h: { instance: string }) => void>();
  const mod = {
    connect: (id: string, url: string, fp: string) => calls.push(`connect ${id} ${url} ${fp}`),
    send: (id: string, text: string) => calls.push(`send ${id} ${text}`),
    close: (id: string) => calls.push(`close ${id}`),
    startDiscovery: () => calls.push('startDiscovery'),
    stopDiscovery: () => calls.push('stopDiscovery'),
    addListener(event: string, l: unknown) {
      const set = (event === 'onSocketEvent' ? socketListeners : event === 'onHubFound' ? foundListeners : lostListeners) as Set<unknown>;
      set.add(l);
      return { remove: () => set.delete(l) };
    },
  } as unknown as KeystoneNetModule;
  return {
    mod,
    calls,
    emit: (e: NativeSocketEvent) => socketListeners.forEach((l) => l(e)),
    found: (h: NativeHub) => foundListeners.forEach((l) => l(h)),
    lost: (instance: string) => lostListeners.forEach((l) => l({ instance })),
    listeners: () => socketListeners.size + foundListeners.size + lostListeners.size,
  };
}

describe('native socket (certificate pinning on a phone)', () => {
  it('passes the URL and the canonical pin to the platform and reports open only after the platform did', () => {
    const f = fakeModule();
    const sock = createNativeSocketFactory(f.mod)(URL, { fingerprint: PIN.toLowerCase().replaceAll(':', '') });
    expect(f.calls).toEqual([`connect s1 ${URL} ${PIN}`]);
    const opened: unknown[] = [];
    sock.onopen = (e) => opened.push(e);
    expect(opened).toEqual([]); // nothing is "open" yet, so no token is sent
    f.emit({ id: 's1', kind: 'open', fingerprint: PIN });
    expect(opened).toHaveLength(1);
  });

  it('an open that reports another certificate is a mismatch, even though the platform said open', () => {
    const f = fakeModule();
    const sock = createNativeSocketFactory(f.mod)(URL, { fingerprint: PIN });
    const log: unknown[] = [];
    sock.onopen = () => log.push('open');
    sock.onerror = (e) => log.push(e);
    sock.onclose = () => log.push('close');
    f.emit({ id: 's1', kind: 'open', fingerprint: OTHER });
    expect(log).toEqual([{ pinMismatch: true, seen: OTHER }, 'close']);
    expect(f.calls).toContain('close s1');
  });

  it('an open without any fingerprint is not trusted either', () => {
    const f = fakeModule();
    const sock = createNativeSocketFactory(f.mod)(URL, { fingerprint: PIN });
    const log: unknown[] = [];
    sock.onopen = () => log.push('open');
    sock.onerror = (e) => log.push(e);
    f.emit({ id: 's1', kind: 'open' });
    expect(log).toEqual([{ pinMismatch: true }]);
  });

  it('a platform pin error becomes a PinMismatchEvent and the close that follows is delivered once', () => {
    const f = fakeModule();
    const sock = createNativeSocketFactory(f.mod)(URL, { fingerprint: PIN });
    const log: unknown[] = [];
    sock.onerror = (e) => log.push(e);
    sock.onclose = () => log.push('close');
    f.emit({ id: 's1', kind: 'error', pinMismatch: true, seen: OTHER.toLowerCase() });
    f.emit({ id: 's1', kind: 'close' });
    f.emit({ id: 's1', kind: 'close' });
    expect(log).toEqual([{ pinMismatch: true, seen: OTHER }, 'close']);
  });

  it('other errors are plain errors; messages and sends are routed by socket id', () => {
    const f = fakeModule();
    const make = createNativeSocketFactory(f.mod);
    const a = make(URL, { fingerprint: PIN });
    const b = make(URL, { fingerprint: PIN });
    const got: string[] = [];
    a.onmessage = (e) => got.push(`a:${String(e.data)}`);
    b.onmessage = (e) => got.push(`b:${String(e.data)}`);
    let err: unknown = null;
    a.onerror = (e) => (err = e);
    f.emit({ id: 's2', kind: 'message', data: 'hi' });
    f.emit({ id: 's1', kind: 'error', message: 'timeout' });
    expect(got).toEqual(['b:hi']);
    expect(err).toBeInstanceOf(Error);
    a.send('x');
    b.send('y');
    expect(f.calls.filter((c) => c.startsWith('send'))).toEqual(['send s1 x', 'send s2 y']);
  });

  it('close() asks the platform, stops events, and the last socket releases the listener', () => {
    const f = fakeModule();
    const make = createNativeSocketFactory(f.mod);
    const sock = make(URL, { fingerprint: PIN });
    expect(f.listeners()).toBe(1);
    let closed = 0;
    sock.onclose = () => closed++;
    sock.close();
    sock.close();
    expect(closed).toBe(1);
    expect(f.calls.filter((c) => c === 'close s1')).toHaveLength(1);
    expect(f.listeners()).toBe(0);
    sock.send('late'); // after close: ignored
    expect(f.calls.some((c) => c.startsWith('send'))).toBe(false);
  });

  it('refuses to be used without a pin or for ws://', () => {
    const f = fakeModule();
    const make = createNativeSocketFactory(f.mod);
    expect(() => make(URL, {})).toThrow(/fingerprint/);
    expect(() => make('ws://192.168.0.202:8080/ws', { fingerprint: PIN })).toThrow(/wss/);
    expect(() => make(URL, { fingerprint: 'not a fingerprint' })).toThrow(/fingerprint/);
    expect(f.calls).toEqual([]);
  });
});

describe('chooseSocketFactory', () => {
  it('without the native module a pinned hub is refused, never connected unpinned', () => {
    const make = chooseSocketFactory(null);
    expect(() => make(URL, { fingerprint: PIN })).toThrow(/cannot verify/);
  });

  it('with the module a pinned hub goes through the native socket', () => {
    const f = fakeModule();
    chooseSocketFactory(f.mod)(URL, { fingerprint: PIN });
    expect(f.calls).toEqual([`connect s1 ${URL} ${PIN}`]);
  });

  it('without a pin the platform WebSocket is used (ws://, or wss:// with a real certificate)', () => {
    const f = fakeModule();
    const made: string[] = [];
    const g = globalThis as { WebSocket?: unknown };
    const prev = g.WebSocket;
    g.WebSocket = class { constructor(u: string) { made.push(u); } };
    try {
      chooseSocketFactory(f.mod)('ws://h:8080/ws', {});
    } finally {
      g.WebSocket = prev;
    }
    expect(made).toEqual(['ws://h:8080/ws']);
    expect(f.calls).toEqual([]);
  });

  it('HubClient: a certificate mismatch from the platform ends in untrusted, and no hello was sent', async () => {
    const f = fakeModule();
    const client = new HubClient({ url: URL, token: 'secret-token', pinnedFingerprint: PIN, socketFactory: chooseSocketFactory(f.mod), reconnect: false });
    client.connect();
    f.emit({ id: 's1', kind: 'error', pinMismatch: true, seen: OTHER });
    f.emit({ id: 's1', kind: 'close' });
    expect(client.getSnapshot().status).toBe('untrusted');
    expect(f.calls.some((c) => c.startsWith('send'))).toBe(false);
    client.close();
  });

  it('HubClient: the token is sent only after a verified open', () => {
    const f = fakeModule();
    const client = new HubClient({ url: URL, token: 'secret-token', pinnedFingerprint: PIN, socketFactory: chooseSocketFactory(f.mod), reconnect: false });
    client.connect();
    expect(f.calls.filter((c) => c.startsWith('send'))).toEqual([]);
    f.emit({ id: 's1', kind: 'open', fingerprint: PIN });
    const sent = f.calls.filter((c) => c.startsWith('send s1 '));
    expect(sent).toHaveLength(1);
    expect(sent[0]).toContain('"type":"hello"');
    client.close();
  });
});

describe('discovery', () => {
  const hub = (instance: string, extra: Partial<NativeHub> = {}): NativeHub => ({
    instance, name: 'home', host: 'keystone.local', address: '192.168.0.202', port: 8443, tls: true, ...extra,
  });

  it('lists hubs as they appear and vanish, sorted, and stops cleanly', () => {
    const f = fakeModule();
    const seen: string[][] = [];
    const stop = startDiscovery(f.mod, (h) => seen.push(h.map((x) => x.instance)));
    expect(f.calls).toEqual(['startDiscovery']);
    f.found(hub('B hub'));
    f.found(hub('A hub'));
    f.found(hub('A hub', { port: 8080, tls: false })); // same instance: replaced, not duplicated
    f.lost('B hub');
    f.lost('unknown'); // nothing changes, no event
    expect(seen).toEqual([['B hub'], ['A hub', 'B hub'], ['A hub', 'B hub'], ['A hub']]);
    stop();
    expect(f.calls).toEqual(['startDiscovery', 'stopDiscovery']);
    expect(f.listeners()).toBe(0);
  });

  it('builds the URL from the address, falling back to the host name', () => {
    expect(hubUrl(hub('x'))).toBe('wss://192.168.0.202:8443/ws');
    expect(hubUrl(hub('x', { tls: false, port: 8080 }))).toBe('ws://192.168.0.202:8080/ws');
    expect(hubUrl(hub('x', { address: null }))).toBe('wss://keystone.local:8443/ws');
  });
});
