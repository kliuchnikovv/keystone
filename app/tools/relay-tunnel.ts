// Keystone Relay Tunnel — connects the hub to the relay for remote access.
//
// Run on the same LAN as the hub (e.g. on the Pi the ESPs are plugged into).
//
//   npm run relay-tunnel -- --relay ws://relay.example.com:9090/tunnel/my-hub \
//     --secret <shared-secret> --hub ws://192.168.0.202:8080/ws
//
// For each remote client that connects through the relay, the tunnel opens a
// local WebSocket to the hub and pipes frames transparently. The hub's own
// token auth works end-to-end: the tunnel never sees the token.
import WebSocket from 'ws';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] ?? '' : undefined;
}

const relayUrl = arg('relay');
const relaySecret = arg('secret') ?? '';
const hubUrl = arg('hub') ?? 'ws://127.0.0.1:8080/ws';

if (!relayUrl || !relaySecret) {
  console.error(
    'Usage: relay-tunnel --relay ws://relay:9090/tunnel/<hub-id>' +
      ' --secret <shared-secret> [--hub ws://hub:8080/ws]',
  );
  process.exit(2);
}

function log(msg: string): void {
  const t = new Date().toLocaleTimeString();
  console.log(`${t}  ${msg}`);
}

// ---- local hub connections (one per remote client) ----------------------------------

const locals = new Map<number, WebSocket>();

function send(ws: WebSocket, msg: string): void {
  if (ws.readyState === WebSocket.OPEN) ws.send(msg);
}

function openLocal(cid: number, relay: WebSocket): void {
  const secure = /^wss:/i.test(hubUrl);
  const ws = new WebSocket(hubUrl, secure ? { rejectUnauthorized: false } : {});

  ws.on('open', () => {
    locals.set(cid, ws);
    send(relay, `!ready:${cid}`);
    log(`[hub] cid ${cid} connected locally`);
  });

  ws.on('message', (raw) => {
    send(relay, `${cid}:${raw.toString()}`);
  });

  ws.on('close', () => {
    if (locals.get(cid) === ws) {
      locals.delete(cid);
      send(relay, `!close:${cid}`);
      log(`[hub] cid ${cid} disconnected`);
    }
  });

  ws.on('error', (err) => {
    log(`[hub] cid ${cid} error: ${(err as Error).message}`);
  });
}

function closeLocal(cid: number): void {
  const ws = locals.get(cid);
  if (ws) {
    locals.delete(cid);
    ws.close(1000);
  }
}

function closeAllLocals(): void {
  for (const [cid, ws] of locals) {
    ws.close(1001);
    locals.delete(cid);
  }
}

// ---- relay connection with reconnect ------------------------------------------------

let relay: WebSocket | null = null;
let attempt = 0;
let stopping = false;

function connect(): void {
  if (stopping) return;
  log(`[relay] connecting to ${relayUrl}`);

  const ws = new WebSocket(relayUrl!);

  ws.on('open', () => {
    relay = ws;
    attempt = 0;
    log('[relay] connected, authenticating…');
    send(ws, `!auth:${relaySecret}`);
  });

  ws.on('message', (raw) => {
    const msg = raw.toString();

    if (msg === '!ok') {
      log('[relay] authenticated — waiting for clients');
      return;
    }

    if (msg.startsWith('!error:')) {
      log(`[relay] error: ${msg.slice(7)}`);
      return;
    }

    if (msg.startsWith('!open:')) {
      const cid = parseInt(msg.slice(6), 10);
      log(`[relay] remote client ${cid} connected`);
      openLocal(cid, ws);
      return;
    }

    if (msg.startsWith('!close:')) {
      const cid = parseInt(msg.slice(7), 10);
      closeLocal(cid);
      return;
    }

    // Data: <cid>:<payload>
    const colon = msg.indexOf(':');
    if (colon > 0) {
      const cid = parseInt(msg.slice(0, colon), 10);
      const data = msg.slice(colon + 1);
      const lws = locals.get(cid);
      if (lws && lws.readyState === WebSocket.OPEN) lws.send(data);
    }
  });

  ws.on('close', () => {
    if (relay === ws) relay = null;
    closeAllLocals();
    if (!stopping) reconnect();
  });

  ws.on('error', (err) => {
    log(`[relay] error: ${(err as Error).message}`);
  });

  ws.on('pong', () => {}); // keep alive
}

function reconnect(): void {
  const delay = Math.min(30_000, 1000 * 2 ** attempt) * (0.8 + Math.random() * 0.4);
  attempt++;
  log(`[relay] reconnecting in ${(delay / 1000).toFixed(1)}s`);
  setTimeout(connect, delay);
}

// ---- start --------------------------------------------------------------------------

connect();

process.on('SIGINT', () => {
  log('shutting down');
  stopping = true;
  closeAllLocals();
  relay?.close(1000);
  setTimeout(() => process.exit(0), 500);
});
