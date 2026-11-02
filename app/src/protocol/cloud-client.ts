// CloudClient: connects to the hub through a cloud relay via HTTP + SSE.
// Same public interface as HubClient so hooks and components work with either.
import type { AckStatus, HistoryPush, ServerMessage, Value } from './messages';
import { type ConnectionStatus, type HomeState, initialState, markedStale, reduce, withStatus } from './store';

export class HubError extends Error {
  constructor(
    readonly code: string,
    message?: string,
  ) {
    super(message ?? code);
    this.name = 'HubError';
  }
}

export interface CloudClientOptions {
  /** Cloud API base, e.g. `https://relay.example.com/hub/abc123`. */
  url: string;
  token: string;
  reconnect?: boolean;
  backoffMs?: { min: number; max: number };
}

type Listener = () => void;

function asMessage(v: unknown): ServerMessage | null {
  if (typeof v !== 'object' || v === null || Array.isArray(v)) return null;
  if (typeof (v as Record<string, unknown>).type !== 'string') return null;
  return v as ServerMessage;
}

export class CloudClient {
  private state: HomeState = initialState;
  private listeners = new Set<Listener>();
  private historyListeners = new Set<(push: HistoryPush) => void>();
  private session: string | null = null;
  private es: EventSource | null = null;
  private stopped = false;
  private attempt = 0;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
  private authAbort: AbortController | null = null;
  /** The hub has sent a snapshot: its word is the last one, the cache is history. */
  private live = false;
  /** The connection lives on in someone else's hands: it must not be closed with us. */
  private handedOver = false;
  private readonly opts: Required<CloudClientOptions>;

  constructor(options: CloudClientOptions) {
    this.opts = {
      url: options.url,
      token: options.token,
      reconnect: options.reconnect ?? true,
      backoffMs: options.backoffMs ?? { min: 500, max: 10_000 },
    };
  }

  getSnapshot = (): HomeState => this.state;

  subscribe = (fn: Listener): (() => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  subscribeHistory = (fn: (push: HistoryPush) => void): (() => void) => {
    this.historyListeners.add(fn);
    return () => this.historyListeners.delete(fn);
  };

  /** Show what the app already knows until the hub says otherwise. */
  hydrate(state: HomeState): void {
    if (this.live) return;
    this.set({ ...state, status: this.state.status, error: this.state.error });
  }

  /** The connection is not ours to close any more: it was handed to someone else. */
  handOver(): void {
    this.handedOver = true;
  }

  /** True after `handOver()`. */
  get isHandedOver(): boolean {
    return this.handedOver;
  }

  connect(): void {
    this.stopped = false;
    if (this.session) return;
    this.clearRetry();
    this.set(withStatus(this.state, this.attempt === 0 ? 'connecting' : 'reconnecting', this.state.error));
    this.doAuth();
  }

  close(): void {
    this.stopped = true;
    this.clearRetry();
    this.authAbort?.abort();
    this.authAbort = null;
    this.es?.close();
    this.es = null;
    this.session = null;
    // What we know stays on screen: it is the last known state, not the live one.
    this.set(withStatus(markedStale(this.state, Date.now()), 'closed'));
  }

  async cmd(entity: number, attr: number, value: Value): Promise<AckStatus> {
    if (!this.session || this.state.status !== 'ready') throw new HubError('disconnected', 'not connected');
    const res = await fetch(`${this.opts.url}/cmd`, { method: 'POST', headers: this.headers(), body: JSON.stringify({ entity, attr, value }) });
    const data = await res.json();
    if (!res.ok) throw new HubError(data.error ?? 'error', data.message);
    return data.status;
  }

  async req(op: string, args?: Record<string, unknown>): Promise<Record<string, unknown>> {
    if (!this.session || this.state.status !== 'ready') throw new HubError('disconnected', 'not connected');
    const res = await fetch(`${this.opts.url}/req`, { method: 'POST', headers: this.headers(), body: JSON.stringify({ op, ...(args ? { args } : {}) }) });
    const data = await res.json();
    if (!res.ok) throw new HubError(data.error ?? 'error', data.message);
    if (data.ok === false) throw new HubError(data.code, data.message);
    return data.result ?? {};
  }

  // -- internal --

  private headers(): Record<string, string> {
    return { 'Content-Type': 'application/json', ...(this.session ? { Authorization: `Bearer ${this.session}` } : {}) };
  }

  private async doAuth(): Promise<void> {
    if (this.stopped) return;
    this.authAbort = new AbortController();
    try {
      const res = await fetch(`${this.opts.url}/auth`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ token: this.opts.token }),
        signal: this.authAbort.signal,
      });
      const data = await res.json();
      if (this.stopped) return;

      if (!res.ok) {
        if (data.error === 'unauthorized') { this.fatal('unauthorized', 'unauthorized'); return; }
        if (data.error === 'hub_offline') { this.scheduleRetry(); return; }
        throw new Error(data.error ?? 'auth failed');
      }

      this.session = data.session;

      for (const raw of data.messages as unknown[]) {
        const msg = asMessage(raw);
        if (!msg) continue;
        if (msg.type === 'snapshot') this.live = true;
        this.set(reduce(this.state, msg));
      }
      if (this.state.status === 'ready') this.attempt = 0;

      this.startSSE();
    } catch {
      if (!this.stopped) this.scheduleRetry();
    } finally {
      this.authAbort = null;
    }
  }

  private startSSE(): void {
    if (this.stopped || !this.session) return;
    const ES = (globalThis as { EventSource?: typeof EventSource }).EventSource;
    if (!ES) return;

    const url = `${this.opts.url}/events?session=${encodeURIComponent(this.session)}`;
    const es = new ES(url);
    this.es = es;

    es.onmessage = (ev: MessageEvent) => {
      let parsed: unknown;
      try { parsed = JSON.parse(ev.data as string); } catch { return; }
      const msg = asMessage(parsed);
      if (!msg) return;

      if (msg.type === 'history') {
        for (const fn of [...this.historyListeners]) fn(msg as HistoryPush);
        return;
      }

      this.set(reduce(this.state, msg));
    };

    es.onerror = () => {
      es.close();
      if (this.es === es) this.es = null;
      if (this.stopped) return;
      this.session = null;
      this.set(withStatus(markedStale(this.state, Date.now()), 'reconnecting'));
      this.scheduleRetry();
    };
  }

  private set(next: HomeState): void {
    if (next === this.state) return;
    this.state = next;
    for (const fn of [...this.listeners]) fn();
  }

  private fatal(status: ConnectionStatus, error: string): void {
    this.stopped = true;
    this.set(withStatus(markedStale(this.state, Date.now()), status, error));
  }

  private scheduleRetry(): void {
    if (this.stopped || !this.opts.reconnect) return;
    const { min, max } = this.opts.backoffMs;
    const base = Math.min(max, min * 2 ** this.attempt);
    const delay = Math.round(base * (0.8 + Math.random() * 0.4));
    this.attempt++;
    this.retryTimer = setTimeout(() => { this.retryTimer = null; this.connect(); }, delay);
  }

  private clearRetry(): void {
    if (this.retryTimer) clearTimeout(this.retryTimer);
    this.retryTimer = null;
  }
}
