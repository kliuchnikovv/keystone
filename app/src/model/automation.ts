// Rules, schedules and timers as people read and write them: describing the hub's
// objects in words, and building `req` arguments from what a form collected. Pure
// functions, so the awkward parts (units, ranges, wording) are covered by tests.
import type {
  Action,
  Attr,
  CmpOp,
  Days,
  Entity,
  Rule,
  Scene,
  SceneAction,
  Schedule,
  Timer,
  Value,
  ValueType,
  Weekday,
} from '../protocol/messages';
import { attrLabel, formatValue } from './format';
import { deviceTitle, primarySwitch } from './devices';

// ---- wording -----------------------------------------------------------------------

export const COMPARISONS: { op: CmpOp; label: string }[] = [
  { op: 'any', label: 'changes' },
  { op: '==', label: 'is' },
  { op: '!=', label: 'is not' },
  { op: '>', label: 'is above' },
  { op: '>=', label: 'is at least' },
  { op: '<', label: 'is below' },
  { op: '<=', label: 'is at most' },
];

export function opText(op: CmpOp): string {
  return COMPARISONS.find((c) => c.op === op)?.label ?? op;
}

const DAY_NAMES: Record<Weekday, string> = { sun: 'Sun', mon: 'Mon', tue: 'Tue', wed: 'Wed', thu: 'Thu', fri: 'Fri', sat: 'Sat' };
export const WEEK: Weekday[] = ['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'];

export function describeDays(d: Days): string {
  if (d === 'daily') return 'Every day';
  if (d === 'weekdays') return 'Weekdays';
  if (d === 'weekends') return 'Weekends';
  return WEEK.filter((w) => d.includes(w)).map((w) => DAY_NAMES[w]).join(', ');
}

export function formatCountdown(seconds: number): string {
  if (seconds <= 0) return 'now';
  if (seconds < 90) return `${seconds} s`;
  const m = Math.round(seconds / 60);
  if (m < 90) return `${m} min`;
  const h = Math.floor(m / 60);
  const rest = m % 60;
  return rest === 0 ? `${h} h` : `${h} h ${rest} min`;
}

function entityName(entities: Entity[], id: number): string {
  const e = entities.find((x) => x.id === id);
  return e ? deviceTitle(e) : `device ${id}`;
}
function attrOf(entities: Entity[], entity: number, attr: number): Attr | undefined {
  return entities.find((e) => e.id === entity)?.attrs.find((a) => a.id === attr);
}
/** An attribute's value as a person reads it, given only the ids (the entity may be gone). */
function valueFor(entities: Entity[], entity: number, attr: number, v: Value): string {
  const a = attrOf(entities, entity, attr);
  return a ? formatValue({ name: a.name, value: v }) : String(v);
}
function attrName(entities: Entity[], entity: number, attr: number): string {
  const a = attrOf(entities, entity, attr);
  return a ? attrLabel(a.name).toLowerCase() : `attribute ${attr}`;
}

export function actionText(a: Action): string {
  return a === 'on' ? 'On' : a === 'off' ? 'Off' : 'Toggle';
}

/** What a schedule or timer does, in words: "Off plug1" or "Scene evening". */
function doText(x: { entity?: number; action?: Action; scene?: string }, entities: Entity[]): string {
  if (x.scene !== undefined) return `Scene ${x.scene}`;
  return `${actionText(x.action ?? 'toggle')} ${entityName(entities, x.entity ?? 0)}`;
}

export function describeSchedule(s: Schedule, entities: Entity[]): { title: string; detail: string } {
  return { title: `${s.time} · ${doText(s, entities)}`, detail: describeDays(s.days) };
}

export function describeTimer(t: Timer, entities: Entity[], leftSeconds: number): { title: string; detail: string } {
  return { title: doText(t, entities), detail: `in ${formatCountdown(leftSeconds)}` };
}

/** "plug1 off · lamp brightness 50 %": what applying the scene does. */
export function describeScene(sc: Scene, entities: Entity[]): { title: string; detail: string } {
  const parts = sc.actions.map((a) => {
    const target = attrOf(entities, a.entity, a.attr);
    const who = entityName(entities, a.entity);
    return target?.name === 'on_off' ? `${who} ${a.value ? 'on' : 'off'}` : `${who} ${attrName(entities, a.entity, a.attr)} ${valueFor(entities, a.entity, a.attr, a.value)}`;
  });
  return { title: sc.name, detail: parts.join(' · ') };
}

export function describeRule(r: Rule, entities: Entity[]): { when: string; then: string; extras: string[] } {
  const who = entityName(entities, r.when.entity);
  const what = attrName(entities, r.when.entity, r.when.attr);
  const when =
    r.when.op === 'any'
      ? `When ${who} ${what} changes`
      : `When ${who} ${what} ${opText(r.when.op)} ${valueFor(entities, r.when.entity, r.when.attr, r.when.value ?? null)}`;
  let then: string;
  if ('scene' in r.then) {
    then = `apply scene ${r.then.scene}`;
  } else {
    const target = attrOf(entities, r.then.entity, r.then.attr);
    const to = valueFor(entities, r.then.entity, r.then.attr, r.then.value);
    then =
      target?.name === 'on_off'
        ? `turn ${entityName(entities, r.then.entity)} ${r.then.value ? 'on' : 'off'}`
        : `set ${entityName(entities, r.then.entity)} ${attrName(entities, r.then.entity, r.then.attr)} to ${to}`;
  }
  const extras: string[] = [];
  if (r.between) extras.push(`between ${r.between.from} and ${r.between.to}`);
  if (r.and) {
    extras.push(
      `only while ${entityName(entities, r.and.entity)} ${attrName(entities, r.and.entity, r.and.attr)} ${opText(r.and.op)} ${valueFor(entities, r.and.entity, r.and.attr, r.and.value)}`,
    );
  }
  if (r.cooldown_s) extras.push(`at most once per ${formatCountdown(r.cooldown_s)}`);
  return { when, then, extras };
}

// ---- what can be picked ------------------------------------------------------------

export interface AttrOption {
  entity: number;
  attr: number;
  /** "Lamp · Brightness" */
  label: string;
  name: string;
  vt: ValueType;
  writable: boolean;
}

/** Attributes of devices a rule can look at or set. xy colour has no form input, so it is left out. */
export function attrOptions(entities: Entity[], opts: { writable?: boolean } = {}): AttrOption[] {
  const out: AttrOption[] = [];
  for (const e of entities) {
    for (const a of e.attrs) {
      const writable = a.flags.includes('w');
      if (a.vt === 'xy' || (opts.writable && !writable)) continue;
      out.push({ entity: e.id, attr: a.id, label: `${deviceTitle(e)} · ${attrLabel(a.name)}`, name: a.name, vt: a.vt, writable });
    }
  }
  return out.sort((a, b) => a.label.localeCompare(b.label));
}

/** Devices with a writable on/off: what schedules and timers can drive. */
export function switchTargets(entities: Entity[]): Entity[] {
  return entities.filter((e) => primarySwitch(e) !== undefined);
}

// ---- values typed into forms ---------------------------------------------------------

/** What the user types vs what goes on the wire: brightness in percent, temperature in °C. */
const SCALES: Record<string, { unit: string; toWire: (n: number) => number; fromWire: (n: number) => number }> = {
  brightness: { unit: '%', toWire: (n) => Math.round((n / 100) * 254), fromWire: (n) => Math.round((n / 254) * 100) },
  temperature: { unit: '°C', toWire: (n) => Math.round(n * 10), fromWire: (n) => n / 10 },
  power_w: { unit: 'W', toWire: (n) => n, fromWire: (n) => n },
  energy_wh: { unit: 'Wh', toWire: (n) => n, fromWire: (n) => n },
};

export function inputUnit(name: string): string {
  return SCALES[name]?.unit ?? '';
}

const RANGES: Partial<Record<ValueType, [number, number]>> = {
  u8: [0, 255],
  enum8: [0, 255],
  i16: [-32768, 32767],
  u16: [0, 65535],
  u32: [0, 0xffffffff],
};

export type Parsed<T> = { ok: true; value: T } | { ok: false; error: string };

/** Text typed for an attribute (or "on"/"off" for a bool) to its wire value, with range checks. */
export function parseInput(name: string, vt: ValueType, text: string): Parsed<Value> {
  const t = text.trim().toLowerCase();
  if (vt === 'bool') {
    if (t === 'on' || t === 'true') return { ok: true, value: true };
    if (t === 'off' || t === 'false') return { ok: true, value: false };
    return { ok: false, error: 'Choose on or off' };
  }
  if (t === '' || !/^-?\d+(\.\d+)?$/.test(t)) return { ok: false, error: 'Enter a number' };
  const n = Number(t);
  const scale = SCALES[name];
  const wire = scale ? scale.toWire(n) : n;
  if (vt === 'f32') return { ok: true, value: wire };
  if (!Number.isInteger(wire)) return { ok: false, error: 'Enter a whole number' };
  const range = RANGES[vt];
  if (range && (wire < range[0] || wire > range[1])) {
    return { ok: false, error: scale ? `Out of range for this device (${scale.fromWire(range[0])}…${scale.fromWire(range[1])} ${scale.unit})` : `Out of range (${range[0]}…${range[1]})` };
  }
  return { ok: true, value: wire };
}

// ---- forms -> request arguments -------------------------------------------------------

export function validTime(t: string): boolean {
  return /^([01]\d|2[0-3]):[0-5]\d$/.test(t);
}

export type Built = Parsed<Record<string, unknown>>;

export interface ScheduleForm {
  time: string;
  days: Days;
  entity: number | null;
  action: Action;
  /** Set to apply a scene instead of switching `entity`. */
  scene?: string | null;
}
export function scheduleRequest(f: ScheduleForm): Built {
  if (!validTime(f.time)) return { ok: false, error: 'Time must look like 07:30' };
  if (Array.isArray(f.days) && f.days.length === 0) return { ok: false, error: 'Pick at least one day' };
  if (f.scene) return { ok: true, value: { time: f.time, days: f.days, scene: f.scene } };
  if (f.entity === null) return { ok: false, error: 'Pick a device' };
  return { ok: true, value: { time: f.time, days: f.days, entity: f.entity, action: f.action } };
}

export interface TimerForm {
  entity: number | null;
  action: Action;
  minutes: string;
  scene?: string | null;
}
export function timerRequest(f: TimerForm): Built {
  if (!f.scene && f.entity === null) return { ok: false, error: 'Pick a device' };
  const m = Number(f.minutes);
  if (!/^\d+(\.\d+)?$/.test(f.minutes.trim()) || m <= 0) return { ok: false, error: 'Enter how many minutes' };
  const seconds = Math.round(m * 60);
  if (seconds > 7 * 24 * 3600) return { ok: false, error: 'At most 7 days' };
  if (f.scene) return { ok: true, value: { scene: f.scene, delay_s: seconds } };
  return { ok: true, value: { entity: f.entity, action: f.action, delay_s: seconds } };
}

export interface ConditionForm {
  option: AttrOption | null;
  op: CmpOp;
  value: string;
}
export interface RuleForm {
  when: ConditionForm;
  then: { option: AttrOption | null; value: string; scene?: string | null };
  between: { from: string; to: string } | null;
  and: ConditionForm | null;
  cooldown: string;
}

function condition(label: string, c: ConditionForm, allowAny: boolean): Parsed<Record<string, unknown>> {
  if (!c.option) return { ok: false, error: `${label}: pick what to look at` };
  if (c.op === 'any') {
    return allowAny ? { ok: true, value: { entity: c.option.entity, attr: c.option.attr, op: 'any' } } : { ok: false, error: `${label}: pick a comparison` };
  }
  const v = parseInput(c.option.name, c.option.vt, c.value);
  if (!v.ok) return { ok: false, error: `${label}: ${v.error}` };
  return { ok: true, value: { entity: c.option.entity, attr: c.option.attr, op: c.op, value: v.value } };
}

export function ruleRequest(f: RuleForm): Built {
  const when = condition('When', f.when, true);
  if (!when.ok) return when;
  let thenArgs: Record<string, unknown>;
  if (f.then.scene) {
    thenArgs = { scene: f.then.scene };
  } else {
    if (!f.then.option) return { ok: false, error: 'Then: pick what to set' };
    const then = parseInput(f.then.option.name, f.then.option.vt, f.then.value);
    if (!then.ok) return { ok: false, error: `Then: ${then.error}` };
    thenArgs = { entity: f.then.option.entity, attr: f.then.option.attr, value: then.value };
  }
  const args: Record<string, unknown> = { when: when.value, then: thenArgs };
  if (f.between) {
    if (!validTime(f.between.from) || !validTime(f.between.to)) return { ok: false, error: 'Between: times must look like 18:00' };
    if (f.between.from === f.between.to) return { ok: false, error: 'Between: start and end must differ' };
    args.between = { from: f.between.from, to: f.between.to };
  }
  if (f.and) {
    const and = condition('Only while', f.and, false);
    if (!and.ok) return and;
    args.and = and.value;
  }
  const cd = f.cooldown.trim();
  if (cd !== '') {
    const s = Number(cd) * 60;
    if (!/^\d+(\.\d+)?$/.test(cd) || !Number.isInteger(Math.round(s)) || Math.round(s) < 1 || Math.round(s) > 65535) {
      return { ok: false, error: 'At most once per: 1 second to 18 hours' };
    }
    args.cooldown_s = Math.round(s);
  }
  return { ok: true, value: args };
}

/** A device name the hub will accept (see the protocol doc, names). */
export function nameProblem(name: string): string | null {
  if (!/^[a-z][a-z0-9_]{0,22}$/.test(name)) return 'Use a-z, 0-9 and _, start with a letter, at most 23 characters';
  return null;
}

// ---- scenes ----------------------------------------------------------------------------

export const MAX_SCENE_ACTIONS = 8;

/** The writable settings of these devices as they are right now: what "save this as a scene" stores. */
export function captureScene(entities: Entity[], ids: number[]): SceneAction[] {
  const out: SceneAction[] = [];
  for (const id of ids) {
    const e = entities.find((x) => x.id === id);
    if (!e) continue;
    for (const a of e.attrs) {
      if (!a.flags.includes('w') || a.vt === 'xy' || a.value === null || a.value === undefined) continue;
      out.push({ entity: e.id, attr: a.id, value: a.value });
    }
  }
  return out;
}

export function sceneRequest(name: string, actions: SceneAction[]): Built {
  const problem = nameProblem(name);
  if (problem) return { ok: false, error: problem };
  if (actions.length === 0) return { ok: false, error: 'Choose at least one device that has something to save' };
  if (actions.length > MAX_SCENE_ACTIONS) {
    return { ok: false, error: `A scene holds at most ${MAX_SCENE_ACTIONS} settings; choose fewer devices` };
  }
  return { ok: true, value: { name, actions } };
}
