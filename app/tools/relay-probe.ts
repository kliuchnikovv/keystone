// Check the path from a relay to a hub: does the client's hello arrive there?
//   npm run relay-probe -- --url wss://relay.example.com/client/home --token <token>
//
// The token is optional on purpose. A wrong token gets "unauthorized" from
// the hub - and that answer already proves the whole path (relay -> tunnel ->
// hub -> back). A hub that never answers means the path is broken: the relay
// lost the frame, or the hub is not connected to the relay at all.
//
// The address is the client side of the tunnel - the relay's status page says
// it (it wants RELAY_STATUS_TOKEN), and the hub console says the tunnel's.
//
// The probe sends an Origin header, the way the phone does: React Native's
// WebSocket on iOS is SocketRocket, and it mirrors the address it dials into
// the header. A relay has to allow its own origin for that to work.
//   --origin <value>   send this origin instead
//   --no-origin        send no origin at all (what the hub's own client does)
import { WebSocket } from 'ws';
import { HubClient, type SocketFactory, type SocketLike } from '../src/protocol/client';

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
const flag = (name: string): boolean => process.argv.includes(`--${name}`);

const url = arg('url');
if (!url) {
  console.error('usage: npm run relay-probe -- --url wss://relay.example.com/client/<hub-id> [--token <token>] [--seconds N]');
  process.exit(2);
}
const token = arg('token') ?? 'probe-token';
const seconds = Number(arg('seconds') ?? '15');

/** The origin SocketRocket would send: the address it dials, "wss" as "https". */
function socketRocketOrigin(u: string): string {
  const parsed = new URL(u);
  return (parsed.protocol === 'wss:' || parsed.protocol === 'https:' ? 'https://' : 'http://') + parsed.host;
}

const origin = flag('no-origin') ? null : (arg('origin') ?? socketRocketOrigin(url));
if (origin) console.log(`origin: ${origin}`);

class ProbeSocket implements SocketLike {
  onopen: SocketLike['onopen'] = null;
  onmessage: SocketLike['onmessage'] = null;
  onclose: SocketLike['onclose'] = null;
  onerror: SocketLike['onerror'] = null;
  private ws: WebSocket;

  constructor(u: string) {
    this.ws = new WebSocket(u, origin ? { origin } : {});
    this.ws.on('open', () => this.onopen?.({}));
    this.ws.on('message', (d) => this.onmessage?.({ data: d.toString() }));
    this.ws.on('close', () => this.onclose?.({}));
    this.ws.on('error', (e) => this.onerror?.(e));
  }

  send(data: string): void {
    this.ws.send(data);
  }

  close(): void {
    this.ws.close();
  }
}

const socketFactory: SocketFactory = (u) => new ProbeSocket(u);

const client = new HubClient({ url, token, clientName: 'relay-probe', socketFactory });

const t0 = Date.now();
const stamp = () => `+${((Date.now() - t0) / 1000).toFixed(1)}s`;
let last = client.getSnapshot().status;

client.subscribe(() => {
  const s = client.getSnapshot();
  if (s.status !== last) {
    console.log(stamp(), s.status, s.error ?? '');
    last = s.status;
  }
});

client.connect();

setTimeout(() => {
  const s = client.getSnapshot();
  client.close();
  switch (s.status) {
    case 'ready':
      console.log(`\n${stamp()} the hub answered: the path works.`, s.hub?.name ?? '');
      process.exit(0);
      break;
    case 'unauthorized':
      console.log(`\n${stamp()} the hub answered and refused the token: the path works, the token does not.`);
      process.exit(1);
      break;
    case 'incompatible':
      console.log(`\n${stamp()} the hub answered with another protocol version: the path works.`);
      process.exit(1);
      break;
    case 'untrusted':
      console.log(`\n${stamp()} the hub answered but its certificate was refused: the path works.`);
      process.exit(1);
      break;
    case 'reconnecting':
      console.log(`\n${stamp()} the relay closed the connection: the path is broken.`);
      process.exit(3);
      break;
    default:
      console.log(`\n${stamp()} no answer from the hub: the path is broken.`);
      process.exit(3);
  }
}, seconds * 1000);
