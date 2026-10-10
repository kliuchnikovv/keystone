// Protocol conformance check against a running hub (the mock or the real one):
//   npm run hub-check -- --url ws://192.168.0.202:8080/ws --token <token> [--lockout] [--hold 20]
// Raw-socket tests of the behaviours docs/app-protocol.md promises. `--lockout` also
// exercises the failed-auth lockout (it locks THIS machine out of the hub for a
// minute, so it is off by default). `--hold N` keeps 3 clients connected for N
// seconds at the end, e.g. to read `heap` on the hub console.
import { WebSocket } from 'ws';
import { HubClient, type SocketLike } from '../src/protocol/client';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] ?? '' : undefined;
}
const url = arg('url') ?? 'ws://127.0.0.1:8080/ws';
const token = arg('token') ?? 'dev-token';
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
  const ws = new WebSocket(url);
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
  if (!(await r.next((m) => m.type === 'welcome'))) throw new Error('no welcome');
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
const snap = new HubClient({ url, token, clientName: 'hub-check', socketFactory: (u) => new WebSocket(u) as unknown as SocketLike });
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
  const clients = await Promise.all([login(), login(), login()]);
  console.log(`  holding ${clients.length} authenticated clients for ${hold} s (read heap on the hub now)`);
  await sleep(hold * 1000);
  clients.forEach((c) => c.ws.close());
}

a.ws.close();
await sleep(200);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
