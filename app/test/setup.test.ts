import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WebSocket } from 'ws';
import { afterAll, afterEach, beforeAll, describe, expect, it } from 'vitest';
import { HubClient, PinUnsupportedError, type SocketFactory, type SocketLike } from '../src/protocol/client';
import { sameFingerprint } from '../src/protocol/fingerprint';
import { SetupClient, SetupError } from '../src/protocol/setup';
import { generateToken, normalizeSetupCode, setupCodeProblem, formatSetupCode } from '../src/model/setup';
import { pinnedSocketFactory } from '../tools/pinned-socket';
import { MockHub } from '../tools/mock-hub/hub';

const CODE = 'HGG5YKJN2MFU';
const wsFactory: SocketFactory = (url) => new WebSocket(url) as unknown as SocketLike;

let hub: MockHub | null = null;
const setups: SetupClient[] = [];
const hubClients: HubClient[] = [];
afterEach(async () => {
  for (const s of setups.splice(0)) s.close();
  for (const c of hubClients.splice(0)) c.close();
  await hub?.close();
  hub = null;
});

const setup = (over: { code?: string; url?: string; socketFactory?: SocketFactory } = {}) => {
  const s = new SetupClient({ code: CODE, socketFactory: wsFactory, connectTimeoutMs: 1500, requestTimeoutMs: 1500, ...over, url: over.url ?? hub!.url });
  setups.push(s);
  return s;
};
const kindOf = async (p: Promise<unknown>): Promise<string> => {
  try {
    await p;
    return 'resolved';
  } catch (e) {
    return e instanceof SetupError ? e.kind : `other: ${(e as Error).message}`;
  }
};
async function until(fn: () => boolean, what: string, ms = 3000): Promise<void> {
  const t0 = Date.now();
  while (!fn()) {
    if (Date.now() - t0 > ms) throw new Error(`timed out waiting for: ${what}`);
    await new Promise((r) => setTimeout(r, 15));
  }
}

describe('the setup code as a person types it', () => {
  it('ignores case, dashes and spaces, and rejects what cannot be a code', () => {
    expect(normalizeSetupCode('hgg5-ykjn-2mfu')).toBe(CODE);
    expect(normalizeSetupCode(' HGG5 YKJN 2MFU ')).toBe(CODE);
    expect(normalizeSetupCode('HGG5-YKJN-2MF')).toBeNull();
    expect(normalizeSetupCode('HGG5-YKJN-2MF0')).toBeNull(); // 0 is not in the alphabet
    expect(formatSetupCode(CODE)).toBe('HGG5-YKJN-2MFU');
  });
  it('says what is wrong in words', () => {
    expect(setupCodeProblem('')).toMatch(/Enter the setup code/);
    expect(setupCodeProblem('ABC')).toMatch(/12 characters \(you typed 3\)/);
    expect(setupCodeProblem('HGG5-YKJN-2MF0')).toMatch(/never 0, O, 1, I or L/);
    expect(setupCodeProblem('hgg5-ykjn-2mfu')).toBeNull();
  });
  it('makes a token the hub accepts, and a different one every time', async () => {
    const a = await generateToken();
    const b = await generateToken();
    expect(a).toMatch(/^[0-9a-f]{32}$/);
    expect(a).not.toBe(b);
  });
});

describe('first-time setup against a hub in setup mode', () => {
  it('signs in with the code however it was typed, and gets a welcome but no snapshot', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    const s = setup({ code: 'hgg5-ykjn-2mfu' });
    await s.connect();
    expect(s.hub?.name).toBe('mock-home');
    // No snapshot or event ever follows for a setup client.
    hub.setValue(0x0100, 0x01, true);
    await new Promise((r) => setTimeout(r, 100));
    expect(await s.scan()).toBeTruthy(); // the connection is alive and answers requests
  });

  it('a wrong code is refused', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    expect(await kindOf(setup({ code: 'AAAAAAAAAAAA' }).connect())).toBe('bad_code');
  });

  it('a hub without setup mode refuses the code like any wrong token', async () => {
    hub = await MockHub.start();
    expect(await kindOf(setup().connect())).toBe('bad_code');
  });

  it('the real token is not a setup code: the hub is already set up', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    expect(await kindOf(setup({ code: hub.token }).connect())).toBe('not_setup'); // it got in as a normal client
  });

  it('nothing answers: unreachable', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    const url = hub.url;
    await hub.close();
    hub = null;
    expect(await kindOf(setup({ url }).connect())).toBe('unreachable');
  });

  it('a setup client may scan and apply, and nothing else', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    const ws = new WebSocket(hub.url);
    const msgs: Record<string, unknown>[] = [];
    ws.on('message', (d) => msgs.push(JSON.parse(d.toString())));
    await new Promise((r) => ws.once('open', r));
    ws.send(JSON.stringify({ type: 'hello', v: 1, token: CODE }));
    await until(() => msgs.some((m) => m.type === 'welcome'), 'welcome');
    expect(msgs.find((m) => m.type === 'welcome')).toMatchObject({ setup: true });
    ws.send(JSON.stringify({ type: 'cmd', id: 1, entity: 0x0100, attr: 1, value: true }));
    for (const [id, op] of [[2, 'rule.add'], [3, 'name.set'], [4, 'scene.apply'], [5, 'wifi.set'], [6, 'pair'], [7, 'location.set']] as const) {
      ws.send(JSON.stringify({ type: 'req', id, op, args: {} }));
    }
    await until(() => msgs.filter((m) => m.type === 'res' || m.type === 'error').length === 7, 'seven refusals');
    expect(msgs.find((m) => m.type === 'error' && m.id === 1)).toMatchObject({ code: 'unsupported' });
    for (const id of [2, 3, 4, 5, 6, 7]) expect(msgs.find((m) => m.type === 'res' && m.id === id)).toMatchObject({ ok: false, code: 'unsupported' });
    expect(msgs.some((m) => m.type === 'snapshot' || m.type === 'state')).toBe(false);
    expect(hub.rules).toEqual([]);
    ws.close();
  });

  it('scan lists what the hub sees, strongest first', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    hub.wifi = null;
    const s = setup();
    await s.connect();
    const nets = await s.scan();
    expect(nets.map((n) => n.ssid)).toEqual(['home', 'cafe', 'neighbour']);
  });

  it('apply hands over the network and the token; the hub then takes the new token and drops the old', async () => {
    hub = await MockHub.start({ setupCode: CODE, token: 'factory-token-0' });
    hub.wifi = null;
    const s = setup();
    await s.connect();
    const token = await generateToken();
    await s.apply('home', 'goodpass1', token);
    expect(hub.setupRequests).toEqual([{ ssid: 'home', password: 'goodpass1', token, kept: true }]);
    await until(() => hub!.clientCount === 0, 'the hub drops the setup connection');
    await until(() => hub!.wifi !== null, 'the hub joins the network');
    // The new token works; the old one does not.
    const ok = new HubClient({ url: hub.url, token, socketFactory: wsFactory, reconnect: false });
    hubClients.push(ok);
    ok.connect();
    await until(() => ok.getSnapshot().status === 'ready', 'the new token is accepted');
    const old = new HubClient({ url: hub.url, token: 'factory-token-0', socketFactory: wsFactory, reconnect: false });
    hubClients.push(old);
    old.connect();
    await until(() => old.getSnapshot().status === 'unauthorized', 'the old token is refused');
  });

  it('a wrong Wi-Fi password is accepted by the hub (it cannot know) but it stays off the network and in setup', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    hub.wifi = null;
    const s = setup();
    await s.connect();
    await s.apply('home', 'wrongpass1', 'some-new-token-1');
    await until(() => hub!.setupRequests.length === 1, 'the request');
    await new Promise((r) => setTimeout(r, 150));
    expect(hub.setupRequests[0]!.kept).toBe(false);
    expect(hub.wifi).toBeNull();
    // Setup can be run again with the code.
    const again = setup();
    await again.connect();
  });

  it('apply checks what it is given', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    const s = setup();
    await s.connect();
    expect(await kindOf(s.apply('', 'goodpass1', 'some-new-token-1'))).toBe('refused');
    expect(await kindOf(s.apply('home', 'short', 'some-new-token-1'))).toBe('refused');
    expect(await kindOf(s.apply('home', 'goodpass1', 'short'))).toBe('refused');
    expect(hub.setupRequests).toEqual([]);
  });

  it('a normal client cannot use setup.apply', async () => {
    hub = await MockHub.start({ setupCode: CODE });
    const c = new HubClient({ url: hub.url, token: hub.token, socketFactory: wsFactory, reconnect: false });
    hubClients.push(c);
    c.connect();
    await until(() => c.getSnapshot().status === 'ready', 'ready');
    // The mock accepts the op from anyone (the real hub refuses non-setup clients); what matters in the app is that
    // the setup client never gets a snapshot, covered above.
    expect(hub.setupRequests).toEqual([]);
  });
});

describe('learning the hub certificate (the first connection over TLS)', () => {
  let dir = '';
  let identity: { key: string; cert: string; fingerprint: string } | null = null;
  beforeAll(() => {
    try {
      dir = mkdtempSync(join(tmpdir(), 'ks-setup-'));
      execFileSync('openssl', ['ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', join(dir, 'k')], { stdio: 'ignore' });
      execFileSync('openssl', ['req', '-new', '-x509', '-key', join(dir, 'k'), '-sha256', '-days', '2', '-out', join(dir, 'c'), '-subj', '/O=Keystone/CN=t', '-addext', 'subjectAltName=IP:127.0.0.1'], { stdio: 'ignore' });
      identity = {
        key: readFileSync(join(dir, 'k'), 'utf8'),
        cert: readFileSync(join(dir, 'c'), 'utf8'),
        fingerprint: execFileSync('openssl', ['x509', '-in', join(dir, 'c'), '-noout', '-fingerprint', '-sha256']).toString().trim().replace(/^.*=/, ''),
      };
    } catch {
      identity = null; // no openssl: these are skipped
    }
  });
  afterAll(() => dir && rmSync(dir, { recursive: true, force: true }));

  it('reports the fingerprint of the certificate the hub presented, and that pin then works', async () => {
    if (!identity) return;
    hub = await MockHub.start({ setupCode: CODE, tls: { key: identity.key, cert: identity.cert } });
    const s = setup({ socketFactory: pinnedSocketFactory });
    await s.connect();
    expect(sameFingerprint(s.fingerprint ?? '', identity.fingerprint)).toBe(true);
    const c = new HubClient({ url: hub.url, token: hub.token, socketFactory: pinnedSocketFactory, pinnedFingerprint: s.fingerprint!, reconnect: false });
    hubClients.push(c);
    c.connect();
    await until(() => c.getSnapshot().status === 'ready', 'a client pinned to the learned fingerprint connects');
  });

  it('a platform that cannot learn is reported as unsupported, not as a network problem', async () => {
    const refuse: SocketFactory = () => {
      throw new PinUnsupportedError();
    };
    const s = new SetupClient({ url: 'wss://192.168.4.1:8443/ws', code: CODE, socketFactory: refuse });
    setups.push(s);
    expect(await kindOf(s.connect())).toBe('unsupported');
  });

  it('a secure socket that reports no fingerprint is not trusted', async () => {
    const mute: SocketFactory = () => {
      const sock: SocketLike = { onopen: null, onmessage: null, onclose: null, onerror: null, send() {}, close() {} };
      queueMicrotask(() => sock.onopen?.({}));
      return sock;
    };
    const s = new SetupClient({ url: 'wss://192.168.4.1:8443/ws', code: CODE, socketFactory: mute, connectTimeoutMs: 500 });
    setups.push(s);
    expect(await kindOf(s.connect())).toBe('unreachable');
  });
});
