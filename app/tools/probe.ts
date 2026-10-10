// Connect to a real (or mock) hub with HubClient and print what happens. For checking
// a hub by hand:  npm run probe -- --url ws://192.168.0.202:8080/ws --token <token>
//   --seconds N        how long to listen (default 15)
//   --cmd E,A,V        send one command after the snapshot, e.g. --cmd 256,1,false
//   --cmd-delay N      wait N seconds before sending it
import { WebSocket } from 'ws';
import { HubClient, type SocketLike } from '../src/protocol/client';
import type { Value } from '../src/protocol/messages';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const url = arg('url') ?? 'ws://127.0.0.1:8080/ws';
const token = arg('token') ?? 'dev-token';
const seconds = Number(arg('seconds') ?? '15');
const cmd = arg('cmd');

const client = new HubClient({
  url,
  token,
  clientName: 'probe',
  socketFactory: (u) => new WebSocket(u) as unknown as SocketLike,
});

let last = client.getSnapshot();
const t0 = Date.now();
const stamp = () => `+${((Date.now() - t0) / 1000).toFixed(1)}s`;
client.subscribe(() => {
  const s = client.getSnapshot();
  if (s.status !== last.status) console.log(stamp(), 'status', last.status, '->', s.status, s.error ?? '');
  if (s.entities !== last.entities && s.status === 'ready') {
    for (const e of s.entities) {
      const old = last.entities.find((x) => x.id === e.id);
      for (const a of e.attrs) {
        const was = old?.attrs.find((x) => x.id === a.id)?.value;
        if (old && JSON.stringify(was) !== JSON.stringify(a.value)) {
          console.log(stamp(), `state ${e.name ?? '0x' + e.id.toString(16)}.${a.name}: ${JSON.stringify(was)} -> ${JSON.stringify(a.value)}`);
        }
      }
    }
  }
  if (s.status === 'ready' && last.status === 'ready') {
    for (const e of s.entities) {
      const old = last.entities.find((x) => x.id === e.id);
      if (!old) console.log(stamp(), `entity added: 0x${e.id.toString(16)} ${e.name ?? '-'}`);
      else if (old.name !== e.name) console.log(stamp(), `entity 0x${e.id.toString(16)} renamed: ${old.name} -> ${e.name}`);
    }
    if (s.rules.length !== last.rules.length) console.log(stamp(), `rules: ${last.rules.length} -> ${s.rules.length}`);
    if (s.schedules.length !== last.schedules.length) console.log(stamp(), `schedules: ${last.schedules.length} -> ${s.schedules.length}`, JSON.stringify(s.schedules));
    if (s.timers.length !== last.timers.length) console.log(stamp(), `timers: ${last.timers.length} -> ${s.timers.length}`);
  }
  for (const c of s.chips) {
    const old = last.chips.find((x) => x.id === c.id);
    if (old && JSON.stringify(old) !== JSON.stringify(c) && old.status !== c.status) {
      console.log(stamp(), `chip ${c.label}: ${old.status} -> ${c.status}`);
    }
  }
  last = s;
});

client.connect();
const waitReady = async () => {
  for (let i = 0; i < 100 && client.getSnapshot().status !== 'ready'; i++) {
    const st = client.getSnapshot().status;
    if (st === 'unauthorized' || st === 'incompatible') return false;
    await new Promise((r) => setTimeout(r, 100));
  }
  return client.getSnapshot().status === 'ready';
};

if (!(await waitReady())) {
  console.log(stamp(), 'not ready:', client.getSnapshot().status, client.getSnapshot().error ?? '');
  client.close();
  process.exit(1);
}
const s = client.getSnapshot();
console.log(stamp(), `ready: hub ${JSON.stringify(s.hub)}`);
console.log(`  ${s.entities.length} entities, ${s.chips.length} chips, ${s.rules.length} rules, ${s.schedules.length} schedules, ${s.timers.length} timers`);
for (const e of s.entities) {
  console.log(`  entity 0x${e.id.toString(16)} ${e.name ?? '-'} chip${e.chip} ${e.device}: ` + e.attrs.map((a) => `${a.name}=${JSON.stringify(a.value)}${a.flags.includes('w') ? '(w)' : ''}`).join(' '));
}
for (const c of s.chips) console.log(`  chip ${c.id} ${c.label} ${c.status} boot=${c.boot_id} reboots=${c.reboots} ${JSON.stringify(c.telemetry)}`);
for (const r of s.rules) console.log('  rule', JSON.stringify(r));
for (const x of s.schedules) console.log('  schedule', JSON.stringify(x));

const cmdDelay = Number(arg('cmd-delay') ?? '0');
if (cmd && cmdDelay > 0) await new Promise((r) => setTimeout(r, cmdDelay * 1000));
if (cmd) {
  const [e, a, v] = cmd.split(',');
  const value = JSON.parse(v ?? 'null') as Value;
  const t = Date.now();
  try {
    const status = await client.cmd(Number(e), Number(a), value);
    console.log(stamp(), `cmd ${cmd} -> ack ${status} after ${Date.now() - t} ms`);
  } catch (err) {
    console.log(stamp(), `cmd ${cmd} failed:`, (err as Error).message);
  }
}

await new Promise((r) => setTimeout(r, seconds * 1000));
client.close();
process.exit(0);
