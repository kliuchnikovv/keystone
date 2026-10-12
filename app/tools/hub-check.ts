// Protocol conformance check against a running hub (the mock or the real one):
//   npm run hub-check -- --url ws://192.168.0.202:8080/ws --token <token> [--lockout] [--hold 20]
//   npm run hub-check -- --url wss://192.168.0.202:8443/ws --token <token> --fingerprint <sha256>
// For wss:// the certificate is pinned (--fingerprint is required) and extra TLS checks run.
// `--wifi` also moves the hub to a network that does not exist and checks that it comes back (~45 s, disruptive).
// Raw-socket tests of the behaviours docs/app-protocol.md promises. `--lockout` also
// exercises the failed-auth lockout (it locks THIS machine out of the hub for a
// minute, so it is off by default). `--mutate` also exercises the management requests
// (names, rules, schedules, timers), putting back everything it creates. `--hold N` keeps 3 clients connected for N
// seconds at the end, e.g. to read `heap` on the hub console.
import { WebSocket } from 'ws';
import { HubClient } from '../src/protocol/client';
import { sameFingerprint } from '../src/protocol/fingerprint';
import { pinnedSocketFactory, tlsInfo } from './pinned-socket';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] ?? '' : undefined;
}
const url = arg('url') ?? 'ws://127.0.0.1:8080/ws';
const token = arg('token') ?? 'dev-token';
const fingerprint = arg('fingerprint');
const secure = /^wss:/i.test(url);
if (secure && !fingerprint) {
  console.error('wss:// needs --fingerprint <sha256> (the hub console prints it with `webtoken`)');
  process.exit(2);
}
const newClient = (name: string) =>
  new HubClient({ url, token, clientName: name, socketFactory: pinnedSocketFactory, ...(fingerprint ? { pinnedFingerprint: fingerprint } : {}) });
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

interface Raw {
  ws: WebSocket;
  msgs: Record<string, unknown>[];
  closed: Promise<void>;
  isClosed: () => boolean;
  send: (o: unknown) => void;
  next: (pred: (m: Record<string, unknown>) => boolean, ms?: number) => Promise<Record<string, unknown> | null>;
}

async function raw(): Promise<Raw> {
  // Raw protocol tests: for wss:// the chain error of a self-signed certificate is expected;
  // the certificate itself is pinned by the TLS checks below, before anything else runs.
  const ws = new WebSocket(url, secure ? { rejectUnauthorized: false } : {});
  const msgs: Record<string, unknown>[] = [];
  let closed = false;
  const closedP = new Promise<void>((r) => ws.on('close', () => { closed = true; r(); }));
  ws.on('message', (d) => { try { msgs.push(JSON.parse(d.toString())); } catch { /* ignore */ } });
  ws.on('error', () => { /* close follows */ });
  await new Promise<void>((resolve, reject) => { ws.once('open', () => resolve()); ws.once('error', reject); });
  const r: Raw = {
    ws, msgs, closed: closedP, isClosed: () => closed,
    send: (o) => ws.send(typeof o === 'string' ? o : JSON.stringify(o)),
    next: async (pred, ms = 3000) => {
      const t0 = Date.now();
      let seen = 0;
      while (Date.now() - t0 < ms) {
        for (; seen < msgs.length; seen++) if (pred(msgs[seen]!)) return msgs[seen]!;
        await sleep(20);
      }
      return null;
    },
  };
  return r;
}

async function login(): Promise<Raw> {
  const r = await raw();
  r.send({ type: 'hello', v: 1, token });
  // Parallel TLS handshakes queue up in the hub's single server task (~0.75 s each), so a
  // hello can wait a couple of seconds for its turn.
  if (!(await r.next((m) => m.type === 'welcome', 8000))) throw new Error('no welcome');
  // Wait for the end of the snapshot.
  if (!(await r.next((m) => m.type === 'snapshot' && (m.last ?? true) === true, 5000))) throw new Error('no snapshot end');
  return r;
}

let pass = 0, fail = 0;
async function check(name: string, fn: () => Promise<boolean | string>): Promise<void> {
  try {
    const res = await fn();
    if (res === true) { pass++; console.log(`  ok    ${name}`); }
    else { fail++; console.log(`  FAIL  ${name}${typeof res === 'string' ? ': ' + res : ''}`); }
  } catch (e) { fail++; console.log(`  FAIL  ${name}: ${(e as Error).message}`); }
}
const code = (m: Record<string, unknown> | null) => (m ? String(m.code) : 'no reply');

console.log(`hub-check ${url}`);

if (secure) {
  const u = new URL(url);
  const host = u.hostname;
  const port = Number(u.port || 443);
  let info: Awaited<ReturnType<typeof tlsInfo>> | null = null;
  await check('TLS: the hub presents the pinned certificate', async () => {
    info = await tlsInfo(host, port);
    return sameFingerprint(info.fingerprint, fingerprint!) ? true : `presented ${info.fingerprint}`;
  });
  if (!info || !sameFingerprint((info as { fingerprint: string }).fingerprint, fingerprint!)) {
    console.log('\nThe certificate is not the pinned one: stopping before any token is sent.');
    process.exit(1);
  }
  await check('TLS: protocol is 1.2 or newer with an AEAD cipher', async () => {
    const i = info!;
    const ok = (i.protocol === 'TLSv1.2' || i.protocol === 'TLSv1.3') && /GCM|CHACHA20|CCM/.test(i.cipher ?? '');
    return ok ? true : `${i.protocol} ${i.cipher}`;
  });
  await check('TLS: the certificate is within its validity period', async () => {
    const i = info!;
    const now = Date.now();
    return i.validFrom.getTime() <= now && now < i.validTo.getTime() ? true : `${i.validFrom.toISOString()} .. ${i.validTo.toISOString()}`;
  });
  await check('TLS: a wrong pin is refused and ends in "untrusted"', async () => {
    const c = new HubClient({ url, token, socketFactory: pinnedSocketFactory, pinnedFingerprint: 'AA:'.repeat(31) + 'AA' });
    c.connect();
    for (let i = 0; i < 50 && c.getSnapshot().status !== 'untrusted'; i++) await sleep(100);
    const st = c.getSnapshot().status;
    c.close();
    return st === 'untrusted' ? true : st;
  });
  await check('TLS: plain ws:// to the TLS port gets no WebSocket', async () => {
    const r = await new Promise<string>((resolve) => {
      const w = new WebSocket(`ws://${host}:${port}/ws`);
      const t = setTimeout(() => { w.terminate(); resolve('closed'); }, 3000);
      w.on('open', () => { clearTimeout(t); w.close(); resolve('OPEN'); });
      w.on('error', () => { clearTimeout(t); resolve('refused'); });
      w.on('close', () => { clearTimeout(t); resolve('closed'); });
    });
    return r === 'OPEN' ? 'the TLS port answered a plain WebSocket' : true;
  });
}

if (process.argv.includes('--only-hold')) {
  const secs = Number(arg('hold') ?? '20');
  const clients = await Promise.all([login(), login(), login()]);
  console.log(`  holding ${clients.length} authenticated clients for ${secs} s`);
  await sleep(secs * 1000);
  clients.forEach((c) => c.ws.close());
  await sleep(300);
  process.exit(0);
}

await check('hello with the right token: welcome, then a snapshot ending with last=true', async () => {
  const r = await login();
  const welcome = r.msgs.find((m) => m.type === 'welcome') as { v: number; hub: Record<string, unknown> } | undefined;
  const ok = welcome?.v === 1 && typeof welcome.hub?.name === 'string' && typeof welcome.hub?.fw === 'string';
  const order = r.msgs.findIndex((m) => m.type === 'welcome') < r.msgs.findIndex((m) => m.type === 'snapshot');
  r.ws.close();
  return ok && order ? true : 'bad welcome shape or order';
});

await check('wrong token: error unauthorized, then closed', async () => {
  const r = await raw();
  r.send({ type: 'hello', v: 1, token: 'definitely-wrong' });
  const e = await r.next((m) => m.type === 'error');
  await Promise.race([r.closed, sleep(2000)]);
  return code(e) === 'unauthorized' && r.isClosed() ? true : `got ${code(e)}, closed=${r.isClosed()}`;
});

await check('hello without a token: unauthorized', async () => {
  const r = await raw();
  r.send({ type: 'hello', v: 1 });
  const e = await r.next((m) => m.type === 'error');
  return code(e) === 'unauthorized' ? true : code(e);
});

await check('unsupported protocol version: error version', async () => {
  const r = await raw();
  r.send({ type: 'hello', v: 99, token });
  const e = await r.next((m) => m.type === 'error');
  return code(e) === 'version' ? true : code(e);
});

await check('a command before hello is refused', async () => {
  const r = await raw();
  r.send({ type: 'cmd', id: 1, entity: 1, attr: 1, value: true });
  const e = await r.next((m) => m.type === 'error');
  return code(e) === 'unauthorized' ? true : code(e);
});

await check('garbage before hello: bad_json or unauthorized, and the socket closes', async () => {
  const r = await raw();
  r.send('this is not json');
  const e = await r.next((m) => m.type === 'error');
  await Promise.race([r.closed, sleep(2000)]);
  return (code(e) === 'bad_json' || code(e) === 'unauthorized') && r.isClosed() ? true : `${code(e)} closed=${r.isClosed()}`;
});

await check('no hello within the timeout: the hub closes the connection (~5 s)', async () => {
  const r = await raw();
  const t0 = Date.now();
  await Promise.race([r.closed, sleep(9000)]);
  const dt = Date.now() - t0;
  return r.isClosed() && dt >= 3000 && dt <= 8000 ? true : `closed=${r.isClosed()} after ${dt} ms`;
});

// ---- authenticated behaviour ----
const a = await login();
const snap = newClient('hub-check');
snap.connect();
for (let i = 0; i < 50 && snap.getSnapshot().status !== 'ready'; i++) await sleep(100);
const ents = snap.getSnapshot().entities;
const writable = ents.flatMap((e) => e.attrs.filter((x) => x.flags.includes('w')).map((x) => ({ e, x })))[0];
const readOnly = ents.flatMap((e) => e.attrs.filter((x) => !x.flags.includes('w')).map((x) => ({ e, x })))[0];
snap.close();
console.log(`  (using writable ${writable ? writable.e.id + '.' + writable.x.name : 'none'}, read-only ${readOnly ? readOnly.e.id + '.' + readOnly.x.name : 'none'})`);

await check('unknown message type: error unsupported, connection stays open', async () => {
  a.send({ type: 'subscribe', id: 5 });
  const e = await a.next((m) => m.type === 'error' && m.code === 'unsupported');
  return e && !a.isClosed() ? true : code(e);
});
await check('bad JSON after hello: error bad_json, connection stays open', async () => {
  a.send('{"type":"cmd"');
  const e = await a.next((m) => m.type === 'error' && m.code === 'bad_json');
  return e && !a.isClosed() ? true : code(e);
});
await check('cmd without an id: error bad_args', async () => {
  a.send({ type: 'cmd', entity: writable?.e.id ?? 1, attr: writable?.x.id ?? 1, value: true });
  const e = await a.next((m) => m.type === 'error' && m.id === undefined && m.code === 'bad_args');
  return e ? true : 'no bad_args';
});
await check('cmd to an unknown entity: error not_found with the id', async () => {
  a.send({ type: 'cmd', id: 11, entity: 65000, attr: 1, value: true });
  const e = await a.next((m) => m.type === 'error' && m.id === 11);
  return code(e) === 'not_found' ? true : code(e);
});
await check('cmd to an unknown attribute: not_found', async () => {
  if (!writable) return 'no writable attribute to test with';
  a.send({ type: 'cmd', id: 12, entity: writable.e.id, attr: 0xee, value: true });
  const e = await a.next((m) => m.type === 'error' && m.id === 12);
  return code(e) === 'not_found' ? true : code(e);
});
await check('cmd to a read-only attribute: read_only', async () => {
  if (!readOnly) return 'no read-only attribute to test with';
  a.send({ type: 'cmd', id: 13, entity: readOnly.e.id, attr: readOnly.x.id, value: 1 });
  const e = await a.next((m) => m.type === 'error' && m.id === 13);
  return code(e) === 'read_only' ? true : code(e);
});
await check('cmd with a value of the wrong type: bad_args', async () => {
  if (!writable) return 'no writable attribute to test with';
  a.send({ type: 'cmd', id: 14, entity: writable.e.id, attr: writable.x.id, value: 'yes' });
  const e = await a.next((m) => m.type === 'error' && m.id === 14);
  return code(e) === 'bad_args' ? true : code(e);
});
await check('a valid cmd: accepted, then ack (value unchanged: re-sends the current value)', async () => {
  if (!writable) return 'no writable attribute to test with';
  const cur = writable.x.value;
  if (typeof cur !== 'boolean') return 'writable attribute is not a bool; skipped';
  a.send({ type: 'cmd', id: 15, entity: writable.e.id, attr: writable.x.id, value: cur });
  const acc = await a.next((m) => m.type === 'accepted' && m.id === 15);
  const ack = await a.next((m) => m.type === 'ack' && m.id === 15, 8000);
  return acc && ack && ack.status === 'ok' ? true : `accepted=${!!acc} ack=${ack ? ack.status : 'none'}`;
});
await check('an unknown request op: res ok=false unsupported', async () => {
  a.send({ type: 'req', id: 21, op: 'no.such.op' });
  const r = await a.next((m) => m.type === 'res' && m.id === 21);
  return r && r.ok === false && r.code === 'unsupported' ? true : JSON.stringify(r);
});
await check('a frame over 4096 bytes: the hub closes the connection', async () => {
  const big = await login();
  big.send(JSON.stringify({ type: 'cmd', id: 1, entity: 1, attr: 1, value: true, pad: 'x'.repeat(5000) }));
  await Promise.race([big.closed, sleep(3000)]);
  const closed = big.isClosed();
  big.ws.close();
  return closed ? true : 'still open';
});
await check('a binary frame is rejected', async () => {
  const b = await login();
  b.ws.send(Buffer.from([1, 2, 3]));
  const e = await b.next((m) => m.type === 'error', 2000);
  await Promise.race([b.closed, sleep(1500)]);
  const ok = e || b.isClosed();
  b.ws.close();
  await sleep(300);  // let the hub notice before the next test counts clients
  return ok ? true : 'no error and still open';
});

await check('the 4th simultaneous client is refused with busy; a slot frees up afterwards', async () => {
  const held: Raw[] = [a];
  const extra = await login(); held.push(extra);
  const third = await login(); held.push(third);
  const fourth = await raw();
  fourth.send({ type: 'hello', v: 1, token });
  const e = await fourth.next((m) => m.type === 'error' || m.type === 'welcome', 3000);
  const refused = code(e) === 'busy';
  third.ws.close();
  await sleep(700);
  const again = await raw();
  again.send({ type: 'hello', v: 1, token });
  const w = await again.next((m) => m.type === 'welcome', 3000);
  extra.ws.close(); again.ws.close(); fourth.ws.close();
  return refused && w ? true : `4th got ${code(e)}, retry welcome=${!!w}`;
});

if (process.argv.includes('--mutate')) {
  // Management requests change the hub, so they only run on request, and each test
  // puts back what it touched. The rule uses a condition that cannot become true.
  const c = newClient('hub-check');
  c.connect();
  for (let i = 0; i < 50 && c.getSnapshot().status !== 'ready'; i++) await sleep(100);
  const waitFor = async (fn: () => boolean, ms = 4000) => {
    for (let t = 0; t < ms && !fn(); t += 50) await sleep(50);
    return fn();
  };
  const target = c.getSnapshot().entities.find((e) => e.attrs.some((x) => x.id === 0x01 && x.flags.includes('w')) && e.attrs.some((x) => x.id === 0x13));
  const before = { rules: c.getSnapshot().rules.length, schedules: c.getSnapshot().schedules.length, timers: c.getSnapshot().timers.length, scenes: c.getSnapshot().scenes.length };
  const code = (p: Promise<unknown>) => p.then(() => 'ok', (e: { code?: string }) => String(e.code));
  if (!target) {
    console.log('  (--mutate needs a device with a writable on/off and power_w; skipped)');
  } else {
    const orig = target.name;
    const ent = target.id;
    const nameOf = () => c.getSnapshot().entities.find((e) => e.id === ent)?.name ?? null;

    await check('name.set renames a device and the hub pushes the new picture; the old name is restored', async () => {
      try {
        await c.req('name.set', { entity: ent, name: 'chk_tmp' });
        return (await waitFor(() => nameOf() === 'chk_tmp')) ? true : 'rename not pushed';
      } finally {
        if (orig !== null) await c.req('name.set', { entity: ent, name: orig });
        else await c.req('name.clear', { entity: ent });
        await waitFor(() => nameOf() === orig);
      }
    });
    await check('name.set rejects bad names, unknown entities and duplicates', async () => {
      const got = [
        await code(c.req('name.set', { entity: ent, name: 'Bad Name' })),
        await code(c.req('name.set', { entity: ent, name: '0x10' })),
        await code(c.req('name.set', { entity: ent, name: '' })),
        await code(c.req('name.set', { entity: 65000, name: 'ghost' })),
        await code(c.req('name.set', { name: 'x' })),
      ];
      return JSON.stringify(got) === JSON.stringify(['bad_args', 'bad_args', 'bad_args', 'not_found', 'bad_args']) ? true : got.join(',');
    });
    await check('schedule.add returns its index, shows up for everyone, schedule.del removes it', async () => {
      const r = await c.req('schedule.add', { time: '03:33', days: 'weekdays', entity: ent, action: 'off' });
      const idx = r.index as number;
      try {
        if (typeof idx !== 'number') return 'no index in the result';
        const shown = await waitFor(() => c.getSnapshot().schedules.some((x) => x.index === idx && x.time === '03:33' && x.days === 'weekdays'));
        return shown ? true : 'not listed';
      } finally {
        if (typeof idx === 'number') await c.req('schedule.del', { index: idx });
        await waitFor(() => c.getSnapshot().schedules.length === before.schedules);
      }
    });
    await check('schedule.add with a date is a one-off (days "once"); a past date, a bad date, or date + days is refused', async () => {
      const r = await c.req('schedule.add', { time: '03:33', date: '2099-12-31', entity: ent, action: 'off' });
      const idx = r.index as number;
      try {
        if (typeof idx !== 'number') return 'no index in the result';
        const shown = await waitFor(() => c.getSnapshot().schedules.some((x) => x.index === idx && x.days === 'once' && x.date === '2099-12-31'));
        if (!shown) return 'not listed as a one-off';
        const got = [
          await code(c.req('schedule.add', { time: '03:33', date: '2001-01-01', entity: ent, action: 'off' })),
          await code(c.req('schedule.add', { time: '03:33', date: '2099-02-30', entity: ent, action: 'off' })),
          await code(c.req('schedule.add', { time: '03:33', date: '2099-12-31', days: 'daily', entity: ent, action: 'off' })),
        ];
        return got.every((x) => x === 'bad_args') ? true : got.join(',');
      } finally {
        if (typeof idx === 'number') await c.req('schedule.del', { index: idx });
        await waitFor(() => c.getSnapshot().schedules.length === before.schedules);
      }
    });
    await check('timer.add / timer.cancel', async () => {
      const r = await c.req('timer.add', { entity: ent, action: 'off', delay_s: 3000 });
      const idx = r.index as number;
      try {
        const shown = await waitFor(() => c.getSnapshot().timers.length === before.timers + 1);
        const t = c.getSnapshot().timers.find((x) => x.index === idx);
        return shown && t && t.left_s > 2000 && t.left_s <= 3000 ? true : `listed=${shown} left=${t?.left_s}`;
      } finally {
        if (typeof idx === 'number') await c.req('timer.cancel', { index: idx });
        await waitFor(() => c.getSnapshot().timers.length === before.timers);
      }
    });
    await check('rule.add with every constraint is stored as given; rule.del removes it', async () => {
      const rule = {
        when: { entity: ent, attr: 0x13, op: '>', value: 65000 }, // a plug cannot draw 65 kW
        then: { entity: ent, attr: 0x01, value: false },
        between: { from: '18:00', to: '23:00' },
        and: { entity: ent, attr: 0x13, op: '<', value: 65500 },
        cooldown_s: 600,
      };
      const r = await c.req('rule.add', rule);
      const idx = r.index as number;
      try {
        const shown = await waitFor(() => c.getSnapshot().rules.length === before.rules + 1);
        const stored = c.getSnapshot().rules.find((x) => x.index === idx);
        const same = stored && JSON.stringify({ ...stored, index: undefined }) === JSON.stringify(rule);
        return shown && same ? true : `listed=${shown} stored=${JSON.stringify(stored)}`;
      } finally {
        if (typeof idx === 'number') await c.req('rule.del', { index: idx });
        await waitFor(() => c.getSnapshot().rules.length === before.rules);
      }
    });
    await check('bad arguments get specific codes', async () => {
      const ok = { when: { entity: ent, attr: 0x13, op: '>', value: 65000 }, then: { entity: ent, attr: 0x01, value: false } };
      const got = [
        await code(c.req('rule.add', { ...ok, cooldown_s: 0 })),
        await code(c.req('rule.add', { ...ok, between: { from: '10:00', to: '10:00' } })),
        await code(c.req('rule.add', { ...ok, when: { ...ok.when, entity: 65000 } })),
        await code(c.req('rule.add', { ...ok, then: { entity: ent, attr: 0x13, value: 1 } })),
        await code(c.req('rule.del', { index: 99 })),
        await code(c.req('schedule.add', { time: '25:00', days: 'daily', entity: ent, action: 'off' })),
        await code(c.req('schedule.del', { index: 99 })),
        await code(c.req('timer.add', { entity: ent, action: 'off', delay_s: 0 })),
        await code(c.req('timer.cancel', { index: 99 })),
      ];
      const want = ['bad_args', 'bad_args', 'not_found', 'read_only', 'not_found', 'bad_args', 'not_found', 'bad_args', 'not_found'];
      return JSON.stringify(got) === JSON.stringify(want) ? true : got.join(',');
    });
    // Zigbee chip operations. Nothing here removes a device: unpair is only checked with arguments
    // that must be refused. A pair of 1 s is harmless; identify makes the plug blink for 10 s.
    await check('pair / identify / unpair validate their arguments', async () => {
      const chipEntity = c.getSnapshot().chips[0] ? c.getSnapshot().chips[0]!.id : 1; // a chip's self entity is its chip id
      const got = [
        await code(c.req('pair', { seconds: 0 })),
        await code(c.req('pair', { seconds: 255 })),
        await code(c.req('pair', { seconds: '60' })),
        await code(c.req('identify', {})),
        await code(c.req('identify', { entity: 65000 })),
        await code(c.req('identify', { entity: chipEntity })),
        await code(c.req('unpair', { entity: 65000 })),
        await code(c.req('unpair', { entity: chipEntity })),
      ];
      const want = ['bad_args', 'bad_args', 'bad_args', 'bad_args', 'not_found', 'not_found', 'not_found', 'not_found'];
      return JSON.stringify(got) === JSON.stringify(want) ? true : got.join(',');
    });
    await check('pair for 1 s and identify a switch are accepted', async () => {
      const r1 = await c.req('pair', { seconds: 1 }).then(() => true, () => false);
      const r2 = await c.req('identify', { entity: ent }).then(() => true, () => false);
      return r1 && r2 ? true : `pair ${r1}, identify ${r2}`;
    });
    // Scenes. The scene re-sends the switch's current value, so nothing visibly changes.
    await check('scene.put / apply / replace / del', async () => {
      const sw = c.getSnapshot().entities.find((e) => e.id === ent)!.attrs.find((a) => a.id === 0x01)!;
      const action = { entity: ent, attr: 0x01, value: sw.value };
      await c.req('scene.put', { name: 'chk_scene', actions: [action] });
      if (!(await waitFor(() => c.getSnapshot().scenes.some((s) => s.name === 'chk_scene')))) return 'scene not listed';
      const res = await c.req('scene.apply', { name: 'chk_scene' });
      if (res?.sent !== 1 || res?.skipped !== 0) return `apply counts ${JSON.stringify(res)}`;
      await c.req('scene.put', { name: 'chk_scene', actions: [action, { entity: ent, attr: 0x13, value: 1 }].slice(0, 1) });
      if (c.getSnapshot().scenes.filter((s) => s.name === 'chk_scene').length !== 1) return 'replace duplicated it';
      await c.req('scene.del', { name: 'chk_scene' });
      return (await waitFor(() => !c.getSnapshot().scenes.some((s) => s.name === 'chk_scene'))) ? true : 'scene not removed';
    });
    await check('scene requests validate their arguments', async () => {
      const ok = { entity: ent, attr: 0x01, value: true };
      const got = [
        await code(c.req('scene.put', { actions: [ok] })),
        await code(c.req('scene.put', { name: 'Bad', actions: [ok] })),
        await code(c.req('scene.put', { name: 'x', actions: [] })),
        await code(c.req('scene.put', { name: 'x', actions: [ok, ok] })),
        await code(c.req('scene.put', { name: 'x', actions: [{ ...ok, value: 5 }] })),
        await code(c.req('scene.put', { name: 'x', actions: [{ entity: ent, attr: 0x13, value: 1 }] })),
        await code(c.req('scene.put', { name: 'x', actions: [{ ...ok, entity: 65000 }] })),
        await code(c.req('scene.apply', { name: 'chk_nope' })),
        await code(c.req('scene.del', { name: 'chk_nope' })),
      ];
      const want = ['bad_args', 'bad_args', 'bad_args', 'bad_args', 'bad_args', 'read_only', 'not_found', 'not_found', 'not_found'];
      return JSON.stringify(got) === JSON.stringify(want) && c.getSnapshot().scenes.length === before.scenes ? true : got.join(',');
    });
    // A schedule (never fires: 03:33 daily, removed at once), a timer (an hour) and a rule (power > 65000 W)
    // that apply a scene; the scene is kept while any of them uses it.
    await check('schedule / timer / rule can apply a scene; a scene in use cannot be deleted', async () => {
      const sw = c.getSnapshot().entities.find((e) => e.id === ent)!.attrs.find((a) => a.id === 0x01)!;
      await c.req('scene.put', { name: 'chk_scene', actions: [{ entity: ent, attr: 0x01, value: sw.value }] });
      const sched = await c.req('schedule.add', { time: '03:33', days: 'daily', scene: 'chk_scene' });
      const timer = await c.req('timer.add', { delay_s: 3600, scene: 'chk_scene' });
      const rule = await c.req('rule.add', { when: { entity: ent, attr: 0x13, op: '>', value: 65000 }, then: { scene: 'chk_scene' } });
      try {
        const listed = await waitFor(() => {
          const s = c.getSnapshot();
          return s.schedules.some((x) => x.scene === 'chk_scene') && s.timers.some((x) => x.scene === 'chk_scene') &&
            s.rules.some((x) => 'scene' in x.then && x.then.scene === 'chk_scene');
        });
        if (!listed) return 'not listed with the scene name';
        const refused = await code(c.req('scene.del', { name: 'chk_scene' }));
        if (refused !== 'bad_args') return `delete in use gave ${refused}`;
        const unknown = [
          await code(c.req('schedule.add', { time: '03:33', days: 'daily', scene: 'chk_nope' })),
          await code(c.req('schedule.add', { time: '03:33', days: 'daily', scene: 'chk_scene', entity: ent })),
          await code(c.req('timer.add', { delay_s: 5, scene: 'chk_nope' })),
          await code(c.req('rule.add', { when: { entity: ent, attr: 0x13, op: '>', value: 65000 }, then: { scene: 'chk_nope' } })),
        ];
        return JSON.stringify(unknown) === JSON.stringify(['not_found', 'bad_args', 'not_found', 'not_found']) ? true : unknown.join(',');
      } finally {
        await c.req('rule.del', { index: Number(rule?.index ?? 0) }).catch(() => undefined);
        await c.req('timer.cancel', { index: Number(timer?.index ?? 0) }).catch(() => undefined);
        await c.req('schedule.del', { index: Number(sched?.index ?? 0) }).catch(() => undefined);
        await c.req('scene.del', { name: 'chk_scene' }).catch(() => undefined);
      }
    });
    // The location and sun-relative times. The previous location is put back.
    await check('location.set / clear reach every client; sun times can be used in schedules and rule windows', async () => {
      const prev = c.getSnapshot().hub?.location ?? null;
      let idx: number | null = null;
      let ridx: number | null = null;
      try {
        await c.req('location.set', { lat: 50.08, lon: 14.43 });
        if (!(await waitFor(() => c.getSnapshot().hub?.location?.lat === 50.08))) return 'location not pushed';
        const sun = c.getSnapshot().hub?.sun;
        if (c.getSnapshot().hub?.synced && !sun) return 'no sun times although the clock is synced';
        if (sun && !(/^\d\d:\d\d$/.test(sun.rise) && /^\d\d:\d\d$/.test(sun.set))) return `odd sun times ${JSON.stringify(sun)}`;
        idx = Number((await c.req('schedule.add', { time: 'sunset+30', days: 'daily', entity: ent, action: 'on' }))?.index);
        ridx = Number((await c.req('rule.add', { when: { entity: ent, attr: 0x13, op: '>', value: 65000 }, then: { entity: ent, attr: 0x01, value: true }, between: { from: 'sunset', to: '23:00' } }))?.index);
        const listed = await waitFor(() => c.getSnapshot().schedules.some((s) => s.time === 'sunset+30') && c.getSnapshot().rules.some((r) => r.between?.from === 'sunset'));
        if (!listed) return 'sun-relative entries not listed as written';
        const bad = [
          await code(c.req('location.set', { lat: 91, lon: 0 })),
          await code(c.req('location.set', { lat: 0 })),
          await code(c.req('schedule.add', { time: 'sunset+181', days: 'daily', entity: ent, action: 'on' })),
          await code(c.req('schedule.add', { time: 'Sunset', days: 'daily', entity: ent, action: 'on' })),
          await code(c.req('rule.add', { when: { entity: ent, attr: 0x13, op: 'any' }, then: { entity: ent, attr: 0x01, value: true }, between: { from: 'sunset', to: 'sunset' } })),
        ];
        return bad.every((x) => x === 'bad_args') ? true : bad.join(',');
      } finally {
        if (ridx !== null) await c.req('rule.del', { index: ridx }).catch(() => undefined);
        if (idx !== null) await c.req('schedule.del', { index: idx }).catch(() => undefined);
        if (prev) await c.req('location.set', prev).catch(() => undefined);
        else await c.req('location.clear').catch(() => undefined);
      }
    });
    // Wi-Fi: a scan (which blocks the hub for a couple of seconds) and argument validation. Nothing here
    // changes the network; `--wifi` below does, on purpose.
    await check('wifi.scan lists networks, strongest first, one per name; wifi.set validates its arguments', async () => {
      const res = await c.req('wifi.scan');
      const nets = res?.networks as { ssid: string; rssi: number; secure: boolean }[] | undefined;
      if (!Array.isArray(nets)) return 'no networks array';
      if (nets.some((n, i) => i > 0 && n.rssi > nets[i - 1]!.rssi)) return 'not sorted strongest first';
      if (new Set(nets.map((n) => n.ssid)).size !== nets.length) return 'a name appears twice';
      const here = c.getSnapshot().hub?.wifi?.ssid;
      if (here && !nets.some((n) => n.ssid === here)) return `the network the hub is on (${here}) is not in the scan`;
      const bad = [
        await code(c.req('wifi.set', { ssid: '', password: '12345678' })),
        await code(c.req('wifi.set', { ssid: 'x', password: 'short' })),
        await code(c.req('wifi.set', { ssid: 'x' })),
        await code(c.req('wifi.set', { ssid: 's'.repeat(33), password: '' })),
      ];
      return bad.every((x) => x === 'bad_args') ? true : bad.join(',');
    });
    await check('everything the tests created is gone again', async () => {
      const ok = await waitFor(() => {
        const s = c.getSnapshot();
        return s.rules.length === before.rules && s.schedules.length === before.schedules && s.timers.length === before.timers && s.scenes.length === before.scenes && nameOf() === orig;
      });
      return ok ? true : 'leftovers';
    });
  }
  c.close();
}

if (process.argv.includes('--wifi')) {
  // Moves the hub to a network that does not exist: it must drop the connection, fail to join, and come
  // back on its own network within about a minute. Takes ~45 s and the hub is unreachable for ~35 s.
  await check('wifi.set to a network that does not exist: the hub goes back to its own', async () => {
    const c = newClient('hub-check-wifi');
    c.connect();
    for (let i = 0; i < 100 && c.getSnapshot().status !== 'ready'; i++) await sleep(100);
    const before = c.getSnapshot().hub?.wifi?.ssid;
    if (!before) { c.close(); return 'the hub reports no Wi-Fi network (is it on Wi-Fi?)'; }
    const hubBefore = c.getSnapshot().hub;
    await c.req('wifi.set', { ssid: 'hub-check-no-such-network', password: '12345678' });
    // While the hub tries the other network it is unreachable. The TCP connection may survive the gap (the
    // hub keeps its state) or break and be re-made; either way, once the hub is back it announces itself with
    // a fresh `hub` event or a new welcome, naming the network it is on.
    let dropped = false;
    const t0 = Date.now();
    let back = false;
    while (Date.now() - t0 < 90_000) {
      const st = c.getSnapshot().status;
      if (st !== 'ready') dropped = true;
      const h = c.getSnapshot().hub;
      const secs = (Date.now() - t0) / 1000;
      if (secs >= 20 && st === 'ready' && h !== hubBefore && h?.wifi?.ssid === before) {
        back = true;
        break;
      }
      await sleep(500);
    }
    const after = c.getSnapshot().hub?.wifi?.ssid;
    const secs = Math.round((Date.now() - t0) / 1000);
    c.close();
    if (!back) return `no sign of the hub on ${before} after ${secs} s (now: ${after ?? 'nothing'})`;
    return secs <= 80 ? `ok` === 'ok' : `came back after ${secs} s (expected ~35)`;
  });
}

if (process.argv.includes('--lockout')) {
  await check('5 failed logins lock the address out, even for the right token', async () => {
    for (let i = 0; i < 5; i++) {
      const r = await raw();
      r.send({ type: 'hello', v: 1, token: 'wrong-wrong-' + i });
      await r.next((m) => m.type === 'error');
      r.ws.close();
      await sleep(100);
    }
    const r = await raw();
    r.send({ type: 'hello', v: 1, token });
    const e = await r.next((m) => m.type === 'error' || m.type === 'welcome');
    return code(e) === 'unauthorized' ? true : `got ${e ? String(e.type) : 'nothing'}`;
  });
}

const hold = Number(arg('hold') ?? '0');
if (hold > 0 && !process.argv.includes('--lockout')) {
  a.ws.close();  // `a` still holds a slot from the checks above; the hold wants all three
  await sleep(700);
  const clients = await Promise.all([login(), login(), login()]);
  console.log(`  holding ${clients.length} authenticated clients for ${hold} s (read heap on the hub now)`);
  await sleep(hold * 1000);
  clients.forEach((c) => c.ws.close());
}

a.ws.close();
await sleep(200);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
