// A software stand-in for the hub that speaks docs/app-protocol.md, so the client
// library and the app can be developed and tested without hardware. It mirrors the
// real hub's rules where the protocol states them (handshake timeout, 3 clients,
// failed-auth lockout, validation, re-broadcast after mutations).
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
  type Rule,
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

export interface MockHubOptions {
  /** 0 = pick a free port. */
  port?: number;
  token?: string;
  helloTimeoutMs?: number;
  /** Delay before the chip's ACK follows `accepted`. */
  ackDelayMs?: number;
  lockoutMs?: number;
  name?: string;
  /** Entities per snapshot frame (0 = everything in one frame), to exercise parts. */
  snapshotChunk?: number;
}

interface TimerEntry {
  entity: number;
  attr: number;
  action: Action;
  dueAt: number;
  handle: ReturnType<typeof setTimeout>;
}

interface ClientInfo {
  ws: WebSocket;
  ip: string;
  authed: boolean;
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
  private timers: TimerEntry[] = [];
  private clients = new Set<ClientInfo>();
  private failures = new Map<string, { count: number; until: number }>();
  private wss: WebSocketServer;
  private seq = 1;
  readonly opts: Required<MockHubOptions>;
  /** When set, every command is answered with this ACK status (and changes nothing unless 'ok'). */
  forceAck: AckStatus | null = null;

  private constructor(wss: WebSocketServer, opts: Required<MockHubOptions>) {
    this.wss = wss;
    this.opts = opts;
    this.token = opts.token;
    wss.on('connection', (ws, req) => this.onConnection(ws, req.socket.remoteAddress ?? '?'));
  }

  static async start(options: MockHubOptions = {}): Promise<MockHub> {
    const opts: Required<MockHubOptions> = {
      port: 0,
      token: 'test-token',
      helloTimeoutMs: 5000,
      ackDelayMs: 20,
      lockoutMs: 60_000,
      name: 'mock-home',
      snapshotChunk: 0,
      ...options,
    };
    const wss = new WebSocketServer({ port: opts.port, host: '127.0.0.1', maxPayload: MAX_FRAME_BYTES });
    await new Promise<void>((resolve) => wss.once('listening', () => resolve()));
    return new MockHub(wss, opts);
  }

  get port(): number {
    const a = this.wss.address();
    return typeof a === 'object' && a ? a.port : 0;
  }
  get url(): string {
    return `ws://127.0.0.1:${this.port}/ws`;
  }
  get clientCount(): number {
    return [...this.clients].filter((c) => c.authed).length;
  }

  async close(): Promise<void> {
    for (const t of this.timers) clearTimeout(t.handle);
    this.timers = [];
    for (const c of this.clients) {
      clearTimeout(c.helloTimer);
      c.ws.terminate();
    }
    await new Promise<void>((resolve) => this.wss.close(() => resolve()));
  }

  /** Drop every connection without a goodbye (to test reconnects). */
  dropAll(): void {
    for (const c of this.clients) c.ws.terminate();
  }

  // ---- simulated device-side changes ------------------------------------------

  /** A device reports a new value (as if a STATE arrived from a chip). */
  setValue(entity: number, attrId: number, value: Value): void {
    const a = this.attrOf(entity, attrId);
    if (!a || a.value === value) return;
    a.value = value;
    this.broadcast({ type: 'state', entity, attr: attrId, value });
  }

  setChipStatus(id: number, status: Chip['status']): void {
    const c = this.chips.find((x) => x.id === id);
    if (!c) return;
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
      return this.hello(c, msg);
    }
    const id = typeof msg.id === 'number' ? msg.id : undefined;
    switch (msg.type) {
      case 'cmd':
        return this.command(c, id, msg);
      case 'req':
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
    if (typeof msg.token !== 'string' || !constantTimeEqual(msg.token, this.opts.token)) {
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
    this.sendTo(c.ws, {
      type: 'welcome',
      v: PROTOCOL_VERSION,
      hub: { name: this.opts.name, fw: 'mock', time: Math.floor(Date.now() / 1000), tz: 'UTC', synced: true },
    });
    this.sendSnapshot(c.ws);
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
          ? { chips: this.chips, rules: this.rules, schedules: this.schedules, timers: this.timerList() }
          : {}),
      });
      if (lastEntities) break;
    }
  }

  /** Re-send the snapshot to every client (what the real hub does after an event overflow). */
  resync(): void {
    for (const c of this.clients) if (c.authed) this.sendSnapshot(c.ws);
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
      default:
        return no('unsupported', String(msg.op));
    }
  }

  private fireTimer(t: TimerEntry): void {
    const i = this.timers.indexOf(t);
    if (i >= 0) this.timers.splice(i, 1);
    const a = this.attrOf(t.entity, t.attr);
    if (a) this.setValue(t.entity, t.attr, t.action === 'toggle' ? !a.value : t.action === 'on');
    this.broadcast({ type: 'timers', timers: this.timerList() });
  }

  private timerList(): Timer[] {
    return this.timers.map((t, index) => ({
      index,
      entity: t.entity,
      attr: t.attr,
      action: t.action,
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
    const thenAttr = this.lookupAttr(then);
    if (typeof thenAttr === 'string') return bad('not_found', `then: no such ${thenAttr}`);
    if (!thenAttr.flags.includes('w')) return bad('read_only', 'then: attribute is read-only');
    if (!valueFits(thenAttr.vt, then.value)) return bad('bad_args', 'then.value does not fit the attribute');
    const rule: Omit<Rule, 'index'> = {
      when: when as unknown as Rule['when'],
      then: then as unknown as Rule['then'],
    };
    if (a.between !== undefined) {
      const b = a.between as Record<string, unknown>;
      if (!isHHMM(b.from) || !isHHMM(b.to) || b.from === b.to) return bad('bad_args', 'between');
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
    if (!isHHMM(a.time)) return bad('bad_args', 'time');
    const days = a.days as Days | undefined;
    const names = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];
    const daysOk =
      days === 'daily' || days === 'weekdays' || days === 'weekends' ||
      (Array.isArray(days) && days.length > 0 && days.every((d) => names.includes(d)));
    if (!daysOk) return bad('bad_args', 'days');
    if (a.action !== 'on' && a.action !== 'off' && a.action !== 'toggle') return bad('bad_args', 'action');
    const e = this.entities.find((x) => x.id === a.entity);
    if (!e) return bad('not_found', 'no such entity');
    const onoff = e.attrs.find((x) => x.name === 'on_off' && x.flags.includes('w'));
    if (!onoff) return bad('bad_args', 'entity has no on/off switch');
    return { time: a.time as string, days: days as Days, entity: e.id, attr: onoff.id, action: a.action };
  }

  // ---- helpers ----------------------------------------------------------------

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
    for (const c of this.clients) if (c.authed) this.sendTo(c.ws, m);
  }
}

function isHHMM(v: unknown): boolean {
  return typeof v === 'string' && /^([01]\d|2[0-3]):[0-5]\d$/.test(v);
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
