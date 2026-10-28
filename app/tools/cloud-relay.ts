// Keystone Cloud Relay — HTTP API for app clients, WebSocket tunnel for hubs.
//
// Run:  npm run cloud-relay -- --port 9090 --secret <shared-secret>
//
// Hub (brain) connects to:       ws(s)://host:port/tunnel/<hub-id>
//     Same multiplexing protocol as relay-server.ts.
//
// App uses the HTTP API:
//   POST /hub/:id/auth           {token}               → {session, messages: [...]}
//   GET  /hub/:id/state          (Bearer session)       → {messages: [...]}
//   POST /hub/:id/cmd            {entity, attr, value}  → {status}
//   POST /hub/:id/req            {op, args?}            → {ok, result?} | {ok:false, code, message}
//   GET  /hub/:id/events         (Bearer or ?session=)  → SSE stream
//
// The relay authenticates each app session by forwarding a hub-protocol `hello`
// through the tunnel and waiting for welcome + snapshot.  Commands and management
// requests are forwarded and the response is returned synchronously.  Live updates
// (state, chip, snapshot, history) are pushed over an SSE stream.
import http from 'node:http';
import crypto from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] ?? '' : undefined;
}

const port = Number(arg('port') ?? process.env.PORT ?? '9090');
const secret = arg('secret') ?? process.env.RELAY_SECRET ?? '';
const sessionTtlMs = Number(arg('session-ttl') ?? String(10 * 60_000));
const authTimeoutMs = 15_000;
const reqTimeoutMs = 15_000;

if (!secret) {
  console.error(
    'cloud-relay: no shared secret. Set the RELAY_SECRET variable (a PaaS passes it ' +
      'in the environment) or run it with --secret <shared-secret>.',
  );
  process.exit(2);
}

// ---- types + state ---------------------------------------------------------------

interface Hub {
  id: string;
  tunnel: WebSocket;
  nextCid: number;
}

interface Session {
  id: string;
  hubId: string;
  cid: number;
  snapshot: string[];
  pending: Map<number, { resolve: (v: unknown) => void; reject: (e: unknown) => void; timer: ReturnType<typeof setTimeout> }>;
  sse: Set<http.ServerResponse>;
  lastActivity: number;
  nextId: number;
  authResolve?: (msgs: string[]) => void;
  authReject?: (err: Error) => void;
  authMessages?: string[];
  authTimer?: ReturnType<typeof setTimeout>;
}

const hubs = new Map<string, Hub>();
const sessions = new Map<string, Session>();
const cidMap = new Map<string, Session>();
const readyCallbacks = new Map<string, () => void>();

function log(tag: string, msg: string): void {
  const t = new Date().toLocaleTimeString();
  console.log(`${t}  [${tag}] ${msg}`);
}

function wsSend(ws: WebSocket, msg: string): void {
  if (ws.readyState === WebSocket.OPEN) ws.send(msg);
}

function cidKey(hubId: string, cid: number): string {
  return `${hubId}:${cid}`;
}

// ---- session lifecycle -----------------------------------------------------------

function closeSession(s: Session): void {
  for (const res of s.sse) res.end();
  s.sse.clear();
  for (const [, p] of s.pending) { clearTimeout(p.timer); p.reject(new Error('session closed')); }
  s.pending.clear();
  if (s.authReject) { s.authReject(new Error('session closed')); s.authResolve = undefined; s.authReject = undefined; }
  if (s.authTimer) clearTimeout(s.authTimer);
  const hub = hubs.get(s.hubId);
  if (hub) wsSend(hub.tunnel, `!close:${s.cid}`);
  cidMap.delete(cidKey(s.hubId, s.cid));
  sessions.delete(s.id);
  log('session', `${s.hubId}#${s.cid} closed`);
}

setInterval(() => {
  const now = Date.now();
  for (const [, s] of sessions) {
    if (now - s.lastActivity > sessionTtlMs && s.sse.size === 0 && s.pending.size === 0 && !s.authResolve) {
      log('session', `${s.hubId}#${s.cid} expired`);
      closeSession(s);
    }
  }
}, 30_000);

// ---- routing hub messages to sessions -------------------------------------------

function onHubMessage(s: Session, jsonStr: string): void {
  s.lastActivity = Date.now();
  let msg: Record<string, unknown>;
  try { msg = JSON.parse(jsonStr); } catch { return; }

  if (s.authResolve) {
    s.authMessages!.push(jsonStr);
    if (msg.type === 'error' && msg.id === undefined) {
      clearTimeout(s.authTimer);
      const reject = s.authReject!;
      s.authResolve = undefined; s.authReject = undefined; s.authMessages = undefined;
      reject(new Error(String(msg.code ?? 'error')));
      return;
    }
    if (msg.type === 'snapshot' && msg.last !== false) {
      clearTimeout(s.authTimer);
      const resolve = s.authResolve;
      const messages = s.authMessages!;
      s.authResolve = undefined; s.authReject = undefined; s.authMessages = undefined;
      s.snapshot = messages.filter(m => { try { return JSON.parse(m).type === 'snapshot'; } catch { return false; } });
      resolve(messages);
      return;
    }
    return;
  }

  if (msg.id !== undefined) {
    const p = s.pending.get(msg.id as number);
    if (p && (msg.type === 'ack' || msg.type === 'res' || msg.type === 'error')) {
      clearTimeout(p.timer); s.pending.delete(msg.id as number);
      msg.type === 'error' ? p.reject(msg) : p.resolve(msg);
      return;
    }
    if (msg.type === 'accepted') return;
  }

  if (msg.type === 'snapshot') {
    if ((msg.part ?? 0) === 0) s.snapshot = [];
    s.snapshot.push(jsonStr);
  }

  for (const res of s.sse) res.write(`data: ${jsonStr}\n\n`);
}

// ---- tunnel (hub WebSocket) -----------------------------------------------------

function handleTunnel(ws: WebSocket, hubId: string): void {
  let authed = false;
  const ping = setInterval(() => { if (ws.readyState === WebSocket.OPEN) ws.ping(); }, 30_000);

  ws.on('message', (raw) => {
    const msg = raw.toString();

    if (!authed) {
      if (msg.startsWith('!auth:') && msg.slice(6) === secret) {
        authed = true;
        const old = hubs.get(hubId);
        if (old) {
          for (const [, s] of sessions) if (s.hubId === hubId) closeSession(s);
          old.tunnel.close(1001, 'replaced');
        }
        hubs.set(hubId, { id: hubId, tunnel: ws, nextCid: 1 });
        wsSend(ws, '!ok');
        log('tunnel', `${hubId} registered`);
      } else {
        wsSend(ws, '!error:unauthorized');
        ws.close(1008, 'unauthorized');
      }
      return;
    }

    const hub = hubs.get(hubId);
    if (!hub || hub.tunnel !== ws) return;

    if (msg.startsWith('!ready:')) {
      const cid = parseInt(msg.slice(7), 10);
      const key = cidKey(hubId, cid);
      const cb = readyCallbacks.get(key);
      if (cb) { readyCallbacks.delete(key); cb(); }
      return;
    }

    if (msg.startsWith('!close:')) {
      const cid = parseInt(msg.slice(7), 10);
      const s = cidMap.get(cidKey(hubId, cid));
      if (s) closeSession(s);
      return;
    }

    const colon = msg.indexOf(':');
    if (colon > 0) {
      const cid = parseInt(msg.slice(0, colon), 10);
      const s = cidMap.get(cidKey(hubId, cid));
      if (s) onHubMessage(s, msg.slice(colon + 1));
    }
  });

  ws.on('close', () => {
    clearInterval(ping);
    const hub = hubs.get(hubId);
    if (hub && hub.tunnel === ws) {
      log('tunnel', `${hubId} disconnected`);
      for (const [, s] of sessions) if (s.hubId === hubId) closeSession(s);
      hubs.delete(hubId);
    }
  });

  ws.on('error', () => {});
}

// ---- HTTP helpers ---------------------------------------------------------------

function cors(res: http.ServerResponse): void {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
}

function json(res: http.ServerResponse, status: number, body: unknown): void {
  cors(res);
  res.writeHead(status, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(body));
}

function readBody(req: http.IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    let d = '';
    req.on('data', (c: Buffer) => { d += c; if (d.length > 16384) { reject(new Error('too large')); req.destroy(); } });
    req.on('end', () => resolve(d));
    req.on('error', reject);
  });
}

function getSession(req: http.IncomingMessage, hubId: string): Session | null {
  const auth = req.headers.authorization;
  if (auth?.startsWith('Bearer ')) {
    const s = sessions.get(auth.slice(7));
    return s && s.hubId === hubId ? s : null;
  }
  const url = new URL(req.url ?? '/', `http://${req.headers.host}`);
  const tok = url.searchParams.get('session');
  if (tok) { const s = sessions.get(tok); return s && s.hubId === hubId ? s : null; }
  return null;
}

// ---- HTTP routes ----------------------------------------------------------------

async function routeAuth(hubId: string, req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
  const hub = hubs.get(hubId);
  if (!hub) { json(res, 404, { error: 'hub_offline' }); return; }

  let body: Record<string, unknown>;
  try { body = JSON.parse(await readBody(req)); } catch { json(res, 400, { error: 'bad_json' }); return; }
  const token = body.token;
  if (typeof token !== 'string' || token.length < 8) { json(res, 400, { error: 'bad_token' }); return; }

  const cid = hub.nextCid++;
  const sessionId = crypto.randomBytes(24).toString('base64url');

  const session: Session = {
    id: sessionId, hubId, cid,
    snapshot: [], pending: new Map(), sse: new Set(),
    lastActivity: Date.now(), nextId: 1,
  };
  sessions.set(sessionId, session);
  cidMap.set(cidKey(hubId, cid), session);

  try {
    wsSend(hub.tunnel, `!open:${cid}`);

    await new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => { readyCallbacks.delete(cidKey(hubId, cid)); reject(new Error('timeout')); }, authTimeoutMs);
      readyCallbacks.set(cidKey(hubId, cid), () => { clearTimeout(timer); resolve(); });
    });

    const hello = JSON.stringify({ type: 'hello', v: 1, token, client: 'cloud-api' });
    wsSend(hub.tunnel, `${cid}:${hello}`);

    const messages = await new Promise<string[]>((resolve, reject) => {
      session.authResolve = resolve;
      session.authReject = reject;
      session.authMessages = [];
      session.authTimer = setTimeout(() => {
        session.authResolve = undefined; session.authReject = undefined;
        reject(new Error('timeout'));
      }, authTimeoutMs);
    });

    json(res, 200, { session: sessionId, messages: messages.map(m => JSON.parse(m)) });
    log('session', `${hubId}#${cid} authenticated`);
  } catch (err: unknown) {
    closeSession(session);
    const msg = err instanceof Error ? err.message : '';
    if (msg === 'unauthorized') json(res, 401, { error: 'unauthorized' });
    else if (msg === 'version') json(res, 400, { error: 'version' });
    else json(res, 504, { error: 'timeout' });
  }
}

function routeState(s: Session, res: http.ServerResponse): void {
  s.lastActivity = Date.now();
  json(res, 200, { messages: s.snapshot.map(m => JSON.parse(m)) });
}

async function routeCmd(s: Session, req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
  s.lastActivity = Date.now();
  const hub = hubs.get(s.hubId);
  if (!hub) { json(res, 502, { error: 'hub_offline' }); return; }

  let body: Record<string, unknown>;
  try { body = JSON.parse(await readBody(req)); } catch { json(res, 400, { error: 'bad_json' }); return; }
  if (typeof body.entity !== 'number' || typeof body.attr !== 'number') { json(res, 400, { error: 'bad_args' }); return; }

  const id = s.nextId++;
  wsSend(hub.tunnel, `${s.cid}:${JSON.stringify({ type: 'cmd', id, entity: body.entity, attr: body.attr, value: body.value })}`);

  try {
    const result = await new Promise<Record<string, unknown>>((resolve, reject) => {
      const timer = setTimeout(() => { s.pending.delete(id); reject(new Error('timeout')); }, reqTimeoutMs);
      s.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
    });
    json(res, 200, { status: (result as Record<string, unknown>).status ?? 'ok' });
  } catch (err: unknown) {
    const e = err as Record<string, unknown>;
    if (e.code) json(res, 502, { error: e.code, message: e.message });
    else json(res, 504, { error: 'timeout' });
  }
}

async function routeReq(s: Session, req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
  s.lastActivity = Date.now();
  const hub = hubs.get(s.hubId);
  if (!hub) { json(res, 502, { error: 'hub_offline' }); return; }

  let body: Record<string, unknown>;
  try { body = JSON.parse(await readBody(req)); } catch { json(res, 400, { error: 'bad_json' }); return; }
  if (typeof body.op !== 'string') { json(res, 400, { error: 'bad_args' }); return; }

  const id = s.nextId++;
  wsSend(hub.tunnel, `${s.cid}:${JSON.stringify({ type: 'req', id, op: body.op, ...(body.args ? { args: body.args } : {}) })}`);

  try {
    const result = await new Promise<Record<string, unknown>>((resolve, reject) => {
      const timer = setTimeout(() => { s.pending.delete(id); reject(new Error('timeout')); }, reqTimeoutMs);
      s.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
    });
    if (result.type === 'res') {
      if (result.ok) json(res, 200, { ok: true, result: result.result ?? {} });
      else json(res, 200, { ok: false, code: result.code, message: result.message });
    } else {
      json(res, 200, { ok: true });
    }
  } catch (err: unknown) {
    const e = err as Record<string, unknown>;
    if (e.code) json(res, 502, { error: e.code, message: e.message });
    else json(res, 504, { error: 'timeout' });
  }
}

function routeEvents(s: Session, req: http.IncomingMessage, res: http.ServerResponse): void {
  s.lastActivity = Date.now();
  cors(res);
  res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' });
  res.write(':ok\n\n');
  for (const msg of s.snapshot) res.write(`data: ${msg}\n\n`);
  s.sse.add(res);
  log('sse', `${s.hubId}#${s.cid} stream opened (${s.sse.size})`);
  req.on('close', () => { s.sse.delete(res); log('sse', `${s.hubId}#${s.cid} stream closed (${s.sse.size})`); });
}

// ---- HTTP server ----------------------------------------------------------------

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url ?? '/', `http://${req.headers.host}`);
  const parts = url.pathname.split('/').filter(Boolean);

  if (req.method === 'OPTIONS') { cors(res); res.writeHead(204); res.end(); return; }

  if (parts.length === 0) {
    cors(res); res.writeHead(200, { 'Content-Type': 'text/plain' });
    const lines = [`Keystone Cloud Relay`, `${hubs.size} hub(s), ${sessions.size} session(s)`];
    for (const [id] of hubs) {
      const n = [...sessions.values()].filter(s => s.hubId === id).length;
      lines.push(`  ${id}: ${n} session(s)`);
    }
    res.end(lines.join('\n') + '\n');
    return;
  }

  if (parts.length >= 3 && parts[0] === 'hub') {
    const hubId = parts[1]!;
    const action = parts[2]!;
    try {
      if (action === 'auth' && req.method === 'POST') { await routeAuth(hubId, req, res); return; }
      const session = getSession(req, hubId);
      if (!session) { json(res, 401, { error: 'no_session' }); return; }
      if (action === 'state' && req.method === 'GET') routeState(session, res);
      else if (action === 'cmd' && req.method === 'POST') await routeCmd(session, req, res);
      else if (action === 'req' && req.method === 'POST') await routeReq(session, req, res);
      else if (action === 'events' && req.method === 'GET') routeEvents(session, req, res);
      else json(res, 404, { error: 'not_found' });
    } catch (err) {
      console.error(err);
      if (!res.headersSent) json(res, 500, { error: 'internal' });
    }
    return;
  }

  json(res, 404, { error: 'not_found' });
});

const wss = new WebSocketServer({ noServer: true });
server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url ?? '/', `http://${req.headers.host}`);
  const parts = url.pathname.split('/').filter(Boolean);
  if (parts.length === 2 && parts[0] === 'tunnel') {
    wss.handleUpgrade(req, socket, head, (ws) => handleTunnel(ws, parts[1]!));
  } else {
    socket.write('HTTP/1.1 404 Not Found\r\n\r\n');
    socket.destroy();
  }
});

server.listen(port, '0.0.0.0', () => log('relay', `cloud relay listening on port ${port}`));

process.on('SIGINT', () => {
  log('relay', 'shutting down');
  for (const [, s] of sessions) closeSession(s);
  for (const [, hub] of hubs) hub.tunnel.close(1001, 'relay stopping');
  server.close();
  setTimeout(() => process.exit(0), 500);
});
