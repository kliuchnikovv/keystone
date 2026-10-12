// Wire types of the hub <-> app protocol. Source of truth: ../../../docs/app-protocol.md.
// Pure types, no runtime dependencies, so the same file works in React Native, the
// browser and Node.

export const PROTOCOL_VERSION = 1;
export const MAX_FRAME_BYTES = 4096;

/** JSON-native attribute value; `null` = never reported. `xy` is `[x, y]`. */
export type Value = boolean | number | [number, number] | null;
export type ValueType = 'bool' | 'u8' | 'i16' | 'u16' | 'u32' | 'f32' | 'xy' | 'enum8';
export type AttrFlag = 'r' | 'w' | 'rep';

export interface Attr {
  id: number;
  name: string;
  vt: ValueType;
  flags: AttrFlag[];
  value: Value;
}

export interface Entity {
  id: number;
  /** User-set name, or null. */
  name: string | null;
  chip: number;
  device: string;
  attrs: Attr[];
}

export type ChipStatus = 'ready' | 'silent' | 'seen';

export interface ChipTelemetry {
  heap?: number;
  uptime_s?: number;
  tx_frames?: number;
  crc_errors?: number;
}

export interface Chip {
  id: number;
  label: string;
  status: ChipStatus;
  boot_id: number;
  reboots: number;
  telemetry: ChipTelemetry;
}

export interface HubInfo {
  name: string;
  fw: string;
  /** Unix seconds. */
  time: number;
  tz: string;
  synced: boolean;
  /** Where the hub is (degrees), for sunrise/sunset; null until set. */
  location?: { lat: number; lon: number } | null;
  /** The network the hub is on (never its password); null while it is not connected. */
  wifi?: { ssid: string; rssi: number; ip: string } | null;
  /** Today's sunrise and sunset, local "HH:MM"; null when unknown (no location, clock not synced, polar day/night). */
  sun?: { rise: string; set: string } | null;
}

export type CmpOp = 'any' | '==' | '!=' | '<' | '<=' | '>' | '>=';

export interface Rule {
  index: number;
  when: { entity: number; attr: number; op: CmpOp; value?: Value };
  then: { entity: number; attr: number; value: Value } | { scene: string };
  between?: { from: string; to: string };
  and?: { entity: number; attr: number; op: Exclude<CmpOp, 'any'>; value: Value };
  cooldown_s?: number;
}

export type Weekday = 'sun' | 'mon' | 'tue' | 'wed' | 'thu' | 'fri' | 'sat';
export type Days = 'daily' | 'weekdays' | 'weekends' | Weekday[];
export type Action = 'on' | 'off' | 'toggle';

/** One step of a scene: set this attribute to this value. */
export interface SceneAction {
  entity: number;
  attr: number;
  value: Value;
}

/** A named group of actions applied together (`scene.apply`). Scenes arrive with the snapshot. */
export interface Scene {
  name: string;
  actions: SceneAction[];
}

/** What a schedule or timer does: switch one device (`entity`, `attr`, `action`) or apply a scene (`scene`). */
export interface Schedule {
  index: number;
  time: string;
  /** `once` is a one-off: it runs on `date` (YYYY-MM-DD, the hub's local day) and then removes itself. */
  days: Days | 'once';
  date?: string;
  entity?: number;
  attr?: number;
  action?: Action;
  scene?: string;
}

export interface Timer {
  index: number;
  entity?: number;
  attr?: number;
  action?: Action;
  scene?: string;
  left_s: number;
}

// ---- client -> hub ---------------------------------------------------------

export interface Hello {
  type: 'hello';
  v: number;
  token: string;
  client?: string;
}
export interface Cmd {
  type: 'cmd';
  id: number;
  entity: number;
  attr: number;
  value: Value;
}
export interface Req {
  type: 'req';
  id: number;
  op: string;
  args?: Record<string, unknown>;
}
export type ClientMessage = Hello | Cmd | Req;

// ---- hub -> client ---------------------------------------------------------

export type ErrorCode =
  | 'unauthorized'
  | 'version'
  | 'busy'
  | 'bad_json'
  | 'too_large'
  | 'unsupported'
  | 'bad_args'
  | 'not_found'
  | 'read_only'
  | 'table_full'
  | 'internal';

export type AckStatus = 'ok' | 'fail' | 'unreachable' | 'unsupported' | 'timeout';

export interface Welcome {
  type: 'welcome';
  v: number;
  hub: HubInfo;
  /** The client got in with the hub's setup code: it gets no snapshot and may only scan and `setup.apply`. */
  setup?: boolean;
}
/**
 * The snapshot may be split into parts that each fit one frame. Part 0 resets the
 * client's lists, every part appends the items it carries, and the part with
 * `last: true` completes it. A single-frame snapshot is `part: 0, last: true`
 * (both fields may then be omitted). Sections absent from a part carry nothing.
 */
export interface Snapshot {
  type: 'snapshot';
  part?: number;
  last?: boolean;
  entities?: Entity[];
  chips?: Chip[];
  rules?: Rule[];
  schedules?: Schedule[];
  timers?: Timer[];
  scenes?: Scene[];
}
export interface StateEvent {
  type: 'state';
  entity: number;
  attr: number;
  value: Value;
}
/** `add` is an upsert: it also carries a renamed or re-declared entity. */
export type EntityEvent =
  | { type: 'entity'; op: 'add'; entity: Entity }
  | { type: 'entity'; op: 'remove'; id: number };
export interface ChipEvent extends Omit<Chip, 'label'> {
  type: 'chip';
  label?: string;
}
export interface RulesEvent {
  type: 'rules';
  rules: Rule[];
}
export interface SchedulesEvent {
  type: 'schedules';
  schedules: Schedule[];
}
export interface TimersEvent {
  type: 'timers';
  timers: Timer[];
}
/** The hub's own facts changed (location, today's sun): the whole object again. */
export interface WifiNetwork {
  ssid: string;
  rssi: number;
  secure: boolean;
}

export interface HubEvent {
  type: 'hub';
  hub: HubInfo;
}
export interface TimeEvent {
  type: 'time';
  time: number;
  tz: string;
  synced: boolean;
}
export interface Accepted {
  type: 'accepted';
  id: number;
  seq: number;
}
export interface Ack {
  type: 'ack';
  id: number;
  status: AckStatus;
}
export type Res =
  | { type: 'res'; id: number; ok: true; result?: Record<string, unknown> }
  | { type: 'res'; id: number; ok: false; code: ErrorCode; message?: string };
export interface ErrorMsg {
  type: 'error';
  id?: number;
  code: ErrorCode;
  message?: string;
}

export type ServerMessage =
  | Welcome
  | Snapshot
  | StateEvent
  | EntityEvent
  | ChipEvent
  | RulesEvent
  | SchedulesEvent
  | TimersEvent
  | TimeEvent
  | HubEvent
  | Accepted
  | Ack
  | Res
  | ErrorMsg;

/** Parse one text frame. Returns null for anything that is not a typed JSON object. */
export function parseServerMessage(text: string): ServerMessage | null {
  let v: unknown;
  try {
    v = JSON.parse(text);
  } catch {
    return null;
  }
  if (typeof v !== 'object' || v === null || Array.isArray(v)) return null;
  const type = (v as { type?: unknown }).type;
  if (typeof type !== 'string') return null;
  return v as ServerMessage;
}
