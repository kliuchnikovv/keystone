// Conformance check of the hub's first-time setup mode (docs/app-protocol.md §11), against a mock or the real hub:
//   npm run setup-check -- --url wss://192.168.0.202:8443/ws --code HGG5-YKJN-2MFU --token <token> [--fingerprint <sha256>]
// The hub must have its setup access point open (`setup on` on the console, or no saved network); the check runs
// over whatever route reaches the hub (the access point, or the LAN while `setup on` keeps the server open to both).
// `--apply` also sends setup.apply with the network the hub is on now, so the token is swapped for `--new-token` (default:
// the same token, so nothing changes for the apps) and the Wi-Fi trial runs for real: pass the network password in the
// environment as KS_SETUP_PASS (it is never printed). Takes ~15 s and drops the hub's connections once.
import { HubClient } from '../src/protocol/client';
import { sameFingerprint } from '../src/protocol/fingerprint';
import { SetupClient, SetupError } from '../src/protocol/setup';
import { pinnedSocketFactory } from './pinned-socket';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] ?? '' : undefined;
}
const flag = (name: string) => process.argv.includes(`--${name}`);
const url = arg('url') ?? 'wss://192.168.4.1:8443/ws';
const code = arg('code');
const token = arg('token');
const fingerprint = arg('fingerprint');
if (!code) {
  console.error('--code <setup code> is required (the hub console prints it with `setup`)');
  process.exit(2);
}
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

let pass = 0;
let fail = 0;
async function check(name: string, fn: () => Promise<boolean | string>): Promise<void> {
  try {
    const res = await fn();
    if (res === true) {
      pass++;
      console.log(`  ok    ${name}`);
    } else {
      fail++;
      console.log(`  FAIL  ${name}${typeof res === 'string' ? ': ' + res : ''}`);
    }
  } catch (e) {
    fail++;
    console.log(`  FAIL  ${name}: ${(e as Error).message}`);
  }
}
const kind = async (p: Promise<unknown>) => {
  try {
    await p;
    return 'resolved';
  } catch (e) {
    return e instanceof SetupError ? e.kind : (e as Error).message;
  }
};
const newSetup = (c = code!) => new SetupClient({ url, code: c, socketFactory: pinnedSocketFactory, connectTimeoutMs: 12_000, requestTimeoutMs: 20_000 });

console.log(`setup-check ${url}`);

const s = newSetup(code.toLowerCase());
let learned: string | null = null;
await check('sign in with the setup code (typed in lower case, with dashes)', async () => {
  await s.connect();
  learned = s.fingerprint;
  return s.hub !== null;
});
if (!s.hub) {
  console.log('\nCould not sign in: stopping.');
  process.exit(1);
}
if (/^wss:/i.test(url)) {
  await check('the certificate fingerprint is learned' + (fingerprint ? ' and is the hub’s' : ''), async () => {
    if (!learned) return 'none reported';
    return fingerprint ? (sameFingerprint(learned, fingerprint) ? true : `learned ${learned}`) : true;
  });
}

// What a setup client can and cannot do: speak raw, so the checks do not depend on SetupClient's own care.
const { WebSocket } = await import('ws');
async function rawSetup(): Promise<{ msgs: Record<string, unknown>[]; send: (o: unknown) => void; close: () => void }> {
  const ws = new WebSocket(url, /^wss:/i.test(url) ? { rejectUnauthorized: false } : {});
  const msgs: Record<string, unknown>[] = [];
  ws.on('message', (d) => {
    try {
      msgs.push(JSON.parse(d.toString()));
    } catch {
      /* ignore */
    }
  });
  ws.on('error', () => {});
  await new Promise<void>((resolve, reject) => {
    ws.once('open', () => resolve());
    ws.once('error', reject);
  });
  ws.send(JSON.stringify({ type: 'hello', v: 1, token: code }));
  for (let i = 0; i < 400 && !msgs.some((m) => m.type === 'welcome'); i++) await sleep(25);
  return { msgs, send: (o) => ws.send(JSON.stringify(o)), close: () => ws.close() };
}

await check('the welcome says setup, and no snapshot or event follows', async () => {
  const r = await rawSetup();
  try {
    const w = r.msgs.find((m) => m.type === 'welcome');
    if (!w || w.setup !== true) return 'welcome without "setup": true';
    await sleep(1500);
    return r.msgs.some((m) => m.type === 'snapshot' || m.type === 'state' || m.type === 'chip') ? 'the setup client was sent home data' : true;
  } finally {
    r.close();
  }
});

await check('a setup client is refused everything but wifi.scan and setup.apply', async () => {
  const r = await rawSetup();
  try {
    r.send({ type: 'cmd', id: 1, entity: 256, attr: 1, value: true });
    const ops = ['rule.add', 'name.set', 'scene.apply', 'wifi.set', 'pair', 'unpair', 'location.set', 'schedule.add', 'timer.add'];
    ops.forEach((op, i) => r.send({ type: 'req', id: 10 + i, op, args: {} }));
    for (let i = 0; i < 400 && r.msgs.filter((m) => m.id !== undefined).length < ops.length + 1; i++) await sleep(25);
    const cmd = r.msgs.find((m) => m.id === 1);
    if (cmd?.code !== 'unsupported') return `cmd: ${JSON.stringify(cmd)}`;
    for (let i = 0; i < ops.length; i++) {
      const m = r.msgs.find((x) => x.id === 10 + i);
      if (!m || m.ok !== false || m.code !== 'unsupported') return `${ops[i]}: ${JSON.stringify(m)} (got ${r.msgs.map((x) => x.id ?? x.type).join(',')})`;
    }
    return true;
  } finally {
    r.close();
  }
});

await check('wifi.scan lists networks', async () => {
  const nets = await s.scan();
  console.log(`        (${nets.length} found: ${nets.slice(0, 4).map((n) => n.ssid).join(', ')}${nets.length > 4 ? ', …' : ''})`);
  return nets.length > 0 && nets.every((n) => typeof n.ssid === 'string' && typeof n.rssi === 'number');
});

await check('setup.apply refuses a bad network, a bad password and a bad token without changing anything', async () => {
  const bad = [
    ['', 'goodpass1', 'a-fine-token-1'],
    ['home', 'short', 'a-fine-token-1'],
    ['home', 'goodpass1', 'short'],
  ] as const;
  for (const [a, b, c] of bad) if ((await kind(s.apply(a, b, c))) !== 'refused') return `accepted ${JSON.stringify([a, b, c.length])}`;
  return true;
});

await check('a wrong setup code is refused', async () => (await kind(newSetup('AAAAAAAAAAAA').connect())) === 'bad_code');
await check('a setup code that is not code-shaped is refused', async () => (await kind(newSetup('nope').connect())) === 'bad_code');

if (token) {
  await check('the normal token still works at the same time and gets the whole home', async () => {
    const c = new HubClient({ url, token, socketFactory: pinnedSocketFactory, ...(fingerprint ? { pinnedFingerprint: fingerprint } : {}), reconnect: false });
    c.connect();
    for (let i = 0; i < 100 && c.getSnapshot().status !== 'ready'; i++) await sleep(100);
    const st = c.getSnapshot();
    c.close();
    return st.status === 'ready' && st.entities.length > 0 ? true : st.status;
  });
  await check('a normal client is refused setup.apply', async () => {
    const c = new HubClient({ url, token, socketFactory: pinnedSocketFactory, ...(fingerprint ? { pinnedFingerprint: fingerprint } : {}), reconnect: false });
    c.connect();
    for (let i = 0; i < 100 && c.getSnapshot().status !== 'ready'; i++) await sleep(100);
    try {
      await c.req('setup.apply', { ssid: 'x', password: '', token: 'a-fine-token-1' });
      return 'accepted';
    } catch (e) {
      return (e as { code?: string }).code === 'unsupported' ? true : String(e);
    } finally {
      c.close();
    }
  });
}

if (flag('apply')) {
  const ssid = s.hub?.wifi?.ssid ?? arg('ssid');
  const pw = process.env.KS_SETUP_PASS;
  const next = arg('new-token') ?? token;
  if (!ssid || pw === undefined || !next) {
    console.log('  --apply needs the hub to be on a network (or --ssid), KS_SETUP_PASS in the environment and --token or --new-token');
    process.exit(2);
  }
  await check(`setup.apply with the current network (${ssid}) and the ${arg('new-token') ? 'new' : 'same'} token, then the hub comes back`, async () => {
    await s.apply(ssid, pw, next);
    // The hub answers, then restarts its server with the token and re-joins the network: wait for it to be reachable again.
    await sleep(2500);
    for (let i = 0; i < 90; i++) {
      const c = new HubClient({ url, token: next, socketFactory: pinnedSocketFactory, ...(fingerprint ? { pinnedFingerprint: fingerprint } : {}), reconnect: false });
      c.connect();
      for (let j = 0; j < 30 && !['ready', 'unauthorized', 'closed', 'untrusted'].includes(c.getSnapshot().status) && c.getSnapshot().status !== 'reconnecting'; j++) await sleep(100);
      const st = c.getSnapshot().status;
      c.close();
      if (st === 'ready') return true;
      await sleep(1000);
    }
    return 'the hub did not come back with the token';
  });
}

s.close();
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
