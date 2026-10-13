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

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/** A real calendar date written YYYY-MM-DD (2000..2099, as the hub accepts). */
export function validDate(s: string): boolean {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(s);
  if (!m) return false;
  const [y, mo, d] = [Number(m[1]), Number(m[2]), Number(m[3])];
  if (y < 2000 || y > 2099) return false;
  const t = new Date(Date.UTC(y, mo - 1, d));
  return t.getUTCFullYear() === y && t.getUTCMonth() === mo - 1 && t.getUTCDate() === d;
}

/** The phone's local date as YYYY-MM-DD (the hub's own day may differ by a time zone; it judges for real). */
export function localDate(now: Date = new Date()): string {
  const p = (n: number) => String(n).padStart(2, '0');
  return `${now.getFullYear()}-${p(now.getMonth() + 1)}-${p(now.getDate())}`;
}

/** "2026-10-20" → "20 Oct 2026". */
export function describeDate(s: string): string {
  if (!validDate(s)) return s;
  return `${Number(s.slice(8, 10))} ${MONTHS[Number(s.slice(5, 7)) - 1]} ${s.slice(0, 4)}`;
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

/** What the app knows about the hub that changes how times read: sun-relative ones need its location. */
export interface TimeContext {
  hasLocation: boolean;
}

export function describeSchedule(s: Schedule, entities: Entity[], ctx?: TimeContext): { title: string; detail: string } {
  const waiting = isSunPoint(s.time) && ctx !== undefined && !ctx.hasLocation;
  const when = s.days === 'once' ? `Once, ${s.date ? describeDate(s.date) : 'one day'}` : describeDays(s.days);
  return {
    title: `${describeTimePoint(s.time)} · ${doText(s, entities)}`,
    detail: waiting ? `${when} · waiting for the hub's location` : when,
  };
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
  if (r.between) extras.push(`between ${describeTimePoint(r.between.from)} and ${describeTimePoint(r.between.to)}`);
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
  setpoint: { unit: '°C', toWire: (n) => Math.round(n * 10), fromWire: (n) => n / 10 },
  humidity: { unit: '%', toWire: (n) => Math.round(n * 10), fromWire: (n) => n / 10 },
  illuminance: { unit: 'lx', toWire: (n) => n, fromWire: (n) => n },
  battery: { unit: '%', toWire: (n) => n, fromWire: (n) => n },
  color_temp: { unit: 'mireds', toWire: (n) => n, fromWire: (n) => n },
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

// ---- sun-relative times ("sunset+30") -----------------------------------------------------------

export const MAX_SUN_OFFSET_MIN = 180;
const SUN_RE = /^(sunrise|sunset)(?:([+-])(\d{1,3}))?$/;

/** A time the hub accepts: "HH:MM", or sunrise / sunset with an optional signed offset in minutes. */
export function validTimePoint(t: string): boolean {
  if (validTime(t)) return true;
  const m = SUN_RE.exec(t);
  return m !== null && (m[3] === undefined || Number(m[3]) <= MAX_SUN_OFFSET_MIN);
}

export function isSunPoint(t: string): boolean {
  return validTimePoint(t) && !validTime(t);
}

/** "sunset+30" → "30 min after sunset"; "22:00" stays "22:00". */
export function describeTimePoint(t: string): string {
  const m = SUN_RE.exec(t);
  if (!m) return t;
  const [, sun, sign, n] = m;
  if (n === undefined || Number(n) === 0) return `at ${sun}`;
  return `${Number(n)} min ${sign === '-' ? 'before' : 'after'} ${sun}`;
}

export type Built = Parsed<Record<string, unknown>>;

export interface ScheduleForm {
  time: string;
  days: Days;
  entity: number | null;
  action: Action;
  /** Set to apply a scene instead of switching `entity`. */
  scene?: string | null;
  /** YYYY-MM-DD: run once on that day instead of on `days`. */
  date?: string | null;
  /** The phone's local date (YYYY-MM-DD) to refuse a past `date` against; defaults to now. */
  today?: string;
}
export function scheduleRequest(f: ScheduleForm): Built {
  if (!validTimePoint(f.time)) return { ok: false, error: 'Time must look like 07:30, sunset or sunset+30' };
  let when: Record<string, unknown>;
  if (f.date !== undefined && f.date !== null) {
    if (!validDate(f.date)) return { ok: false, error: 'Date must look like 2026-10-20' };
    if (f.date < (f.today ?? localDate())) return { ok: false, error: 'That date has already passed' };
    when = { time: f.time, date: f.date };
  } else {
    if (Array.isArray(f.days) && f.days.length === 0) return { ok: false, error: 'Pick at least one day' };
    when = { time: f.time, days: f.days };
  }
  if (f.scene) return { ok: true, value: { ...when, scene: f.scene } };
  if (f.entity === null) return { ok: false, error: 'Pick a device' };
  return { ok: true, value: { ...when, entity: f.entity, action: f.action } };
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
    if (!validTimePoint(f.between.from) || !validTimePoint(f.between.to)) {
      return { ok: false, error: 'Between: times must look like 18:00, sunset or sunset+30' };
    }
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

// ---- the hub's location ------------------------------------------------------------------------

export function locationRequest(lat: string, lon: string): Built {
  const la = Number(lat.trim().replace(',', '.'));
  const lo = Number(lon.trim().replace(',', '.'));
  if (lat.trim() === '' || lon.trim() === '' || !Number.isFinite(la) || !Number.isFinite(lo)) {
    return { ok: false, error: 'Enter latitude and longitude as numbers, e.g. 50.08 and 14.43' };
  }
  if (la < -90 || la > 90) return { ok: false, error: 'Latitude is between -90 and 90 (north is positive)' };
  if (lo < -180 || lo > 180) return { ok: false, error: 'Longitude is between -180 and 180 (east is positive)' };
  return { ok: true, value: { lat: la, lon: lo } };
}

/** One line for the Hub screen. */
export function describeLocation(hub: { location?: { lat: number; lon: number } | null; sun?: { rise: string; set: string } | null; synced?: boolean } | null): string {
  if (!hub?.location) return 'Not set. Sunrise and sunset times wait until it is.';
  const where = `${hub.location.lat.toFixed(4)}, ${hub.location.lon.toFixed(4)}`;
  if (hub.sun) return `${where} · sunrise ${hub.sun.rise}, sunset ${hub.sun.set}`;
  return hub.synced === false ? `${where} · waiting for the clock` : `${where} · the sun does not rise and set today`;
}

// ---- Wi-Fi ----------------------------------------------------------------------------------------

const utf8Bytes = (s: string) => new TextEncoder().encode(s).length;
const hasControl = (s: string) => /[\u0000-\u001f\u007f]/.test(s);

/** The same rules the hub applies (core::wifi_credentials_valid), so a typo is caught before the hub drops off the network. */
export function wifiRequest(ssid: string, password: string): Built {
  const n = utf8Bytes(ssid);
  if (n < 1 || n > 32) return { ok: false, error: 'The network name is 1 to 32 characters' };
  if (hasControl(ssid)) return { ok: false, error: 'The network name has a character that is not allowed' };
  if (password !== '') {
    const hex64 = password.length === 64 && /^[0-9a-fA-F]+$/.test(password);
    if (!hex64 && (password.length < 8 || password.length > 63)) {
      return { ok: false, error: 'The password is 8 to 63 characters (leave it empty for an open network)' };
    }
    if (password.length === 64 && !hex64) return { ok: false, error: 'A 64-character password must be hex digits' };
    if (hasControl(password)) return { ok: false, error: 'The password has a character that is not allowed' };
  }
  return { ok: true, value: { ssid, password } };
}

/** "gigacube · −61 dBm · 192.168.0.202" */
export function describeWifi(hub: { wifi?: { ssid: string; rssi: number; ip: string } | null } | null): string {
  if (!hub?.wifi) return 'Not connected';
  return `${hub.wifi.ssid} · ${hub.wifi.rssi} dBm · ${hub.wifi.ip}`;
}

/** Signal strength in words, for the network list. */
export function signalText(rssi: number): string {
  return rssi >= -55 ? 'strong' : rssi >= -70 ? 'good' : rssi >= -82 ? 'weak' : 'very weak';
}

// ---- editing a scene -----------------------------------------------------------------------------

/** One setting of a scene being edited: the attribute and the value as it is typed (on/off, or a number in the user's units). */
export interface SceneDraft {
  option: AttrOption;
  value: string;
  /**
   * The exact value this setting had when it was loaded, and the text it was shown as. Brightness is typed in percent
   * (254 steps → 100), which cannot hold every step, so a setting the person did not touch keeps its exact value.
   */
  keep?: { text: string; wire: Value };
}

/** The inverse of parseInput: a wire value as the text a person would type. */
export function inputText(name: string, vt: ValueType, v: Value): string {
  if (vt === 'bool') return v === true ? 'on' : 'off';
  if (typeof v !== 'number') return '';
  const scale = SCALES[name];
  return String(scale ? scale.fromWire(v) : v);
}

/**
 * The settings of a scene as editable drafts. Settings whose device or attribute no longer exists (or can no longer
 * be written) come back in `gone`: the hub would refuse them, so saving the edit drops them.
 */
export function sceneDrafts(scene: Scene, entities: Entity[]): { drafts: SceneDraft[]; gone: SceneAction[] } {
  const writable = attrOptions(entities, { writable: true });
  const drafts: SceneDraft[] = [];
  const gone: SceneAction[] = [];
  for (const a of scene.actions) {
    const option = writable.find((o) => o.entity === a.entity && o.attr === a.attr);
    if (option) {
      const text = inputText(option.name, option.vt, a.value);
      drafts.push({ option, value: text, keep: { text, wire: a.value } });
    }
    else gone.push(a);
  }
  return { drafts, gone };
}

/** The writable settings that could still be added to a scene. */
export function addableSettings(entities: Entity[], drafts: SceneDraft[]): AttrOption[] {
  return attrOptions(entities, { writable: true }).filter((o) => !drafts.some((d) => d.option.entity === o.entity && d.option.attr === o.attr));
}

/** Every setting set to what its device is doing right now (a device that is gone, or whose value is unknown, is left as it was). */
export function draftsFromCurrentState(drafts: SceneDraft[], entities: Entity[]): SceneDraft[] {
  return drafts.map((d) => {
    const a = entities.find((e) => e.id === d.option.entity)?.attrs.find((x) => x.id === d.option.attr);
    if (!a || a.value === null || a.value === undefined) return d;
    const text = inputText(d.option.name, d.option.vt, a.value);
    return { option: d.option, value: text, keep: { text, wire: a.value } };
  });
}

/** `scene.put` arguments for the edited scene: every value parsed and range-checked, an attribute at most once. */
export function draftsRequest(name: string, drafts: SceneDraft[]): Built {
  const actions: SceneAction[] = [];
  for (const d of drafts) {
    if (d.keep && d.value === d.keep.text) {
      actions.push({ entity: d.option.entity, attr: d.option.attr, value: d.keep.wire });
      continue;
    }
    const v = parseInput(d.option.name, d.option.vt, d.value);
    if (!v.ok) return { ok: false, error: `${d.option.label}: ${v.error}` };
    actions.push({ entity: d.option.entity, attr: d.option.attr, value: v.value });
  }
  return sceneRequest(name, actions);
}
