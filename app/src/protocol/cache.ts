// The last known state of the home, kept on the phone so the app has something to
// show while the hub is out of reach. Portable: the format only, no storage here
// (the file on a phone and localStorage in a browser live in ../statecache*.ts).
import type { Attr, Chip, Entity, HubInfo, Rule, Scene, SceneAction, Schedule, Timer, Value } from './messages';
import { initialState, type HomeState } from './store';

/** Bump when the format changes: an older cache is then thrown away, not misread. */
export const CACHE_VERSION = 1;

/** A cache bigger than this is not something we wrote: ignore it. */
export const MAX_CACHE_BYTES = 512 * 1024;

export interface CachedHome {
  v: number;
  /** When the state was true (epoch ms). */
  savedAt: number;
  /** Which hub it belongs to — a cache of another home is not this home. */
  url: string;
  hub: HubInfo | null;
  entities: Entity[];
  chips: Chip[];
  rules: Rule[];
  schedules: Schedule[];
  timers: Timer[];
  scenes: Scene[];
}

/** Put the cached state back into the app's shape: it is the last known one, not live. */
export function cachedState(c: CachedHome): HomeState {
  return {
    ...initialState,
    stale: true,
    savedAt: c.savedAt,
    hub: c.hub,
    entities: c.entities,
    chips: c.chips,
    rules: c.rules,
    schedules: c.schedules,
    timers: c.timers,
    scenes: c.scenes,
  };
}

/**
 * The cache as text. `now` is the moment the state was true; a state that is already
 * stale keeps its own moment, so a cache that is written again while the hub is away
 * does not pretend the data is fresher than it is.
 */
export function encodeCache(s: HomeState, url: string, now: number): string {
  const cached: CachedHome = {
    v: CACHE_VERSION,
    savedAt: s.stale ? (s.savedAt ?? now) : now,
    url,
    hub: s.hub,
    entities: s.entities,
    chips: s.chips,
    rules: s.rules,
    schedules: s.schedules,
    timers: s.timers,
    scenes: s.scenes,
  };
  return JSON.stringify(cached);
}

/**
 * Read a cache back. null when it is not ours, too old, or broken: a cache is a
 * convenience, never a reason to fail. Items that do not make sense are dropped —
 * the rest of the state is still worth showing.
 */
export function decodeCache(raw: string | null | undefined): CachedHome | null {
  if (!raw || raw.length > MAX_CACHE_BYTES) return null;
  let v: unknown;
  try {
    v = JSON.parse(raw);
  } catch {
    return null;
  }
  if (typeof v !== 'object' || v === null || Array.isArray(v)) return null;
  const c = v as Partial<CachedHome>;
  if (c.v !== CACHE_VERSION) return null;
  if (typeof c.url !== 'string' || c.url === '') return null;
  if (typeof c.savedAt !== 'number' || !Number.isFinite(c.savedAt)) return null;
  return {
    v: CACHE_VERSION,
    savedAt: c.savedAt,
    url: c.url,
    hub: hubOf(c.hub),
    entities: list(c.entities).map(entityOf).filter((e): e is Entity => e !== null),
    chips: list(c.chips).map(chipOf).filter((x): x is Chip => x !== null),
    rules: list(c.rules).map(ruleOf).filter((x): x is Rule => x !== null),
    schedules: list(c.schedules).map(scheduleOf).filter((x): x is Schedule => x !== null),
    timers: list(c.timers).map(timerOf).filter((x): x is Timer => x !== null),
    scenes: list(c.scenes).map(sceneOf).filter((x): x is Scene => x !== null),
  };
}

/** Two addresses for the same hub: the scheme and the host do not matter, the path does. */
export function sameHub(a: string, b: string): boolean {
  return norm(a) === norm(b);
}

function norm(url: string): string {
  return url.trim().toLowerCase().replace(/\/+$/, '');
}

function list(v: unknown): unknown[] {
  return Array.isArray(v) ? v : [];
}

function num(v: unknown): number | null {
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
}

function str(v: unknown): string | null {
  return typeof v === 'string' ? v : null;
}

/** The value on the wire: undefined means "not a value at all" (null is a value). */
function valueOf(v: unknown): Value | undefined {
  if (v === null || typeof v === 'boolean') return v;
  if (typeof v === 'number') return Number.isFinite(v) ? v : undefined;
  if (Array.isArray(v) && v.length === 2 && v.every((n) => typeof n === 'number')) return [v[0] as number, v[1] as number];
  return undefined;
}

function attrOf(v: unknown): Attr | null {
  if (typeof v !== 'object' || v === null) return null;
  const a = v as Partial<Attr>;
  const id = num(a.id);
  const name = str(a.name);
  const vt = str(a.vt);
  if (id === null || name === null || vt === null) return null;
  if (!Array.isArray(a.flags)) return null;
  const flags = a.flags.filter((f): f is Attr['flags'][number] => f === 'r' || f === 'w' || f === 'rep');
  const value = valueOf(a.value);
  if (value === undefined) return null;
  return { id, name, vt: vt as Attr['vt'], flags, value };
}

function entityOf(v: unknown): Entity | null {
  if (typeof v !== 'object' || v === null) return null;
  const e = v as Partial<Entity>;
  const id = num(e.id);
  const chip = num(e.chip);
  const device = str(e.device);
  if (id === null || chip === null || device === null) return null;
  const name = e.name === null || e.name === undefined ? null : str(e.name);
  if (name === null && e.name !== null && e.name !== undefined) return null;
  return { id, name, chip, device, attrs: list(e.attrs).map(attrOf).filter((a): a is Attr => a !== null) };
}

function chipOf(v: unknown): Chip | null {
  if (typeof v !== 'object' || v === null) return null;
  const c = v as Partial<Chip>;
  const id = num(c.id);
  const boot = num(c.boot_id);
  if (id === null || boot === null) return null;
  const status = c.status === 'ready' || c.status === 'silent' || c.status === 'seen' ? c.status : 'seen';
  const telemetry = typeof c.telemetry === 'object' && c.telemetry !== null ? (c.telemetry as Chip['telemetry']) : {};
  return {
    id,
    label: str(c.label) ?? `chip${id}`,
    status,
    boot_id: boot,
    reboots: num(c.reboots) ?? 0,
    telemetry,
  };
}

function ruleOf(v: unknown): Rule | null {
  if (typeof v !== 'object' || v === null) return null;
  const r = v as Partial<Rule>;
  const index = num(r.index);
  if (index === null || typeof r.when !== 'object' || r.when === null) return null;
  const when = r.when as Partial<Rule['when']>;
  const wEntity = num(when.entity);
  const wAttr = num(when.attr);
  if (wEntity === null || wAttr === null || typeof when.op !== 'string') return null;
  return r as Rule;
}

function scheduleOf(v: unknown): Schedule | null {
  if (typeof v !== 'object' || v === null) return null;
  const s = v as Partial<Schedule>;
  if (num(s.index) === null || typeof s.time !== 'string') return null;
  return s as Schedule;
}

function timerOf(v: unknown): Timer | null {
  if (typeof v !== 'object' || v === null) return null;
  const t = v as Partial<Timer>;
  if (num(t.index) === null || num(t.left_s) === null) return null;
  return t as Timer;
}

function sceneOf(v: unknown): Scene | null {
  if (typeof v !== 'object' || v === null) return null;
  const s = v as Partial<Scene>;
  if (typeof s.name !== 'string' || s.name === '') return null;
  const actions: SceneAction[] = [];
  for (const a of list(s.actions)) {
    if (typeof a !== 'object' || a === null) continue;
    const act = a as { entity?: unknown; attr?: unknown; value?: unknown };
    const entity = num(act.entity);
    const attr = num(act.attr);
    const value = valueOf(act.value);
    if (entity === null || attr === null || value === undefined || value === null) continue;
    actions.push({ entity, attr, value });
  }
  return { name: s.name, actions };
}

function hubOf(v: unknown): HubInfo | null {
  if (typeof v !== 'object' || v === null) return null;
  const h = v as Partial<HubInfo>;
  if (typeof h.name !== 'string' || typeof h.fw !== 'string') return null;
  return h as HubInfo;
}
