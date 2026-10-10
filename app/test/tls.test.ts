import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, afterEach, beforeAll, describe, expect, it } from 'vitest';
import { HubClient, PinUnsupportedError, type HubClientOptions } from '../src/protocol/client';
import { normalizeFingerprint, sameFingerprint } from '../src/protocol/fingerprint';
import { fetchFingerprint, pinnedSocketFactory } from '../tools/pinned-socket';
import { MockHub } from '../tools/mock-hub/hub';

// A throw-away self-signed identity, made with the same recipe as scripts/gen-hub-cert.sh.
let dir: string;
let identity: { key: string; cert: string; fingerprint: string } | null = null;

function makeIdentity(name: string): { key: string; cert: string; fingerprint: string } {
  const key = join(dir, `${name}.key`);
  const cert = join(dir, `${name}.crt`);
  execFileSync('openssl', ['ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', key], { stdio: 'ignore' });
  execFileSync(
    'openssl',
    ['req', '-new', '-x509', '-key', key, '-sha256', '-days', '2', '-out', cert, '-subj', '/O=Keystone/CN=test-hub', '-addext', 'subjectAltName=IP:127.0.0.1'],
    { stdio: 'ignore' },
  );
  const fp = execFileSync('openssl', ['x509', '-in', cert, '-noout', '-fingerprint', '-sha256']).toString().trim().replace(/^.*=/, '');
  return { key: readFileSync(key, 'utf8'), cert: readFileSync(cert, 'utf8'), fingerprint: fp };
}

let haveOpenssl = true;
beforeAll(() => {
  try {
    dir = mkdtempSync(join(tmpdir(), 'ks-tls-'));
    identity = makeIdentity('hub');
  } catch {
    haveOpenssl = false; // the TLS tests need the openssl CLI; they are skipped without it
  }
});
afterAll(() => {
  if (dir) rmSync(dir, { recursive: true, force: true });
});

let hub: MockHub | null = null;
const clients: HubClient[] = [];
afterEach(async () => {
  for (const c of clients.splice(0)) c.close();
  await hub?.close();
  hub = null;
});

async function until(fn: () => boolean, what: string, ms = 4000): Promise<void> {
  const t0 = Date.now();
  while (!fn()) {
    if (Date.now() - t0 > ms) throw new Error(`timed out waiting for: ${what}`);
    await new Promise((r) => setTimeout(r, 15));
  }
}

function client(over: Partial<HubClientOptions>): HubClient {
  const c = new HubClient({ url: hub!.url, token: hub!.token, socketFactory: pinnedSocketFactory, backoffMs: { min: 20, max: 60 }, ...over });
  clients.push(c);
  return c;
}

describe('fingerprint helpers', () => {
  const plain = 'D7C08EE028AAC6DE7CD8F290625F08A7843442ADD9208FA9276A719DAC96B771';
  const colons = 'D7:C0:8E:E0:28:AA:C6:DE:7C:D8:F2:90:62:5F:08:A7:84:34:42:AD:D9:20:8F:A9:27:6A:71:9D:AC:96:B7:71';
  it('accepts the usual ways of writing one and canonicalises them', () => {
    expect(normalizeFingerprint(colons)).toBe(colons);
    expect(normalizeFingerprint(plain)).toBe(colons);
    expect(normalizeFingerprint(plain.toLowerCase())).toBe(colons);
    expect(normalizeFingerprint(`  SHA256:${colons}  `)).toBe(colons);
    expect(normalizeFingerprint(`sha-256 ${colons.replace(/:/g, ' ')}`)).toBe(colons);
  });
  it('rejects anything that is not 32 bytes of hex', () => {
    for (const bad of ['', 'xyz', plain.slice(2), plain + '00', plain.replace('D', 'G'), 'sha256:']) {
      expect(normalizeFingerprint(bad)).toBeNull();
    }
    expect(sameFingerprint(colons, plain.toLowerCase())).toBe(true);
    expect(sameFingerprint(colons, 'nonsense')).toBe(false);
    expect(sameFingerprint('nonsense', 'nonsense')).toBe(false);
  });
});

describe('certificate pinning over wss', () => {
  it('connects when the pin matches the hub certificate', async (ctx) => {
    if (!haveOpenssl || !identity) return ctx.skip();
    hub = await MockHub.start({ tls: identity });
    expect(hub.url.startsWith('wss://')).toBe(true);
    const c = client({ pinnedFingerprint: identity.fingerprint });
    c.connect();
    await until(() => c.getSnapshot().status === 'ready', 'ready over TLS');
    expect(c.getSnapshot().entities).toHaveLength(3);
    expect(hub.helloCount).toBe(1);
  });

  it('accepts the pin in any notation', async (ctx) => {
    if (!haveOpenssl || !identity) return ctx.skip();
    hub = await MockHub.start({ tls: identity });
    const c = client({ pinnedFingerprint: `sha256:${identity.fingerprint.replace(/:/g, '').toLowerCase()}` });
    c.connect();
    await until(() => c.getSnapshot().status === 'ready', 'ready');
  });

  it('a different certificate is refused and the token is never sent', async (ctx) => {
    if (!haveOpenssl || !identity) return ctx.skip();
    const other = makeIdentity('impostor');
    expect(sameFingerprint(other.fingerprint, identity.fingerprint)).toBe(false);
    hub = await MockHub.start({ tls: other }); // an impostor on the hub's address
    const c = client({ pinnedFingerprint: identity.fingerprint });
    c.connect();
    await until(() => c.getSnapshot().status === 'untrusted', 'untrusted');
    expect(c.getSnapshot().error).toBe('certificate_mismatch');
    await new Promise((r) => setTimeout(r, 300)); // and it does not keep retrying
    expect(c.getSnapshot().status).toBe('untrusted');
    expect(hub.helloCount).toBe(0); // the token (inside hello) never reached the impostor
    expect(hub.clientCount).toBe(0);
  });

  it('wss:// without any pin is refused too (no silent trust of a self-signed certificate)', async (ctx) => {
    if (!haveOpenssl || !identity) return ctx.skip();
    hub = await MockHub.start({ tls: identity });
    const c = client({ pinnedFingerprint: undefined });
    c.connect();
    await until(() => c.getSnapshot().status === 'untrusted', 'untrusted');
    expect(hub.helloCount).toBe(0);
  });

  it('a malformed pin never matches', async (ctx) => {
    if (!haveOpenssl || !identity) return ctx.skip();
    hub = await MockHub.start({ tls: identity });
    const c = client({ pinnedFingerprint: 'not-a-fingerprint' });
    c.connect();
    await until(() => c.getSnapshot().status === 'untrusted', 'untrusted');
    expect(hub.helloCount).toBe(0);
  });

  it('a hub whose certificate was replaced is detected on reconnect', async (ctx) => {
    if (!haveOpenssl || !identity) return ctx.skip();
    hub = await MockHub.start({ tls: identity });
    const port = hub.port;
    const c = client({ pinnedFingerprint: identity.fingerprint });
    c.connect();
    await until(() => c.getSnapshot().status === 'ready', 'ready');
    await hub.close();
    hub = await MockHub.start({ tls: makeIdentity('replaced'), port });
    await until(() => c.getSnapshot().status === 'untrusted', 'untrusted after the certificate changed');
  });

  it('fetchFingerprint shows what a hub presents, for trust-on-first-use', async (ctx) => {
    if (!haveOpenssl || !identity) return ctx.skip();
    hub = await MockHub.start({ tls: identity });
    expect(await fetchFingerprint('127.0.0.1', hub.port)).toBe(identity.fingerprint);
  });

  it('plain ws:// needs no pin', async () => {
    hub = await MockHub.start();
    const c = client({});
    c.connect();
    await until(() => c.getSnapshot().status === 'ready', 'ready over ws');
  });
});

describe('a platform that cannot pin', () => {
  it('refuses to connect with a pin instead of pretending to check it', async () => {
    hub = await MockHub.start();
    // The default factory stands in for browsers and stock React Native.
    const c = new HubClient({ url: hub.url, token: hub.token, pinnedFingerprint: 'D7:C0:8E:E0:28:AA:C6:DE:7C:D8:F2:90:62:5F:08:A7:84:34:42:AD:D9:20:8F:A9:27:6A:71:9D:AC:96:B7:71' });
    clients.push(c);
    c.connect();
    expect(c.getSnapshot().status).toBe('untrusted');
    expect(c.getSnapshot().error).toBe('pin_unsupported');
    expect(hub.helloCount).toBe(0);
    expect(new PinUnsupportedError().name).toBe('PinUnsupportedError');
  });
  it('still works without a pin', async () => {
    hub = await MockHub.start();
    const c = new HubClient({ url: hub.url, token: hub.token });
    clients.push(c);
    c.connect();
    await until(() => c.getSnapshot().status === 'ready', 'ready');
  });
});
