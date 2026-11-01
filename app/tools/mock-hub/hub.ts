// A software stand-in for the hub that speaks docs/app-protocol.md, so the client
// library and the app can be developed and tested without hardware. It mirrors the
// real hub's rules where the protocol states them (handshake timeout, 3 clients,
// failed-auth lockout, validation, re-broadcast after mutations).
import { createServer as createHttpsServer, type Server as HttpsServer } from 'node:https';
import { WebSocketServer, type WebSocket, type RawData } from 'ws';
import {
  MAX_FRAME_BYTES,
  PROTOCOL_VERSION,
  type Action,
  type AckStatus,
  type Attr,
  type Chip,
  type Days,
  type Entity,
  type ErrorCode,
  type HistoryEvent,
  type HubInfo,
  type MatterCode,
  type MatterState,
  type WifiNetwork,
  type Rule,
  type Scene,
  type Schedule,
  type ServerMessage,
  type Timer,
  type Value,
} from '../../src/protocol/messages';

export const MAX_CLIENTS = 3;
const MAX_RULES = 16;
const MAX_SCHEDULES = 8;
const MAX_TIMERS = 8;
const AUTH_FAILURES_BEFORE_LOCKOUT = 5;
const RING_SIZE = 256; // the real hub's event ring
const HISTORY_PAGE = 24;

export interface MockHubOptions {
  /** 0 = pick a free port. */
  port?: number;
  token?: string;
  helloTimeoutMs?: number;
  /** Delay before the chip's ACK follows `accepted`. */
  ackDelayMs?: number;
  lockoutMs?: number;
  /** Push new events as they happen (the real hub does); false behaves like a hub from before live history. */
  pushHistory?: boolean;
  name?: string;
  /** Entities per snapshot frame (0 = everything in one frame), to exercise parts. */
  snapshotChunk?: number;
  /** Serve wss:// with this identity (PEM strings) instead of plain ws://. */
  tls?: { key: string; cert: string } | null;
  /** The real hub sends something at least every 10 s (chip telemetry); the mock sends a `time` event this often (0 = never). */
  heartbeatMs?: number;
  /**
   * The first-time setup code (12 characters, as the real hub's label has it). A client that signs in with it is a setup
   * client: no snapshot, no events, only `wifi.scan` and `setup.apply`. null = this hub has no setup mode.
   */
  setupCode?: string | null;
}

/** An event as the mock keeps it: the wire event with an absolute time instead of an age. */
type MockEvent = Omit<HistoryEvent, 'age'> & { at: number };

interface TimerEntry {
  entity: number;
  attr: number;
  action: Action;
  scene?: string;
  dueAt: number;
  handle: ReturnType<typeof setTimeout>;
}

interface ClientInfo {
  ws: WebSocket;
  ip: string;
  authed: boolean;
  /** Signed in with the setup code. */
  setupOnly: boolean;
  helloTimer: ReturnType<typeof setTimeout>;
}

const DEVICE_FIRST_ENTITY = 0x0100;

function attr(id: number, name: string, vt: Attr['vt'], flags: Attr['flags'], value: Value): Attr {
  return { id, name, vt, flags, value };
}

export function defaultEntities(): Entity[] {
  return [
    {
      id: DEVICE_FIRST_ENTITY,
      name: 'plug1',
      chip: 1,
      device: 'switch',
      attrs: [
        attr(0x01, 'on_off', 'bool', ['r', 'w', 'rep'], true),
        attr(0x13, 'power_w', 'u16', ['r', 'rep'], 3),
        attr(0x14, 'energy_wh', 'u32', ['r', 'rep'], 1280),
      ],
    },
    {
      id: DEVICE_FIRST_ENTITY + 1,
      name: 'lamp',
      chip: 1,
      device: 'light',
      attrs: [
        attr(0x01, 'on_off', 'bool', ['r', 'w', 'rep'], false),
        attr(0x02, 'brightness', 'u8', ['r', 'w', 'rep'], 128),
      ],
    },
    {
      id: DEVICE_FIRST_ENTITY + 2,
      name: null,
      chip: 1,
      device: 'occupancy',
      attrs: [attr(0x50, 'occupied', 'bool', ['r', 'rep'], false)],
    },
  ];
}

export function defaultChips(): Chip[] {
  return [
    {
      id: 1,
      label: 'zigbee',
      status: 'ready',
      boot_id: 129,
      reboots: 0,
      telemetry: { heap: 377260, uptime_s: 4480, tx_frames: 1880, crc_errors: 0 },
    },
    {
      id: 2,
      label: 'thread',
      status: 'ready',
      boot_id: 32,
      reboots: 0,
      telemetry: { heap: 237124, uptime_s: 4350, tx_frames: 1757, crc_errors: 0 },
    },
  ];
}

export class MockHub {
  readonly token: string;
  entities: Entity[] = defaultEntities();
  chips: Chip[] = defaultChips();
  rules: Rule[] = [];
  schedules: Schedule[] = [];
  scenes: Scene[] = [];
  private timers: TimerEntry[] = [];
  private events: MockEvent[] = [];
  private eventSeq = 1;
  private pushedSeq = 0;
  private pushTimer: ReturnType<typeof setTimeout> | null = null;
  private clients = new Set<ClientInfo>();
  private failures = new Map<string, { count: number; until: number }>();
  private wss: WebSocketServer;
  private https: HttpsServer | null = null;
  private seq = 1;
  /** How many `hello` frames the hub has received (a pinned client must send none to a wrong server). */
  helloCount = 0;
  readonly opts: Required<MockHubOptions>;
  private heartbeat: ReturnType<typeof setInterval> | null = null;
  /** When set, every command is answered with this ACK status (and changes nothing unless 'ok'). */
  forceAck: AckStatus | null = null;
  /** The running update, if any: the mock pretends to make progress. */
  ota: { state: string; pct: number; chip: number; message: string } | null = null;
  /** The relay the hub would talk to, if one is set up. */
  relayUrl: string | null = null;
  /** What the app asked the hub to do to itself. */
  rebootCalls = 0;
  factoryResetCalls = 0;
  wifiForgetCalls = 0;

  private constructor(wss: WebSocketServer, opts: Required<MockHubOptions>) {
    this.wss = wss;
    this.opts = opts;
    this.token = opts.token;
    this.record({ kind: 'boot', what: 'power_on' });
    wss.on('connection', (ws, req) => this.onConnection(ws, req.socket.remoteAddress ?? '?'));
    if (opts.heartbeatMs > 0) {
      this.heartbeat = setInterval(() => this.broadcast({ type: 'time', time: Math.floor(Date.now() / 1000), tz: 'UTC', synced: true }), opts.heartbeatMs);
      this.heartbeat.unref();
    }
  }

  static async start(options: MockHubOptions = {}): Promise<MockHub> {
    const opts: Required<MockHubOptions> = {
      port: 0,
      token: 'test-token',
      helloTimeoutMs: 5000,
      ackDelayMs: 20,
      lockoutMs: 60_000,
      pushHistory: true,
      name: 'mock-home',
      snapshotChunk: 0,
      tls: null,
      heartbeatMs: 10_000,
      setupCode: null,
      ...options,
    };
    if (opts.tls) {
      const https = createHttpsServer({ key: opts.tls.key, cert: opts.tls.cert });
      const wss = new WebSocketServer({ server: https, maxPayload: MAX_FRAME_BYTES });
      await new Promise<void>((resolve) => https.listen(opts.port, '127.0.0.1', () => resolve()));
      const hub = new MockHub(wss, opts);
      hub.https = https;
      return hub;
    }
    const wss = new WebSocketServer({ port: opts.port, host: '127.0.0.1', maxPayload: MAX_FRAME_BYTES });
    await new Promise<void>((resolve) => wss.once('listening', () => resolve()));
    return new MockHub(wss, opts);
  }

  get port(): number {
    const a = (this.https ?? this.wss).address();
    return typeof a === 'object' && a ? a.port : 0;
  }
  get url(): string {
    return `${this.https ? 'wss' : 'ws'}://127.0.0.1:${this.port}/ws`;
  }
  get clientCount(): number {
    return [...this.clients].filter((c) => c.authed).length;
  }

  async close(): Promise<void> {
    if (this.heartbeat) clearInterval(this.heartbeat);
    if (this.pushTimer) clearTimeout(this.pushTimer);
    for (const t of this.timers) clearTimeout(t.handle);
    this.timers = [];
    for (const c of this.clients) {
      clearTimeout(c.helloTimer);
      c.ws.terminate();
    }
    await new Promise<void>((resolve) => this.wss.close(() => resolve()));
    if (this.https) {
      this.https.closeAllConnections();
      await new Promise<void>((resolve) => this.https!.close(() => resolve()));
    }
  }

  /** What the app asked for with pair / identify / unpair (for tests). */
  readonly pairCalls: number[] = [];
  readonly scenesApplied: string[] = [];
  readonly identifyCalls: number[] = [];
  readonly unpairCalls: number[] = [];
  /** What the app asked for with matter.commission (for tests). */
  readonly matterCalls: { kind: number; payload: string }[] = [];
  /** How the attempt to add a Matter device stands (the hub's Matter chip does the work). */
  matter: { state: MatterState; code: MatterCode; entity: number; message: string } | null = null;

  /** What the Matter chip says about the attempt (for tests). */
  simulateMatter(state: MatterState, code: MatterCode = 'ok', entity = 0, message = ''): void {
    this.matter = { state, code, entity, message };
    this.broadcast({ type: 'matter', state, code, entity, message });
  }

  /** A device joining while the network is open: it appears in every client's list. */
  simulateJoin(id: number, name: string | null = null): void {
    this.entities.push({
      id, name, chip: 1, device: 'switch',
      attrs: [attr(0x01, 'on_off', 'bool', ['r', 'w', 'rep'], false)],
    });
    this.resync();
  }

  /** How many timers are pending (for tests). */
  timersCount(): number {
    return this.timers.length;
  }

  /** Drop every connection without a goodbye (to test reconnects). */
  dropAll(): void {
    for (const c of this.clients) c.ws.terminate();
  }

  // ---- simulated device-side changes ------------------------------------------

  /** Put an event in the history ring, as the hub's producers do. */
  private record(e: Omit<HistoryEvent, 'seq' | 'age'>): void {
    this.events.push({ ...e, seq: this.eventSeq++, at: Date.now() });
    if (this.events.length > RING_SIZE) this.events.shift();
    if (this.opts.pushHistory && this.pushTimer === null) this.pushTimer = setTimeout(() => this.pushHistory(), 5);
  }

  /** What the real hub's main loop does every 250 ms: the events nobody has been told about go out in one message. */
  private pushHistory(): void {
    this.pushTimer = null;
    const fresh = this.events.filter((e) => e.seq > this.pushedSeq).slice(-HISTORY_PAGE);
    const latest = this.events[this.events.length - 1]?.seq ?? 0;
    this.pushedSeq = latest;
    if (fresh.length === 0) return;
    const now = Date.now();
    this.broadcast({
      type: 'history',
      now: Math.floor(now / 1000),
      latest,
      events: fresh.reverse().map(({ at, ...e }): HistoryEvent => ({ ...e, age: Math.max(0, Math.round((now - at) / 1000)) })),
    });
  }

  /** A device reports a new value (as if a STATE arrived from a chip). */
  setValue(entity: number, attrId: number, value: Value): void {
    const a = this.attrOf(entity, attrId);
    if (!a || a.value === value) return;
    a.value = value;
    // Not the noisy measurements (power, energy, light level), and not a button's return to idle.
    if (![0x12, 0x13, 0x14].includes(attrId) && !(a.name === 'event' && value === 0)) {
      this.record({ kind: 'state', entity, attr: attrId, value });
    }
    this.broadcast({ type: 'state', entity, attr: attrId, value });
  }

  setChipStatus(id: number, status: Chip['status']): void {
    const c = this.chips.find((x) => x.id === id);
    if (!c) return;
    if (status !== c.status && (status === 'silent' || c.status === 'silent')) {
      this.record({ kind: 'chip', what: status === 'silent' ? 'lost' : 'resumed', ref: c.id, value: 20 });
    }
    c.status = status;
    this.broadcast({ type: 'chip', id: c.id, status: c.status, boot_id: c.boot_id, reboots: c.reboots, telemetry: c.telemetry });
  }

  // ---- connection handling ----------------------------------------------------

  private onConnection(ws: WebSocket, ip: string): void {
    const ban = this.failures.get(ip);
    if (ban && ban.count >= AUTH_FAILURES_BEFORE_LOCKOUT && Date.now() < ban.until) {
      this.sendTo(ws, { type: 'error', code: 'unauthorized', message: 'locked out' });
      ws.close();
      return;
    }
    if (this.clientCount >= MAX_CLIENTS) {
      this.sendTo(ws, { type: 'error', code: 'busy' });
      ws.close();
      return;
    }
    const client: ClientInfo = {
      ws,
      ip,
      authed: false,
      setupOnly: false,
      helloTimer: setTimeout(() => {
        this.sendTo(ws, { type: 'error', code: 'unauthorized', message: 'hello timeout' });
        ws.close();
      }, this.opts.helloTimeoutMs),
    };
    this.clients.add(client);
    ws.on('message', (data: RawData, isBinary: boolean) => this.onFrame(client, data, isBinary));
    ws.on('close', () => {
      clearTimeout(client.helloTimer);
      this.clients.delete(client);
    });
    ws.on('error', () => ws.terminate());
  }

  private onFrame(c: ClientInfo, data: RawData, isBinary: boolean): void {
    if (isBinary) return this.fail(c, 'bad_json');
    const text = data.toString('utf8');
    if (text.length > MAX_FRAME_BYTES) return this.fail(c, 'too_large');
    let msg: Record<string, unknown>;
    try {
      const v: unknown = JSON.parse(text);
      if (typeof v !== 'object' || v === null || Array.isArray(v)) throw new Error('not an object');
      msg = v as Record<string, unknown>;
    } catch {
      return this.fail(c, 'bad_json');
    }

    if (!c.authed) {
      if (msg.type !== 'hello') return this.fail(c, 'unauthorized');
      this.helloCount += 1;
      return this.hello(c, msg);
    }
    const id = typeof msg.id === 'number' ? msg.id : undefined;
    switch (msg.type) {
      case 'cmd':
        if (c.setupOnly) return this.sendTo(c.ws, { type: 'error', ...(id !== undefined ? { id } : {}), code: 'unsupported', message: 'setup only' });
        return this.command(c, id, msg);
      case 'req':
        if (c.setupOnly && msg.op !== 'wifi.scan' && msg.op !== 'setup.apply') {
          if (id === undefined) return this.sendTo(c.ws, { type: 'error', code: 'bad_args', message: 'missing id' });
          return this.sendTo(c.ws, { type: 'res', id, ok: false, code: 'unsupported', message: 'setup only' });
        }
        return this.request(c, id, msg);
      default:
        this.sendTo(c.ws, { type: 'error', ...(id !== undefined ? { id } : {}), code: 'unsupported' });
    }
  }

  private fail(c: ClientInfo, code: ErrorCode): void {
    this.sendTo(c.ws, { type: 'error', code });
    if (!c.authed || code === 'too_large') c.ws.close();
  }

  private hello(c: ClientInfo, msg: Record<string, unknown>): void {
    if (msg.v !== PROTOCOL_VERSION) {
      this.sendTo(c.ws, { type: 'error', code: 'version' });
      c.ws.close();
      return;
    }
    const viaToken = typeof msg.token === 'string' && constantTimeEqual(msg.token, this.opts.token);
    const typed = typeof msg.token === 'string' ? msg.token.replace(/[-\s]/g, '').toUpperCase() : '';
    const viaSetup = !viaToken && this.opts.setupCode !== null && constantTimeEqual(typed, this.opts.setupCode);
    if (!viaToken && !viaSetup) {
      const f = this.failures.get(c.ip) ?? { count: 0, until: 0 };
      f.count += 1;
      f.until = Date.now() + this.opts.lockoutMs;
      this.failures.set(c.ip, f);
      this.sendTo(c.ws, { type: 'error', code: 'unauthorized' });
      c.ws.close();
      return;
    }
    this.failures.delete(c.ip);
    clearTimeout(c.helloTimer);
    c.authed = true;
    c.setupOnly = viaSetup;
    this.sendTo(c.ws, {
      type: 'welcome',
      v: PROTOCOL_VERSION,
      hub: this.hubInfo(),
      ...(viaSetup ? { setup: true } : {}),
    });
    if (!viaSetup) this.sendSnapshot(c.ws);
  }

  /** Send the whole picture, in parts of `snapshotChunk` entities if configured. */
  private sendSnapshot(ws: WebSocket): void {
    const n = this.opts.snapshotChunk > 0 ? this.opts.snapshotChunk : Math.max(1, this.entities.length);
    let part = 0;
    for (let i = 0; i < this.entities.length || part === 0; i += n) {
      const lastEntities = i + n >= this.entities.length;
      this.sendTo(ws, {
        type: 'snapshot',
        part: part++,
        last: lastEntities,
        entities: this.entities.slice(i, i + n),
        ...(lastEntities
          ? { chips: this.chips, rules: this.rules, schedules: this.schedules, timers: this.timerList(), scenes: this.scenes }
          : {}),
      });
      if (lastEntities) break;
    }
  }

  /** Re-send the snapshot to every client (what the real hub does after an event overflow). */
  resync(): void {
    for (const c of this.clients) if (c.authed && !c.setupOnly) this.sendSnapshot(c.ws);
  }

  /** Add `count` plain switches (ids from 0x0200) to the home, before clients connect. */
  addEntities(count: number): void {
    for (let i = 0; i < count; i++) {
      this.entities.push({
        id: 0x0200 + i,
        name: null,
        chip: 1,
        device: 'switch',
        attrs: [attr(0x01, 'on_off', 'bool', ['r', 'w', 'rep'], false)],
      });
    }
  }

  // ---- commands ---------------------------------------------------------------

  private command(c: ClientInfo, id: number | undefined, msg: Record<string, unknown>): void {
    const err = (code: ErrorCode, message?: string) =>
      this.sendTo(c.ws, { type: 'error', ...(id !== undefined ? { id } : {}), code, ...(message ? { message } : {}) });
    if (id === undefined) return err('bad_args', 'missing id');
    const entity = this.entities.find((e) => e.id === msg.entity);
    if (!entity) return err('not_found', 'entity');
    const a = entity.attrs.find((x) => x.id === msg.attr);
    if (!a) return err('not_found', 'attr');
    if (!a.flags.includes('w')) return err('read_only');
    if (!valueFits(a.vt, msg.value)) return err('bad_args', `value does not fit ${a.vt}`);

    this.sendTo(c.ws, { type: 'accepted', id, seq: this.seq++ });
    const chip = this.chips.find((x) => x.id === entity.chip);
    const status: AckStatus = this.forceAck ?? (chip && chip.status === 'ready' ? 'ok' : 'unreachable');
    this.record({ kind: 'command', what: 'app', entity: entity.id, attr: a.id, value: msg.value as Value });
    if (status !== 'ok') this.record({ kind: 'command_failed', what: status, entity: entity.id, attr: a.id });
    setTimeout(() => {
      if (status === 'ok') this.setValue(entity.id, a.id, msg.value as Value);
      this.sendTo(c.ws, { type: 'ack', id, status });
    }, this.opts.ackDelayMs);
  }

  // ---- management requests ----------------------------------------------------

  private request(c: ClientInfo, id: number | undefined, msg: Record<string, unknown>): void {
    if (id === undefined) return this.sendTo(c.ws, { type: 'error', code: 'bad_args', message: 'missing id' });
    const args = (typeof msg.args === 'object' && msg.args !== null ? msg.args : {}) as Record<string, unknown>;
    const ok = (result?: Record<string, unknown>) =>
      this.sendTo(c.ws, { type: 'res', id, ok: true, ...(result ? { result } : {}) });
    const no = (code: ErrorCode, message?: string) =>
      this.sendTo(c.ws, { type: 'res', id, ok: false, code, ...(message ? { message } : {}) });

    switch (msg.op) {
      case 'name.set': {
        if (typeof args.entity !== 'number') return no('bad_args', 'entity');
        const e = this.entities.find((x) => x.id === args.entity);
        if (!e) return no('not_found', 'entity');
        if (typeof args.name !== 'string' || !/^[a-z][a-z0-9_]{0,22}$/.test(args.name)) return no('bad_args', 'name');
        if (this.entities.some((x) => x.name === args.name && x.id !== e.id)) return no('bad_args', 'name in use');
        e.name = args.name;
        this.broadcast({ type: 'entity', op: 'add', entity: e });
        return ok();
      }
      case 'name.clear': {
        if (typeof args.entity !== 'number') return no('bad_args', 'entity');
        const e = this.entities.find((x) => x.id === args.entity);
        if (!e) return no('not_found', 'entity');
        e.name = null;
        this.broadcast({ type: 'entity', op: 'add', entity: e });
        return ok();
      }
      case 'rule.add': {
        if (this.rules.length >= MAX_RULES) return no('table_full');
        const rule = this.parseRule(args);
        if ('code' in rule) return no(rule.code, rule.message);
        this.rules.push({ ...rule, index: this.rules.length });
        this.broadcast({ type: 'rules', rules: this.rules });
        return ok({ index: this.rules.length - 1 });
      }
      case 'rule.del': {
        const i = args.index;
        if (typeof i !== 'number' || !Number.isInteger(i) || i < 0 || i >= this.rules.length) return no('not_found', 'index');
        this.rules.splice(i, 1);
        this.rules.forEach((r, k) => (r.index = k));
        this.broadcast({ type: 'rules', rules: this.rules });
        return ok();
      }
      case 'schedule.add': {
        if (this.schedules.length >= MAX_SCHEDULES) return no('table_full');
        const s = this.parseSchedule(args);
        if ('code' in s) return no(s.code, s.message);
        this.schedules.push({ ...s, index: this.schedules.length });
        this.broadcast({ type: 'schedules', schedules: this.schedules });
        return ok({ index: this.schedules.length - 1 });
      }
      case 'schedule.del': {
        const i = args.index;
        if (typeof i !== 'number' || !Number.isInteger(i) || i < 0 || i >= this.schedules.length) return no('not_found', 'index');
        this.schedules.splice(i, 1);
        this.schedules.forEach((s, k) => (s.index = k));
        this.broadcast({ type: 'schedules', schedules: this.schedules });
        return ok();
      }
      case 'timer.add': {
        if (this.timers.length >= MAX_TIMERS) return no('table_full');
        if (args.scene !== undefined) {
          const delay = args.delay_s;
          if (args.entity !== undefined || args.action !== undefined) return no('bad_args', 'scene excludes entity/action');
          if (typeof args.scene !== 'string' || !/^[a-z][a-z0-9_]{0,22}$/.test(args.scene)) return no('bad_args', 'scene: a scene name');
          if (!this.scenes.some((s) => s.name === args.scene)) return no('not_found', 'no such scene');
          if (typeof delay !== 'number' || !Number.isInteger(delay) || delay < 1 || delay > 86_400 * 7) return no('bad_args', 'delay_s');
          const entry: TimerEntry = {
            entity: 0xfffe,
            attr: 0,
            action: 'on',
            scene: args.scene,
            dueAt: Date.now() + delay * 1000,
            handle: setTimeout(() => this.fireTimer(entry), delay * 1000),
          };
          this.timers.push(entry);
          this.broadcast({ type: 'timers', timers: this.timerList() });
          return ok({ index: this.timers.length - 1 });
        }
        const e = this.entities.find((x) => x.id === args.entity);
        const action = args.action;
        const delay = args.delay_s;
        if (!e) return no('not_found', 'entity');
        if (action !== 'on' && action !== 'off' && action !== 'toggle') return no('bad_args', 'action');
        if (typeof delay !== 'number' || !Number.isInteger(delay) || delay < 1 || delay > 86_400 * 7) return no('bad_args', 'delay_s');
        const onoff = e.attrs.find((a) => a.name === 'on_off');
        if (!onoff) return no('bad_args', 'entity has no on_off');
        const entry: TimerEntry = {
          entity: e.id,
          attr: onoff.id,
          action,
          dueAt: Date.now() + delay * 1000,
          handle: setTimeout(() => this.fireTimer(entry), delay * 1000),
        };
        this.timers.push(entry);
        this.broadcast({ type: 'timers', timers: this.timerList() });
        return ok({ index: this.timers.length - 1 });
      }
      case 'timer.cancel': {
        const i = args.index;
        if (typeof i !== 'number' || !Number.isInteger(i) || i < 0 || i >= this.timers.length) return no('not_found', 'index');
        clearTimeout(this.timers[i]!.handle);
        this.timers.splice(i, 1);
        this.broadcast({ type: 'timers', timers: this.timerList() });
        return ok();
      }
      case 'scene.put': {
        const name = args.name;
        if (typeof name !== 'string' || !/^[a-z][a-z0-9_]{0,22}$/.test(name)) return no('bad_args', 'name');
        const list = args.actions;
        if (!Array.isArray(list) || list.length < 1 || list.length > 8) return no('bad_args', 'actions: 1..8 items');
        const actions: Scene['actions'] = [];
        for (const raw of list as Record<string, unknown>[]) {
          const e = this.entities.find((x) => x.id === raw?.entity);
          if (!e) return no('not_found', 'no such entity');
          const a = e.attrs.find((x) => x.id === raw.attr);
          if (!a) return no('not_found', 'no such attribute');
          if (!a.flags.includes('w')) return no('read_only', 'action: attribute is read-only');
          if (!valueFits(a.vt, raw.value)) return no('bad_args', 'action value does not fit the attribute');
          if (actions.some((x) => x.entity === e.id && x.attr === a.id)) return no('bad_args', 'the same attribute twice');
          actions.push({ entity: e.id, attr: a.id, value: raw.value as Value });
        }
        const i = this.scenes.findIndex((x) => x.name === name);
        if (i < 0 && this.scenes.length >= 8) return no('table_full', 'scene table is full');
        if (i >= 0) this.scenes[i] = { name, actions };
        else this.scenes.push({ name, actions });
        this.resync();
        return ok();
      }
      case 'scene.del': {
        const i = this.scenes.findIndex((x) => x.name === args.name);
        if (i < 0) return no('not_found', 'no such scene');
        const used =
          this.schedules.some((s) => s.scene === args.name) ||
          this.timers.some((t) => t.scene === args.name) ||
          this.rules.some((r) => 'scene' in r.then && r.then.scene === args.name);
        if (used) return no('bad_args', 'scene is used by a schedule, timer or rule');
        this.scenes.splice(i, 1);
        this.resync();
        return ok();
      }
      case 'scene.apply': {
        const sc = this.scenes.find((x) => x.name === args.name);
        if (!sc) return no('not_found', 'no such scene');
        return ok(this.applyScene(sc));
      }
      case 'history': {
        // Same arguments and limits as the real hub's core::app::history_parse; newest first, strictly older than `before`.
        const { before, limit, entity } = args;
        if (before !== undefined && (typeof before !== 'number' || !Number.isInteger(before) || before < 0 || before > 0xffffffff)) return no('bad_args', 'before');
        if (limit !== undefined && (typeof limit !== 'number' || !Number.isInteger(limit) || limit < 1 || limit > HISTORY_PAGE)) return no('bad_args', 'limit: 1..24');
        if (entity !== undefined && (typeof entity !== 'number' || !Number.isInteger(entity) || entity < 0 || entity > 0xffff)) return no('bad_args', 'entity');
        const aboutEntity = (e: MockEvent) => ['state', 'command', 'command_failed', 'reconcile', 'pairing', 'rule'].includes(e.kind as string) && e.entity === entity;
        const now = Date.now();
        const matching = this.events.filter((e) => (before === undefined || before === 0 || e.seq < before) && (entity === undefined || aboutEntity(e)));
        const take = limit ?? HISTORY_PAGE;
        const page = matching
          .slice(-take)
          .reverse()
          .map(({ at, ...e }): HistoryEvent => ({ ...e, age: Math.max(0, Math.round((now - at) / 1000)) }));
        const oldest = this.events[0]?.seq ?? 0;
        const latest = this.events[this.events.length - 1]?.seq ?? 0;
        return ok({ now: Math.floor(now / 1000), latest, oldest, more: matching.length > take, events: page });
      }
      case 'wifi.scan': {
        return ok({ networks: [...this.networks].sort((a, b) => b.rssi - a.rssi) });
      }
      case 'wifi.set': {
        const { ssid, password } = args;
        if (typeof ssid !== 'string' || ssid.length < 1 || new TextEncoder().encode(ssid).length > 32 || /[\u0000-\u001f]/.test(ssid)) return no('bad_args', 'ssid: 1..32 bytes');
        if (typeof password !== 'string') return no('bad_args', 'password');
        const hex64 = password.length === 64 && /^[0-9a-fA-F]+$/.test(password);
        if (password !== '' && !hex64 && (password.length < 8 || password.length > 63)) return no('bad_args', 'password: empty (open network), or 8..63 characters');
        // The real hub answers, then drops every connection while it tries the new network, and goes back if
        // it cannot join. The mock does the same, with a short trial.
        const net = this.networks.find((n) => n.ssid === ssid);
        const works = !!net && (!net.secure || password === 'goodpass1');
        const entry = { ssid, password, kept: works };
        this.wifiRequests.push(entry);
        setTimeout(() => {
          this.dropAll();
          if (works) this.wifi = { ssid, rssi: net!.rssi, ip: '192.168.0.77' };
        }, 30);
        return ok();
      }
      case 'setup.apply': {
        // Like the real hub: validate, answer, then (after a moment) drop everyone, switch to the new network and take
        // the new token. A network that does not exist, or a wrong password, leaves it on no network, still in setup.
        const { ssid, password, token } = args;
        if (typeof ssid !== 'string' || ssid.length < 1 || new TextEncoder().encode(ssid).length > 32 || /[\u0000-\u001f]/.test(ssid)) return no('bad_args', 'ssid: 1..32 bytes');
        if (typeof password !== 'string') return no('bad_args', 'password');
        const hex64 = password.length === 64 && /^[0-9a-fA-F]+$/.test(password);
        if (password !== '' && !hex64 && (password.length < 8 || password.length > 63)) return no('bad_args', 'password: empty (open network), or 8..63 characters');
        if (typeof token !== 'string' || !/^[\x21-\x7e]{8,64}$/.test(token)) return no('bad_args', 'token: 8..64 printable characters');
        const net = this.networks.find((n) => n.ssid === ssid);
        const works = !!net && (!net.secure || password === 'goodpass1');
        this.setupRequests.push({ ssid, password, token, kept: works });
        setTimeout(() => {
          this.opts.token = token;
          this.dropAll();
          if (works) this.wifi = { ssid, rssi: net!.rssi, ip: '192.168.0.77' };
        }, 30);
        return ok();
      }
      case 'location.set': {
        const { lat, lon } = args;
        if (typeof lat !== 'number' || typeof lon !== 'number' || !Number.isFinite(lat) || !Number.isFinite(lon)) return no('bad_args', 'lat and lon: numbers');
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return no('bad_args', 'lat -90..90, lon -180..180');
        this.location = { lat, lon };
        this.broadcast({ type: 'hub', hub: this.hubInfo() });
        return ok();
      }
      case 'location.clear': {
        this.location = null;
        this.broadcast({ type: 'hub', hub: this.hubInfo() });
        return ok();
      }
      case 'pair': {
        let seconds = 180;
        if (args.seconds !== undefined) {
          if (typeof args.seconds !== 'number' || !Number.isInteger(args.seconds) || args.seconds < 1 || args.seconds > 254) return no('bad_args', 'seconds: 1..254');
          seconds = args.seconds;
        }
        this.pairCalls.push(seconds);
        return ok();
      }
      case 'identify':
      case 'unpair': {
        if (typeof args.entity !== 'number' || !Number.isInteger(args.entity)) return no('bad_args', 'entity');
        const e = this.entities.find((x) => x.id === args.entity);
        if (!e) return no('not_found', 'no such entity');
        if (e.chip !== 1) return no('bad_args', 'not a Zigbee device');
        if (msg.op === 'identify') {
          this.identifyCalls.push(e.id);
        } else {
          this.entities = this.entities.filter((x) => x.id !== e.id);
          this.unpairCalls.push(e.id);
          this.resync(); // the real hub re-sends the snapshot when a device goes away
        }
        return ok();
      }
      case 'matter.commission': {
        // The app hands the bytes it read from the air; the hub's Matter stack parses
        // them. The kinds are the ones the app knows: 0 — the commissioning service
        // data (only identifies the device), 1 — the enhanced advertisement (the whole
        // onboarding payload), 2 — the payload of a QR code.
        const { kind, payload } = args;
        if (kind !== 0 && kind !== 1 && kind !== 2) return no('bad_args', 'kind: 0, 1 or 2');
        if (typeof payload !== 'string' || payload.length % 2 !== 0 || !/^[0-9a-f]*$/.test(payload)) {
          return no('bad_args', 'payload: hex');
        }
        const bytes = payload.length / 2;
        if (kind === 0 && bytes !== 7) return no('bad_args', 'payload: 7 bytes of service data');
        if (kind !== 0 && bytes < 11) return no('bad_args', 'payload: at least 11 bytes');
        this.matterCalls.push({ kind, payload });
        // The hub starts the attempt; the Matter chip reports how far it got (a `matter` push).
        this.matter = { state: 'scanning', code: 'ok', entity: 0, message: 'the hub is working on it' };
        return ok({ state: 'started' });
      }
      case 'matter.status': {
        const m = this.matter;
        if (!m) return ok({ state: 'idle', entity: 0, code: 'ok', message: '' });
        return ok({ state: m.state, entity: m.entity, code: m.code, message: m.message });
      }
      case 'ota.start': {
        const { chip, url, sha256 } = args;
        if (chip !== 1 && chip !== 2) return no('bad_args', 'chip: 1 or 2');
        if (typeof url !== 'string' || !/^https?:\/\//.test(url)) return no('bad_args', 'url: http(s)');
        if (typeof sha256 !== 'string' || !/^[0-9a-fA-F]{64}$/.test(sha256)) return no('bad_args', 'sha256: 64 hex characters');
        this.ota = { state: 'downloading', pct: 0, chip, message: 'starting…' };
        return ok();
      }
      case 'ota.status': {
        const o = this.ota;
        if (!o) return ok({ state: 'idle', pct: 0, chip: 0, message: '' });
        if (o.state === 'downloading') {
          o.pct = Math.min(100, o.pct + 25);
          o.message = `downloading ${o.pct} %`;
          if (o.pct >= 100) o.state = 'verifying';
        } else if (o.state === 'verifying') {
          o.state = 'done';
          o.message = 'verified';
        }
        return ok({ state: o.state, pct: o.pct, chip: o.chip, message: o.message });
      }
      case 'access.get': {
        return ok({
          token: this.opts.token,
          fingerprint: 'AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99',
          remote: this.relayUrl !== null,
          relay_url: this.relayUrl ?? '',
        });
      }
      case 'access.token.set': {
        const { token } = args;
        if (typeof token !== 'string' || token.length < 8 || token.length > 64) return no('bad_args', 'token: 8..64 characters');
        if (/[^\x21-\x7e]/.test(token)) return no('bad_args', 'token: printable ASCII without spaces');
        this.opts.token = token;
        return ok();
      }
      case 'access.relay.set': {
        const { url, secret } = args;
        if (typeof url !== 'string' || !/^(wss?|https?):\/\//.test(url)) return no('bad_args', 'url: ws://, wss://, http:// or https://');
        // The secret is optional: a hub that was flashed with a secret of its own
        // uses the one in its firmware.
        if (secret !== undefined && typeof secret !== 'string') return no('bad_args', 'secret');
        this.relayUrl = url;
        return ok();
      }
      case 'access.relay.clear': {
        this.relayUrl = null;
        return ok();
      }
      case 'hub.reboot': {
        this.rebootCalls += 1;
        return ok();
      }
      case 'hub.factory_reset': {
        this.factoryResetCalls += 1;
        this.entities = [];
        this.rules = [];
        this.schedules = [];
        this.scenes = [];
        this.resync();
        return ok();
      }
      case 'wifi.forget': {
        this.wifiForgetCalls += 1;
        return ok();
      }
      default:
        return no('unsupported', String(msg.op));
    }
  }

  private fireTimer(t: TimerEntry): void {
    const i = this.timers.indexOf(t);
    if (i >= 0) this.timers.splice(i, 1);
    if (t.scene !== undefined) {
      const sc = this.scenes.find((s) => s.name === t.scene);
      this.record({ kind: 'timer', ref: Math.max(i, 0), scene: t.scene });
      if (sc) this.applyScene(sc, 'timer');
    } else {
      const a = this.attrOf(t.entity, t.attr);
      if (a) {
        const to = t.action === 'toggle' ? !a.value : t.action === 'on';
        this.record({ kind: 'timer', ref: Math.max(i, 0), entity: t.entity, attr: t.attr, value: to });
        this.setValue(t.entity, t.attr, to);
      }
    }
    this.broadcast({ type: 'timers', timers: this.timerList() });
  }

  private timerList(): Timer[] {
    return this.timers.map((t, index) => ({
      index,
      ...(t.scene !== undefined ? { scene: t.scene } : { entity: t.entity, attr: t.attr, action: t.action }),
      left_s: Math.max(0, Math.ceil((t.dueAt - Date.now()) / 1000)),
    }));
  }

  /** Same checks, in the same order and with the same codes, as the hub's core::app::rule_add. */
  private parseRule(a: Record<string, unknown>): Omit<Rule, 'index'> | { code: ErrorCode; message: string } {
    const bad = (code: ErrorCode, message: string) => ({ code, message });
    const when = a.when as Record<string, unknown> | undefined;
    const then = a.then as Record<string, unknown> | undefined;
    if (!when || !then) return bad('bad_args', 'when/then required');
    const ops = ['any', '==', '!=', '<', '<=', '>', '>='];
    const whenAttr = this.lookupAttr(when);
    if (typeof whenAttr === 'string') return bad(whenAttr === 'entity' ? 'not_found' : 'not_found', `when: no such ${whenAttr}`);
    if (typeof when.op !== 'string' || !ops.includes(when.op)) return bad('bad_args', 'when.op');
    if (when.op !== 'any' && !valueFits(whenAttr.vt, when.value)) return bad('bad_args', 'when.value does not fit the attribute');
    let thenOut: Rule['then'];
    if (then.scene !== undefined) {
      if (then.entity !== undefined || then.attr !== undefined || then.value !== undefined) return bad('bad_args', 'then: scene excludes entity/attr/value');
      if (typeof then.scene !== 'string' || !/^[a-z][a-z0-9_]{0,22}$/.test(then.scene)) return bad('bad_args', 'scene: a scene name');
      if (!this.scenes.some((s) => s.name === then.scene)) return bad('not_found', 'no such scene');
      thenOut = { scene: then.scene };
    } else {
      const thenAttr = this.lookupAttr(then);
      if (typeof thenAttr === 'string') return bad('not_found', `then: no such ${thenAttr}`);
      if (!thenAttr.flags.includes('w')) return bad('read_only', 'then: attribute is read-only');
      if (!valueFits(thenAttr.vt, then.value)) return bad('bad_args', 'then.value does not fit the attribute');
      thenOut = then as unknown as Rule['then'];
    }
    const rule: Omit<Rule, 'index'> = {
      when: when as unknown as Rule['when'],
      then: thenOut,
    };
    if (a.between !== undefined) {
      const b = a.between as Record<string, unknown>;
      if (!isTimePoint(b.from) || !isTimePoint(b.to) || b.from === b.to) return bad('bad_args', 'between');
      rule.between = { from: b.from as string, to: b.to as string };
    }
    if (a.and !== undefined) {
      const n = a.and as Record<string, unknown>;
      const andAttr = this.lookupAttr(n);
      if (typeof andAttr === 'string') return bad('not_found', `and: no such ${andAttr}`);
      if (typeof n.op !== 'string' || n.op === 'any' || !ops.includes(n.op)) return bad('bad_args', 'and.op');
      if (!valueFits(andAttr.vt, n.value)) return bad('bad_args', 'and.value does not fit the attribute');
      rule.and = n as unknown as NonNullable<Rule['and']>;
    }
    if (a.cooldown_s !== undefined) {
      const s = a.cooldown_s;
      if (typeof s !== 'number' || !Number.isInteger(s) || s < 1 || s > 65535) return bad('bad_args', 'cooldown_s');
      rule.cooldown_s = s;
    }
    return rule;
  }

  /** The attribute named by {entity, attr}, or which part of it does not exist. */
  private lookupAttr(o: Record<string, unknown>): Attr | 'entity' | 'attribute' {
    const e = this.entities.find((x) => x.id === o.entity);
    if (!e) return 'entity';
    return e.attrs.find((x) => x.id === o.attr) ?? 'attribute';
  }

  private parseSchedule(a: Record<string, unknown>): Omit<Schedule, 'index'> | { code: ErrorCode; message: string } {
    const bad = (code: ErrorCode, message: string) => ({ code, message });
    if (!isTimePoint(a.time)) return bad('bad_args', 'time');
    const names = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];
    let days = a.days as Days | undefined;
    let date: string | undefined;
    if (a.date !== undefined) {
      // A one-off: a real date, today (the mock's clock is UTC) or later, and no days.
      if (a.days !== undefined) return bad('bad_args', 'date and days exclude each other');
      const d = a.date;
      const real = typeof d === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(d) && !Number.isNaN(Date.parse(d)) && new Date(d).toISOString().slice(0, 10) === d;
      if (!real || Number(d.slice(0, 4)) < 2000 || Number(d.slice(0, 4)) > 2099) return bad('bad_args', 'date: YYYY-MM-DD');
      if (d < new Date().toISOString().slice(0, 10)) return bad('bad_args', 'date is in the past');
      date = d;
    } else {
      const daysOk =
        days === 'daily' || days === 'weekdays' || days === 'weekends' ||
        (Array.isArray(days) && days.length > 0 && days.every((d) => names.includes(d)));
      if (!daysOk) return bad('bad_args', 'days');
    }
    const when = date !== undefined ? { days: 'once' as const, date } : { days: days as Days };
    if (a.scene !== undefined) {
      if (a.entity !== undefined || a.action !== undefined) return bad('bad_args', 'scene excludes entity/action');
      if (typeof a.scene !== 'string' || !/^[a-z][a-z0-9_]{0,22}$/.test(a.scene)) return bad('bad_args', 'scene: a scene name');
      if (!this.scenes.some((s) => s.name === a.scene)) return bad('not_found', 'no such scene');
      return { time: a.time as string, ...when, scene: a.scene };
    }
    if (a.action !== 'on' && a.action !== 'off' && a.action !== 'toggle') return bad('bad_args', 'action');
    const e = this.entities.find((x) => x.id === a.entity);
    if (!e) return bad('not_found', 'no such entity');
    const onoff = e.attrs.find((x) => x.name === 'on_off' && x.flags.includes('w'));
    if (!onoff) return bad('bad_args', 'entity has no on/off switch');
    return { time: a.time as string, ...when, entity: e.id, attr: onoff.id, action: a.action };
  }

  // ---- helpers ----------------------------------------------------------------

  /** Where the hub is, set with location.set; the mock's "sun" is a fixed 06:00-18:00 once it is known. */
  location: { lat: number; lon: number } | null = null;
  /** The network the mock "is on", the networks a scan sees (a secured one needs the password `goodpass1`), and what the app asked for. */
  wifi: HubInfo['wifi'] = { ssid: 'home', rssi: -58, ip: '192.168.0.202' };
  networks: WifiNetwork[] = [
    { ssid: 'home', rssi: -58, secure: true },
    { ssid: 'neighbour', rssi: -77, secure: true },
    { ssid: 'cafe', rssi: -64, secure: false },
  ];
  readonly wifiRequests: { ssid: string; password: string; kept: boolean }[] = [];
  /** What `setup.apply` asked for (the token is the one the hub accepts from then on). */
  readonly setupRequests: { ssid: string; password: string; token: string; kept: boolean }[] = [];
  private hubInfo(): HubInfo {
    return {
      name: this.opts.name, fw: 'mock', time: Math.floor(Date.now() / 1000), tz: 'UTC', synced: true,
      location: this.location,
      wifi: this.wifi,
      sun: this.location ? { rise: '06:00', set: '18:00' } : null,
    };
  }

  /** Set every action of the scene; an action whose device is gone or whose chip is silent is skipped. */
  private applyScene(sc: Scene, via: 'app' | 'timer' = 'app'): { sent: number; skipped: number } {
    let sent = 0;
    let skipped = 0;
    for (const a of sc.actions) {
      const e = this.entities.find((x) => x.id === a.entity);
      const at = e?.attrs.find((x) => x.id === a.attr);
      if (e && at && at.flags.includes('w') && this.chips.find((c) => c.id === e.chip)?.status === 'ready') {
        this.setValue(e.id, at.id, a.value);
        sent++;
      } else {
        skipped++;
      }
    }
    this.scenesApplied.push(sc.name);
    this.record({ kind: 'scene', what: via, ref: this.scenes.indexOf(sc) + 1, scene: sc.name });
    return { sent, skipped };
  }

  private attrOf(entity: number, attrId: number): Attr | undefined {
    return this.entities.find((e) => e.id === entity)?.attrs.find((a) => a.id === attrId);
  }
  private hasAttr(entity: unknown, attrId: unknown): boolean {
    return typeof entity === 'number' && typeof attrId === 'number' && this.attrOf(entity, attrId) !== undefined;
  }

  private sendTo(ws: WebSocket, m: ServerMessage): void {
    if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(m));
  }
  private broadcast(m: ServerMessage): void {
    for (const c of this.clients) if (c.authed && !c.setupOnly) this.sendTo(c.ws, m);
  }
}

function isTimePoint(v: unknown): boolean {
  if (typeof v !== 'string') return false;
  if (/^([01]\d|2[0-3]):[0-5]\d$/.test(v)) return true;
  const m = /^(?:sunrise|sunset)(?:[+-](\d{1,3}))?$/.exec(v);
  return m !== null && (m[1] === undefined || Number(m[1]) <= 180);
}

function valueFits(vt: Attr['vt'], v: unknown): boolean {
  const int = (lo: number, hi: number) => typeof v === 'number' && Number.isInteger(v) && v >= lo && v <= hi;
  switch (vt) {
    case 'bool': return typeof v === 'boolean';
    case 'u8': case 'enum8': return int(0, 255);
    case 'i16': return int(-32768, 32767);
    case 'u16': return int(0, 65535);
    case 'u32': return int(0, 0xffffffff);
    case 'f32': return typeof v === 'number' && Number.isFinite(v);
    case 'xy': return Array.isArray(v) && v.length === 2 && v.every((n) => typeof n === 'number' && Number.isInteger(n) && n >= 0 && n <= 65535);
  }
}

function constantTimeEqual(a: string, b: string): boolean {
  let diff = a.length ^ b.length;
  const n = Math.max(a.length, b.length);
  for (let i = 0; i < n; i++) diff |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
  return diff === 0;
}
