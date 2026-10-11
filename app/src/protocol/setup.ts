// First-time setup of a hub that has no network yet (docs/app-protocol.md §11).
//
// The phone has joined the hub's own access point `Keystone-XXXX` (the password is the hub's setup code) and
// talks to it at wss://192.168.4.1:8443/ws. This client signs in with the same setup code, which lets it do
// exactly two things: scan for networks and send `setup.apply` (the home network and the app's long-term token).
//
// Trust on first use, and why it is safe here: the hub's certificate is self-signed and not known yet, so the
// socket is asked to accept it and report its SHA-256. The access point is WPA2 with the setup code as password,
// so only a device that knows the code can be on that link at all; a fingerprint learned over it is the hub's.
// The app stores that fingerprint and pins it on the home network from then on.
import { PROTOCOL_VERSION, parseServerMessage, type ErrorCode, type HubInfo, type WifiNetwork } from './messages';
import { PinUnsupportedError, type SocketFactory, type SocketLike } from './client';
import { normalizeFingerprint } from './fingerprint';

export type SetupFailure =
  | 'unreachable' // the hub did not answer: wrong Wi-Fi, out of range
  | 'bad_code' // the hub does not know that setup code
  | 'not_setup' // the code was not a setup code: this hub is already set up
  | 'unsupported' // this build cannot learn a certificate (browser, Expo Go)
  | 'busy'
  | 'incompatible'
  | 'timeout'
  | 'refused' // the hub said no to a request (message has why)
  | 'closed';

export class SetupError extends Error {
  constructor(
    readonly kind: SetupFailure,
    message?: string,
  ) {
    super(message ?? kind);
    this.name = 'SetupError';
  }
}

/** Where the hub's access point puts it. */
export const SETUP_URL = 'wss://192.168.4.1:8443/ws';

export interface SetupClientOptions {
  url?: string;
  /** The setup code as typed (case, dashes and spaces do not matter to the hub). */
  code: string;
  socketFactory: SocketFactory;
  connectTimeoutMs?: number;
  requestTimeoutMs?: number;
}

interface Waiting {
  resolve: (r: Record<string, unknown>) => void;
  reject: (e: SetupError) => void;
  timer: ReturnType<typeof setTimeout>;
}

const REFUSALS: Partial<Record<ErrorCode, string>> = {
  bad_args: 'The hub did not accept that.',
  unsupported: 'The hub does not allow that during setup.',
};

export class SetupClient {
  private sock: SocketLike | null = null;
  private done = false;
  private nextId = 1;
  private waiting = new Map<number, Waiting>();
  private lastError: ErrorCode | null = null;
  private readonly url: string;

  /** The certificate fingerprint the hub presented (wss only), to be pinned afterwards. */
  fingerprint: string | null = null;
  /** The hub's `welcome` object once connected. */
  hub: HubInfo | null = null;

  constructor(private readonly opts: SetupClientOptions) {
    this.url = opts.url ?? SETUP_URL;
  }

  /** Open the socket, sign in with the setup code and wait for the hub's welcome. */
  connect(): Promise<void> {
    const secure = /^wss:/i.test(this.url);
    return new Promise<void>((resolve, reject) => {
      let settled = false;
      const finish = (err?: SetupError) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        if (err) {
          this.shut();
          reject(err);
        } else {
          resolve();
        }
      };
      const timer = setTimeout(() => finish(new SetupError('unreachable', 'no answer')), this.opts.connectTimeoutMs ?? 10_000);

      let sock: SocketLike;
      try {
        sock = this.opts.socketFactory(this.url, secure ? { learn: true } : {});
      } catch (e) {
        finish(e instanceof PinUnsupportedError ? new SetupError('unsupported') : new SetupError('unreachable', (e as Error).message));
        return;
      }
      this.sock = sock;
      sock.onopen = (ev) => {
        if (this.sock !== sock) return;
        const seen = (ev as { fingerprint?: unknown } | null)?.fingerprint;
        this.fingerprint = typeof seen === 'string' ? normalizeFingerprint(seen) : null;
        if (secure && !this.fingerprint) return finish(new SetupError('unreachable', 'the hub presented no certificate'));
        this.send({ type: 'hello', v: PROTOCOL_VERSION, token: this.opts.code, client: 'keystone-setup' });
      };
      sock.onmessage = (ev) => {
        if (this.sock !== sock || typeof ev.data !== 'string') return;
        const m = parseServerMessage(ev.data);
        if (!m) return;
        switch (m.type) {
          case 'welcome':
            if (!m.setup) return finish(new SetupError('not_setup'));
            this.hub = m.hub;
            return finish();
          case 'res': {
            const w = this.waiting.get(m.id);
            if (!w) return;
            this.waiting.delete(m.id);
            clearTimeout(w.timer);
            if (m.ok) w.resolve(m.result ?? {});
            else w.reject(new SetupError('refused', m.message ?? REFUSALS[m.code] ?? m.code));
            return;
          }
          case 'error':
            if (m.id !== undefined) {
              const w = this.waiting.get(m.id);
              if (w) {
                this.waiting.delete(m.id);
                clearTimeout(w.timer);
                w.reject(new SetupError('refused', m.message ?? m.code));
              }
              return;
            }
            this.lastError = m.code;
            if (!settled) {
              if (m.code === 'unauthorized') finish(new SetupError('bad_code'));
              else if (m.code === 'busy') finish(new SetupError('busy'));
              else if (m.code === 'version') finish(new SetupError('incompatible'));
            }
            return;
          default:
            return; // a setup client is sent nothing else
        }
      };
      sock.onerror = () => {
        /* onclose follows */
      };
      sock.onclose = () => {
        if (this.sock !== sock) return;
        this.sock = null;
        if (!settled) {
          finish(new SetupError(this.lastError === 'unauthorized' ? 'bad_code' : 'unreachable'));
        } else {
          this.failAll(new SetupError('closed'));
        }
      };
    });
  }

  /** The networks the hub can see, strongest first. */
  async scan(): Promise<WifiNetwork[]> {
    const r = await this.request('wifi.scan', undefined, 15_000);
    return Array.isArray(r.networks) ? (r.networks as WifiNetwork[]) : [];
  }

  /**
   * Give the hub the home network and the token the app will use from now on. The hub answers first and joins the
   * network about a second later, so the answer arrives and the connection then drops: that is success.
   */
  async apply(ssid: string, password: string, token: string): Promise<void> {
    await this.request('setup.apply', { ssid, password, token });
  }

  close(): void {
    this.shut();
    this.failAll(new SetupError('closed'));
  }

  // ---- plumbing ---------------------------------------------------------------

  private request(op: string, args?: Record<string, unknown>, timeoutMs?: number): Promise<Record<string, unknown>> {
    if (!this.sock || this.done) return Promise.reject(new SetupError('closed', 'not connected'));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting.delete(id);
        reject(new SetupError('timeout'));
      }, timeoutMs ?? this.opts.requestTimeoutMs ?? 15_000);
      this.waiting.set(id, { resolve, reject, timer });
      this.send({ type: 'req', id, op, ...(args ? { args } : {}) });
    });
  }

  private send(m: unknown): void {
    try {
      this.sock?.send(JSON.stringify(m));
    } catch {
      /* onclose deals with a dead socket */
    }
  }

  private failAll(err: SetupError): void {
    for (const [id, w] of this.waiting) {
      clearTimeout(w.timer);
      this.waiting.delete(id);
      w.reject(err);
    }
  }

  private shut(): void {
    this.done = true;
    const s = this.sock;
    this.sock = null;
    try {
      s?.close();
    } catch {
      /* already closed */
    }
  }
}
