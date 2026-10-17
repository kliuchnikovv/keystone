// Keystone Relay Server — remote access for the hub.
//
// Run on a VPS (or localhost for testing). Put behind nginx/caddy for TLS in production.
//
//   npm run relay-server -- --port 9090 --secret <shared-secret>
//
// Tunnel agent (on the hub's LAN) connects to:  ws://relay:9090/tunnel/<hub-id>
// Remote clients (console, app) connect to:      ws://relay:9090/client/<hub-id>
//
// The relay is transparent: it forwards WebSocket text frames without inspecting them.
// The hub's own token-based auth protects against unauthorized access; the relay only
// checks the tunnel's shared secret.
//
// Wire protocol between relay and tunnel (text WebSocket frames):
//   !auth:<secret>      tunnel → relay, first message
//   !ok                 relay → tunnel, auth accepted
//   !open:<cid>         relay → tunnel, a client connected — open a local hub connection
//   !ready:<cid>        tunnel → relay, local connection open, start forwarding
//   !close:<cid>        either direction, connection gone
//   !error:<msg>        relay → tunnel
//   <cid>:<payload>     either direction, a hub-protocol text frame for client <cid>
import http from 'node:http';
import { WebSocketServer, WebSocket } from 'ws';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] ?? '' : undefined;
}

const port = Number(arg('port') ?? '9090');
const secret = arg('secret') ?? '';
const maxClientsPerHub = Number(arg('max-clients') ?? '3');

if (!secret) {
  console.error('Usage: relay-server --port <port> --secret <shared-secret> [--max-clients 3]');
  process.exit(2);
}

// ---- state --------------------------------------------------------------------------

interface Hub {
  id: string;
  tunnel: WebSocket;
  clients: Map<number, WebSocket>;
  nextCid: number;
  buffered: Map<number, string[]>;
}

const hubs = new Map<string, Hub>();

function send(ws: WebSocket, msg: string): void {
  if (ws.readyState === WebSocket.OPEN) ws.send(msg);
}

function log(tag: string, msg: string): void {
  const t = new Date().toLocaleTimeString();
  console.log(`${t}  [${tag}] ${msg}`);
}

// ---- HTTP + WebSocket ---------------------------------------------------------------

const server = http.createServer((_, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  const lines = [`Keystone Relay`, `${hubs.size} hub(s) connected`];
  for (const [id, hub] of hubs) lines.push(`  ${id}: ${hub.clients.size} client(s)`);
  res.end(lines.join('\n') + '\n');
});

const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url ?? '/', `http://${req.headers.host}`);
  const parts = url.pathname.split('/').filter(Boolean);

  if (parts.length === 2 && parts[0] === 'tunnel') {
    wss.handleUpgrade(req, socket, head, (ws) => handleTunnel(ws, parts[1]!));
  } else if (parts.length === 2 && parts[0] === 'client') {
    wss.handleUpgrade(req, socket, head, (ws) => handleClient(ws, parts[1]!));
  } else {
    socket.write('HTTP/1.1 404 Not Found\r\n\r\n');
    socket.destroy();
  }
});

// ---- tunnel -------------------------------------------------------------------------

function handleTunnel(ws: WebSocket, hubId: string): void {
  let authed = false;

  const pingInterval = setInterval(() => {
    if (ws.readyState === WebSocket.OPEN) ws.ping();
  }, 30_000);

  ws.on('message', (raw) => {
    const msg = raw.toString();

    if (!authed) {
      if (msg.startsWith('!auth:') && msg.slice(6) === secret) {
        authed = true;
        const old = hubs.get(hubId);
        if (old) {
          for (const [, cws] of old.clients) cws.close(1001, 'tunnel replaced');
          old.tunnel.close(1001, 'replaced');
        }
        const hub: Hub = { id: hubId, tunnel: ws, clients: new Map(), nextCid: 1, buffered: new Map() };
        hubs.set(hubId, hub);
        send(ws, '!ok');
        log('tunnel', `${hubId} registered`);
      } else {
        send(ws, '!error:unauthorized');
        ws.close(1008, 'unauthorized');
      }
      return;
    }

    const hub = hubs.get(hubId);
    if (!hub || hub.tunnel !== ws) return;

    if (msg.startsWith('!ready:')) {
      const cid = parseInt(msg.slice(7), 10);
      const buf = hub.buffered.get(cid);
      if (buf) {
        for (const frame of buf) send(ws, `${cid}:${frame}`);
        hub.buffered.delete(cid);
      }
      return;
    }

    if (msg.startsWith('!close:')) {
      const cid = parseInt(msg.slice(7), 10);
      const cws = hub.clients.get(cid);
      if (cws) {
        cws.close(1000, 'hub disconnected');
        hub.clients.delete(cid);
        hub.buffered.delete(cid);
      }
      return;
    }

    const colon = msg.indexOf(':');
    if (colon > 0) {
      const cid = parseInt(msg.slice(0, colon), 10);
      const data = msg.slice(colon + 1);
      const cws = hub.clients.get(cid);
      if (cws && cws.readyState === WebSocket.OPEN) cws.send(data);
    }
  });

  ws.on('close', () => {
    clearInterval(pingInterval);
    const hub = hubs.get(hubId);
    if (hub && hub.tunnel === ws) {
      log('tunnel', `${hubId} disconnected (${hub.clients.size} client(s) dropped)`);
      for (const [, cws] of hub.clients) cws.close(1001, 'tunnel lost');
      hubs.delete(hubId);
    }
  });

  ws.on('error', () => {});
}

// ---- client -------------------------------------------------------------------------

function handleClient(ws: WebSocket, hubId: string): void {
  const hub = hubs.get(hubId);
  if (!hub) {
    ws.close(4404, 'hub not connected');
    return;
  }
  if (hub.clients.size >= maxClientsPerHub) {
    ws.close(4429, 'too many clients');
    return;
  }

  const cid = hub.nextCid++;
  hub.clients.set(cid, ws);
  hub.buffered.set(cid, []);

  send(hub.tunnel, `!open:${cid}`);
  log('client', `${hubId}#${cid} connected (${hub.clients.size} total)`);

  ws.on('message', (raw) => {
    const data = raw.toString();
    const buf = hub.buffered.get(cid);
    if (buf) {
      buf.push(data);
    } else {
      send(hub.tunnel, `${cid}:${data}`);
    }
  });

  ws.on('close', () => {
    log('client', `${hubId}#${cid} disconnected`);
    hub.clients.delete(cid);
    hub.buffered.delete(cid);
    send(hub.tunnel, `!close:${cid}`);
  });

  ws.on('error', () => {});
}

// ---- start --------------------------------------------------------------------------

server.listen(port, () => {
  log('relay', `listening on port ${port}`);
});

process.on('SIGINT', () => {
  log('relay', 'shutting down');
  for (const [, hub] of hubs) {
    for (const [, cws] of hub.clients) cws.close(1001, 'relay stopping');
    hub.tunnel.close(1001, 'relay stopping');
  }
  server.close();
  setTimeout(() => process.exit(0), 500);
});
