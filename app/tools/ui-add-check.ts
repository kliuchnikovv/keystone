// The app's own path to the hub, without a phone: the same HubClient the app uses,
// over plain ws:// (the hub's development mode). It checks what the Add device button
// does — `matter.commission` with a QR payload — and that the hub answers "started".
//
// Usage:
//   npx tsx tools/ui-add-check.ts --url ws://<hub>:8080/ws --token <token> --payload <hex>
import { HubClient } from '../src/protocol/client';

function arg(name: string, fallback = ''): string {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && process.argv[i + 1] !== undefined ? process.argv[i + 1]! : fallback;
}

const url = arg('url', 'ws://127.0.0.1:8080/ws');
const token = arg('token', 'dev-token');
const payload = arg('payload', '');

const client = new HubClient({ url, token, clientName: 'ui-add-check', reconnect: false });

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
  console.log(`connected to ${url}`);

  if (!payload) {
    console.log('no payload given — pass --payload <hex of a Matter QR payload>');
    client.close();
    return;
  }
  try {
    const res = await client.req('matter.commission', { kind: 2, payload });
    console.log('matter.commission answered:', JSON.stringify(res));
  } catch (err) {
    console.log('matter.commission failed:', (err as Error).message);
    client.close();
    process.exitCode = 1;
    return;
  }

  // The hub pushes the attempt's progress; wait a moment for it.
  await sleep(500);
  const snap = client.getSnapshot();
  console.log('matter attempt:', JSON.stringify(snap.matter));
  client.close();
}

main().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
