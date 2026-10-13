// The hub's event history: parse a `history` page, merge pages into one list, and say each event in words.
// Pure functions; the hook (useHistory) does the fetching.
import type { Entity, HistoryEvent, HistoryPage, Value } from '../protocol/messages';
import type { HomeState } from '../protocol/store';
import { deviceIcon, deviceTitle, hexId } from './devices';
import { attrLabel, formatUptime, formatValue } from './format';

/** The most the app keeps in memory: more than the hub's ring holds (256), so "load older" never grows without bound. */
export const MAX_KEPT = 300;

// ---- parsing ---------------------------------------------------------------------------

const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);

function parseValue(v: unknown): Value | undefined {
  if (typeof v === 'boolean' || isNum(v)) return v;
  if (Array.isArray(v) && v.length === 2 && isNum(v[0]) && isNum(v[1])) return [v[0], v[1]];
  return undefined;
}

/** Validate a `history` result. Anything that does not look like a page is null; a malformed event is dropped, not fatal. */
export function parseHistoryPage(result: Record<string, unknown> | null): HistoryPage | null {
  if (!result || !isNum(result.now) || !isNum(result.latest) || !isNum(result.oldest) || !Array.isArray(result.events)) return null;
  const events: HistoryEvent[] = [];
  for (const raw of result.events) {
    if (typeof raw !== 'object' || raw === null) continue;
    const e = raw as Record<string, unknown>;
    if (!isNum(e.seq) || !isNum(e.age) || typeof e.kind !== 'string') continue;
    const ev: HistoryEvent = { seq: e.seq, age: e.age, kind: e.kind };
    if (typeof e.what === 'string') ev.what = e.what;
    if (isNum(e.entity)) ev.entity = e.entity;
    if (isNum(e.attr)) ev.attr = e.attr;
    if (isNum(e.ref)) ev.ref = e.ref;
    const value = parseValue(e.value);
    if (value !== undefined) ev.value = value;
    if (typeof e.scene === 'string') ev.scene = e.scene;
    events.push(ev);
  }
  return { now: result.now, latest: result.latest, oldest: result.oldest, more: result.more === true, events };
}

// ---- the list ----------------------------------------------------------------------------

/** An event placed in time: `time` is Unix seconds when the hub's clock was synced, else null (then `age` is all there is). */
export interface Recorded extends HistoryEvent {
  time: number | null;
  /** Client clock (ms) when the page it came from arrived: `age` was true then. */
  fetchedAt: number;
}

export interface HistoryList {
  /** Newest first. */
  events: Recorded[];
  /** The hub's newest sequence number as of the last fetch (0 = none yet). */
  latest: number;
  /** True while the hub still holds events older than the oldest we have. */
  hasMore: boolean;
}

export const emptyHistory: HistoryList = { events: [], latest: 0, hasMore: false };

function place(page: HistoryPage, nowMs: number): Recorded[] {
  return page.events.map((e) => ({ ...e, time: page.now > 0 ? page.now - e.age : null, fetchedAt: nowMs }));
}

const lastSeq = (events: { seq: number }[]): number => events[events.length - 1]?.seq ?? 0;

/**
 * Fold the newest page into the list. Three cases: the hub restarted (its sequence numbers began again, so
 * `latest` went down) or too much happened between two fetches to join the page to what we have: start over
 * from the page, older events come back with "load older"; otherwise just put the new ones on top.
 */
export function mergeNewest(list: HistoryList, page: HistoryPage, nowMs: number): HistoryList {
  const fresh = place(page, nowMs);
  const oldestInPage = lastSeq(fresh);
  const restarted = page.latest < list.latest;
  const gap = list.events.length > 0 && fresh.length > 0 && oldestInPage > list.latest + 1;
  if (list.events.length === 0 || restarted || gap) {
    return { events: fresh.slice(0, MAX_KEPT), latest: page.latest, hasMore: page.more || fresh.length > MAX_KEPT };
  }
  const added = fresh.filter((e) => e.seq > list.latest);
  if (added.length === 0) return { ...list, latest: page.latest };
  const all = [...added, ...list.events];
  // Older events were not fetched, or fell off the end of what we keep: either way there are more to ask for.
  return { events: all.slice(0, MAX_KEPT), latest: page.latest, hasMore: list.hasMore || all.length > MAX_KEPT };
}

/**
 * Fold a live push (`history` message: new events, newest first) onto the list. Null when it does not join what
 * we have — nothing loaded yet, the hub restarted, or events in between were missed — and the caller should
 * fetch the newest page instead.
 */
export function mergePush(list: HistoryList, page: HistoryPage, nowMs: number): HistoryList | null {
  if (list.events.length === 0 || page.latest < list.latest) return null;
  const fresh = place(page, nowMs).filter((e) => e.seq > list.latest);
  if (fresh.length === 0) return { ...list, latest: Math.max(list.latest, page.latest) };
  if (lastSeq(fresh) > list.latest + 1) return null;
  const all = [...fresh, ...list.events];
  return { events: all.slice(0, MAX_KEPT), latest: fresh[0]!.seq, hasMore: list.hasMore || all.length > MAX_KEPT };
}

/** Fold a page of older events (asked with `before = oldest we have`) onto the end of the list. */
export function mergeOlder(list: HistoryList, page: HistoryPage, nowMs: number): HistoryList {
  const have = lastSeq(list.events);
  const older = place(page, nowMs).filter((e) => have === 0 || e.seq < have);
  const events = [...list.events, ...older].slice(0, MAX_KEPT);
  return { events, latest: Math.max(list.latest, page.latest), hasMore: page.more && events.length < MAX_KEPT };
}

// ---- time --------------------------------------------------------------------------------

/** Seconds ago, as of `nowMs` on this device. */
export function secondsAgo(e: Recorded, nowMs: number): number {
  return Math.max(0, Math.round(e.age + (nowMs - e.fetchedAt) / 1000));
}

export function formatAgo(seconds: number): string {
  if (seconds < 5) return 'just now';
  if (seconds < 60) return `${seconds} s ago`;
  return `${formatUptime(seconds)} ago`;
}

const pad = (n: number) => String(n).padStart(2, '0');

/** "14:05:09" in the phone's time zone; without a synced hub clock, how long ago. */
export function formatEventTime(e: Recorded, nowMs: number): string {
  if (e.time === null) return formatAgo(secondsAgo(e, nowMs));
  const d = new Date(e.time * 1000);
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

/** The day heading an event falls under ("Today", "Yesterday", "Mon 12 Oct"); null when the clock was not synced. */
export function dayOf(e: Recorded, nowMs: number): string | null {
  if (e.time === null) return null;
  const d = new Date(e.time * 1000);
  const today = new Date(nowMs);
  const days = Math.round((Date.UTC(today.getFullYear(), today.getMonth(), today.getDate()) - Date.UTC(d.getFullYear(), d.getMonth(), d.getDate())) / 86_400_000);
  if (days === 0) return 'Today';
  if (days === 1) return 'Yesterday';
  return d.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' });
}

// ---- words -------------------------------------------------------------------------------

/** Names of attributes by id, for events about a device that has since gone (the entity is no longer in the list). */
const ATTR_BY_ID: Record<number, string> = {
  0x01: 'on_off', 0x02: 'brightness', 0x03: 'color_xy', 0x04: 'color_temp', 0x10: 'temperature', 0x11: 'humidity',
  0x12: 'illuminance', 0x13: 'power_w', 0x14: 'energy_wh', 0x1f: 'battery', 0x40: 'setpoint', 0x41: 'mode',
  0x50: 'occupied', 0x51: 'contact', 0x60: 'event',
};

export interface Described {
  icon: string;
  text: string;
  /** How much it matters: failures are `bad`, hub/chip trouble `warn`, the rest `normal`. */
  tone: 'normal' | 'warn' | 'bad';
}

type Home = Pick<HomeState, 'entities' | 'chips'>;

export function entityName(entity: number, entities: Entity[]): string {
  const e = entities.find((x) => x.id === entity);
  return e ? deviceTitle(e) : `Device ${hexId(entity)}`;
}

function attrName(entity: number, attr: number, entities: Entity[]): string {
  return entities.find((x) => x.id === entity)?.attrs.find((a) => a.id === attr)?.name ?? ATTR_BY_ID[attr] ?? `attr ${attr}`;
}

/** "On", "21.5 °C", "Pressed" … */
function valueWords(name: string, value: Value | undefined): string {
  return value === undefined ? '' : formatValue({ name, value });
}

/** "turned on" / "opened" / "is 21.5 °C": what a change of one attribute means. */
function changeWords(name: string, value: Value | undefined): string {
  if (typeof value === 'boolean') {
    if (name === 'on_off') return value ? 'turned on' : 'turned off';
    if (name === 'occupied') return value ? 'detected someone' : 'sees no one';
    if (name === 'contact') return value ? 'closed' : 'opened';
  }
  if (name === 'event') return `button: ${valueWords(name, value).toLowerCase()}`;
  return `${attrLabel(name).toLowerCase()}: ${valueWords(name, value)}`;
}

const VIA: Record<string, string> = { console: 'from the console', app: 'from the app', rule: 'by a rule', schedule: 'by a schedule', timer: 'by a timer' };

/** Who asked for a command (the event's `what`): the History row ends with it. */
const COMMAND_VIA: Record<string, string> = {
  console: 'from the console',
  app: 'from the app',
  rule: 'by a rule',
  schedule: 'by a schedule',
  timer: 'by a timer',
  scene: 'by a scene',
  matter: 'from a Matter controller',
  reconcile: 'to restore its last setting',
};

function sceneWords(e: HistoryEvent): string {
  return e.scene ? `scene \u201c${e.scene}\u201d` : 'a scene that has since been deleted';
}

function chipLabel(id: number | undefined, chips: Home['chips']): string {
  const c = chips.find((x) => x.id === id);
  return c ? `${c.label} chip` : `chip ${id ?? '?'}`;
}

/** What an action did: "Kitchen light on", or the scene it applied. */
function actionWords(e: HistoryEvent, h: Home): string {
  if (e.entity === undefined || e.attr === undefined) return sceneWords(e);
  const name = attrName(e.entity, e.attr, h.entities);
  return `${entityName(e.entity, h.entities)}: ${changeWords(name, e.value)}`;
}

export function describeEvent(e: HistoryEvent, h: Home): Described {
  const device = e.entity !== undefined ? entityName(e.entity, h.entities) : '';
  const icon = e.entity !== undefined ? deviceIcon(h.entities.find((x) => x.id === e.entity)?.device ?? '') : '';
  const name = e.entity !== undefined && e.attr !== undefined ? attrName(e.entity, e.attr, h.entities) : '';
  switch (e.kind) {
    case 'boot':
      return { icon: '🔌', tone: e.what === 'panic' || e.what === 'watchdog' || e.what === 'brownout' ? 'bad' : 'normal', text: `Hub started${e.what && e.what !== 'unknown' ? ` (${e.what.replace('_', ' ')})` : ''}` };
    case 'state':
      return { icon, tone: 'normal', text: `${device} ${changeWords(name, e.value)}` };
    case 'command': {
      const why = e.what ? COMMAND_VIA[e.what] : undefined; // an unknown source (newer hub) says nothing
      return { icon: '➡️', tone: 'normal', text: `Sent to ${device}: ${changeWords(name, e.value)}${why ? ` ${why}` : ''}` };
    }
    case 'command_failed':
      return { icon: '⚠️', tone: 'bad', text: `${device}: ${attrLabel(name).toLowerCase()} command failed (${e.what ?? 'unknown'})` };
    case 'rule':
      return { icon: '⚙️', tone: 'normal', text: `Rule ${(e.ref ?? 0) + 1} ran: ${actionWords(e, h)}` };
    case 'schedule':
      return { icon: '🕒', tone: 'normal', text: `Schedule ${(e.ref ?? 0) + 1} ran: ${actionWords(e, h)}` };
    case 'timer':
      return { icon: '⏱️', tone: 'normal', text: `Timer ${(e.ref ?? 0) + 1} ran: ${actionWords(e, h)}` };
    case 'scene':
      return { icon: '🎬', tone: 'normal', text: `Applied ${sceneWords(e)} ${VIA[e.what ?? ''] ?? ''}`.trim() };
    case 'chip': {
      const label = chipLabel(e.ref, h.chips);
      const n = typeof e.value === 'number' ? e.value : 0;
      switch (e.what) {
        case 'lost':
          return { icon: '📡', tone: 'warn', text: `The ${label} went silent (${formatUptime(n)} without a sign)` };
        case 'resumed':
          return { icon: '📡', tone: 'normal', text: `The ${label} is back after ${formatUptime(n)}` };
        case 'rebooted':
          return { icon: '📡', tone: 'warn', text: `The ${label} restarted (restart ${n})` };
        case 'flapping':
          return { icon: '📡', tone: 'bad', text: `The ${label} keeps restarting (${n} times in 5 minutes)` };
      }
      return { icon: '📡', tone: 'warn', text: `The ${label} changed: ${e.what ?? '?'}` };
    }
    case 'reconcile':
      return e.what === 'corrected'
        ? { icon: '🔁', tone: 'warn', text: `${device} had ${changeWords(name, e.value)} behind the hub’s back; putting it back` }
        : { icon: '🔁', tone: 'normal', text: `${device} ${changeWords(name, e.value)} outside the hub` };
    case 'pairing':
      if (e.what === 'opened') return { icon: '➕', tone: 'normal', text: `Network opened for new devices (${e.ref ?? '?'} s)` };
      if (e.what === 'unpair') return { icon: '➖', tone: 'normal', text: `Removing ${device} from the network` };
      return { icon: '💡', tone: 'normal', text: `Blinking ${device}` };
  }
  return { icon: '•', tone: 'normal', text: `${e.kind}${e.what ? ` ${e.what}` : ''}` };
}
