// HubClient: one connection to the hub (docs/app-protocol.md). Handles the
// handshake, keeps a HomeState up to date, correlates commands/requests with
// their replies and reconnects with back-off. No React, no Node-only APIs: the
// WebSocket constructor is injectable, and by default the platform's global one
// is used (React Native, browsers, Node >= 22).
import {
  type AckStatus,
  type ClientMessage,
  type ErrorCode,
  type ServerMessage,
  type Value,
  PROTOCOL_VERSION,
  parseServerMessage,
} from './messages';
import { type ConnectionStatus, type HomeState, cleared, initialState, reduce, withStatus } from './store';

/** The subset of the standard WebSocket API this client uses. */
export interface SocketLike {
  onopen: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  onclose: ((ev: unknown) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  send(data: string): void;
  close(): void;
}
export type SocketFactory = (url: string) => SocketLike;

export interface HubClientOptions {
  /** `wss://host:8443/ws` (or `ws://host:8080/ws` for development). */
  url: string;
  token: string;
  /** Sent in `hello`, for the hub's log. */
  clientName?: string;
  socketFactory?: SocketFactory;
  /** Reconnect after a drop (default true). */
  reconnect?: boolean;
  backoffMs?: { min: number; max: number };
  /** How long to wait for a reply to a cmd/req (default 15 s). */
  requestTimeoutMs?: number;
}

/** A failure reported by the hub, or by the connection itself. */
export class HubError extends Error {
  constructor(
    readonly code: ErrorCode | 'disconnected' | 'timeout' | 'closed',
    message?: string,
  ) {
    super(message ?? code);
    this.name = 'HubError';
  }
}

interface Pending {
  kind: 'cmd' | 'req';
  resolve: (v: never) => void;
  reject: (e: HubError) => void;
  timer: ReturnType<typeof setTimeout>;
}

type Listener = () => void;

export class HubClient {
  private state: HomeState = initialState;
  private listeners = new Set<Listener>();
  private socket: SocketLike | null = null;
  private pending = new Map<number, Pending>();
  private nextId = 1;
  private attempt = 0;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
  private stopped = false;
  private readonly opts: Required<Omit<HubClientOptions, 'socketFactory'>> & { socketFactory: SocketFactory };

  constructor(options: HubClientOptions) {
    // `??` rather than a spread: an explicit `undefined` must not erase a default.
    this.opts = {
      url: options.url,
      token: options.token,
      clientName: options.clientName ?? 'keystone-app',
      reconnect: options.reconnect ?? true,
      backoffMs: options.backoffMs ?? { min: 500, max: 10_000 },
      requestTimeoutMs: options.requestTimeoutMs ?? 15_000,
      socketFactory: options.socketFactory ?? defaultSocketFactory,
    };
  }

  // ---- state (works with React's useSyncExternalStore) ------------------------

  getSnapshot = (): HomeState => this.state;

  subscribe = (fn: Listener): (() => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  private set(next: HomeState): void {
    if (next === this.state) return;
    this.state = next;
    for (const fn of [...this.listeners]) fn();
  }

  // ---- lifecycle --------------------------------------------------------------

  connect(): void {
    if (this.socket || this.stopped) return;
    this.clearRetry();
    this.set(withStatus(this.state, this.attempt === 0 ? 'connecting' : 'reconnecting', this.state.error));
    let sock: SocketLike;
    try {
      sock = this.opts.socketFactory(this.opts.url);
    } catch {
      this.scheduleRetry();
      return;
    }
    this.socket = sock;
    sock.onopen = () => {
      if (this.socket !== sock) return;
      this.set(withStatus(this.state, 'authenticating', this.state.error));
      this.send({ type: 'hello', v: PROTOCOL_VERSION, token: this.opts.token, client: this.opts.clientName });
    };
    sock.onmessage = (ev) => {
      if (this.socket !== sock || typeof ev.data !== 'string') return;
      const msg = parseServerMessage(ev.data);
      if (msg) this.handle(msg);
    };
    sock.onerror = () => {
      /* onclose follows; nothing to add */
    };
    sock.onclose = () => {
      if (this.socket !== sock) return;
      this.socket = null;
      this.failPending(new HubError('disconnected'));
      const s = this.state.status;
      if (this.stopped || s === 'unauthorized' || s === 'incompatible') return;
      this.set(withStatus(cleared(this.state), 'reconnecting', this.state.error));
      this.scheduleRetry();
    };
  }

  /** Stop for good: no further reconnects. Pending requests fail with `closed`. */
  close(): void {
    this.stopped = true;
    this.clearRetry();
    const sock = this.socket;
    this.socket = null;
    this.failPending(new HubError('closed'));
    this.set(withStatus(cleared(this.state), 'closed'));
    try {
      sock?.close();
    } catch {
      /* already closed */
    }
  }

  // ---- requests ---------------------------------------------------------------

  /** Set an attribute. Resolves with the chip's CMD_ACK status (or rejects on a protocol error). */
  cmd(entity: number, attr: number, value: Value): Promise<AckStatus> {
    return this.request<AckStatus>('cmd', (id) => ({ type: 'cmd', id, entity, attr, value }));
  }

  /** Management request (`rule.add`, `name.set`, …). Resolves with `result` (or `{}`). */
  req(op: string, args?: Record<string, unknown>): Promise<Record<string, unknown>> {
    return this.request<Record<string, unknown>>('req', (id) => ({ type: 'req', id, op, ...(args ? { args } : {}) }));
  }

  private request<T>(kind: Pending['kind'], build: (id: number) => ClientMessage): Promise<T> {
    if (this.state.status !== 'ready' || !this.socket) {
      return Promise.reject(new HubError('disconnected', 'not connected'));
    }
    const id = this.nextId;
    this.nextId = this.nextId >= 0x7fffffff ? 1 : this.nextId + 1;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new HubError('timeout'));
      }, this.opts.requestTimeoutMs);
      this.pending.set(id, { kind, resolve: resolve as (v: never) => void, reject, timer });
      this.send(build(id));
    });
  }

  private settle(id: number, fn: (p: Pending) => void): void {
    const p = this.pending.get(id);
    if (!p) return;
    clearTimeout(p.timer);
    this.pending.delete(id);
    fn(p);
  }

  private failPending(err: HubError): void {
    for (const [id, p] of this.pending) {
      clearTimeout(p.timer);
      this.pending.delete(id);
      p.reject(err);
    }
  }

  // ---- incoming ---------------------------------------------------------------

  private handle(m: ServerMessage): void {
    switch (m.type) {
      case 'accepted':
        return; // the command is on its way; its `ack` settles the promise
      case 'ack':
        this.settle(m.id, (p) => p.resolve(m.status as never));
        return;
      case 'res':
        this.settle(m.id, (p) =>
          m.ok ? p.resolve((m.result ?? {}) as never) : p.reject(new HubError(m.code, m.message)),
        );
        return;
      case 'error':
        if (m.id !== undefined) {
          this.settle(m.id, (p) => p.reject(new HubError(m.code, m.message)));
          return;
        }
        this.connectionError(m.code);
        return;
      case 'snapshot':
        this.set(reduce(this.state, m));
        if (this.state.status === 'ready') this.attempt = 0; // handshake fully done
        return;
      default:
        this.set(reduce(this.state, m));
    }
  }

  /** A hub-level error with no request id: handshake refusals and the like. */
  private connectionError(code: ErrorCode): void {
    let status: ConnectionStatus | null = null;
    if (code === 'unauthorized') status = 'unauthorized';
    else if (code === 'version') status = 'incompatible';
    if (status) {
      this.stopped = true; // retrying with the same token / version cannot help
      this.set(withStatus(this.state, status, code));
    } else {
      this.set(withStatus(this.state, this.state.status, code)); // e.g. busy: onclose retries
    }
  }

  // ---- plumbing ---------------------------------------------------------------

  private send(m: ClientMessage): void {
    try {
      this.socket?.send(JSON.stringify(m));
    } catch {
      /* the close handler deals with a dead socket */
    }
  }

  private scheduleRetry(): void {
    if (this.stopped || !this.opts.reconnect) return;
    const { min, max } = this.opts.backoffMs;
    const base = Math.min(max, min * 2 ** this.attempt);
    const delay = Math.round(base * (0.8 + Math.random() * 0.4));
    this.attempt += 1;
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      this.connect();
    }, delay);
  }

  private clearRetry(): void {
    if (this.retryTimer) clearTimeout(this.retryTimer);
    this.retryTimer = null;
  }
}

function defaultSocketFactory(url: string): SocketLike {
  const WS = (globalThis as { WebSocket?: new (url: string) => SocketLike }).WebSocket;
  if (!WS) throw new Error('no global WebSocket; pass socketFactory');
  return new WS(url);
}
