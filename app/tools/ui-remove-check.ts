// The app's own path to the hub, without a phone: the same HubClient the app uses,
// over plain ws:// (the hub's development mode). It checks what the Remove device
// button does — `unpair` for an entity of the Matter pseudo-chip — and that the
// entity really leaves the snapshot.
//
// Usage:
//   npx tsx tools/ui-remove-check.ts --url ws://<hub>:8080/ws --token <token>
import { HubClient } from '../src/protocol/client';

function arg(name: string, fallback = ''): string {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && process.argv[i + 1] !== undefined ? process.argv[i + 1]! : fallback;
}

const url = arg('url', 'ws://127.0.0.1:8080/ws');
const token = arg('token', 'dev-token');

const client = new HubClient({ url, token, clientName: 'ui-remove-check', reconnect: false });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function waitReady(timeoutMs = 10_000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (client.getSnapshot().status === 'ready') return;
    await sleep(200);
  }
  throw new Error(`the hub did not become ready (status ${client.getSnapshot().status})`);
}

async function main(): Promise<void> {
  client.connect();
  await waitReady();
  const snap = client.getSnapshot();
  console.log(`connected to ${url}`);
  console.log('entities:', snap.entities.map((e) => `${e.id} ${e.name ?? e.device} (chip ${e.chip})`).join(', ') || 'none');

  const matter = snap.entities.filter((e) => e.chip === 3);
  if (matter.length === 0) {
    console.log('no Matter device on the hub — nothing to remove');
    client.close();
    return;
  }
  const target = matter[0]!;
  console.log(`removing the Matter device ${target.id} (${target.name ?? target.device}) — the app's Remove button`);

  try {
    const res = await client.req('unpair', { entity: target.id });
    console.log('unpair answered:', JSON.stringify(res));
  } catch (err) {
    console.log('unpair failed:', (err as Error).message);
    client.close();
    process.exitCode = 1;
    return;
  }

  // The hub re-sends the snapshot when a device goes away.
  const deadline = Date.now() + 8000;
  let gone = false;
  while (Date.now() < deadline) {
    if (!client.getSnapshot().entities.some((e) => e.id === target.id)) {
      gone = true;
      break;
    }
    await sleep(200);
  }
  const after = client.getSnapshot().entities;
  console.log(gone ? 'the device left the snapshot — as the app expects' : 'the device is STILL in the snapshot');
  console.log('entities now:', after.map((e) => `${e.id} ${e.name ?? e.device} (chip ${e.chip})`).join(', ') || 'none');
  client.close();
  if (!gone) process.exitCode = 1;
}

main().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
