// What the client knows about the home, and the pure function that updates it from
// hub messages. Immutable: every change returns a new object (cheap structural
// sharing), so it plugs straight into React's useSyncExternalStore.
import type {
  Chip,
  Entity,
  HubInfo,
  Rule,
  Schedule,
  ServerMessage,
  Timer,
} from './messages';

export type ConnectionStatus =
  | 'idle' // not started
  | 'connecting'
  | 'authenticating' // socket open, waiting for welcome + snapshot
  | 'ready'
  | 'reconnecting' // dropped, will retry
  | 'unauthorized' // wrong token: fatal, no retry
  | 'incompatible' // protocol version mismatch: fatal
  | 'closed'; // close() was called

export interface HomeState {
  status: ConnectionStatus;
  /** Last error code worth showing (e.g. "busy"), cleared on `ready`. */
  error: string | null;
  hub: HubInfo | null;
  entities: Entity[];
  chips: Chip[];
  rules: Rule[];
  schedules: Schedule[];
  timers: Timer[];
}

export const initialState: HomeState = {
  status: 'idle',
  error: null,
  hub: null,
  entities: [],
  chips: [],
  rules: [],
  schedules: [],
  timers: [],
};

export function withStatus(s: HomeState, status: ConnectionStatus, error: string | null = null): HomeState {
  if (s.status === status && s.error === error) return s;
  return { ...s, status, error };
}

/** Forget what a previous connection knew (a new snapshot replaces it anyway). */
export function cleared(s: HomeState): HomeState {
  return { ...initialState, status: s.status, error: s.error };
}

/** Apply one hub message. Messages that carry no state (acks, errors) return `s`. */
export function reduce(s: HomeState, m: ServerMessage): HomeState {
  switch (m.type) {
    case 'welcome':
      return { ...s, hub: m.hub };
    case 'snapshot':
      return {
        ...s,
        status: 'ready',
        error: null,
        entities: m.entities,
        chips: m.chips,
        rules: m.rules,
        schedules: m.schedules,
        timers: m.timers,
      };
    case 'state': {
      let changed = false;
      const entities = s.entities.map((e) => {
        if (e.id !== m.entity) return e;
        const attrs = e.attrs.map((a) => {
          if (a.id !== m.attr || sameValue(a.value, m.value)) return a;
          changed = true;
          return { ...a, value: m.value };
        });
        return changed ? { ...e, attrs } : e;
      });
      return changed ? { ...s, entities } : s;
    }
    case 'entity':
      if (m.op === 'remove') {
        return s.entities.some((e) => e.id === m.id)
          ? { ...s, entities: s.entities.filter((e) => e.id !== m.id) }
          : s;
      } else {
        const i = s.entities.findIndex((e) => e.id === m.entity.id);
        const entities = s.entities.slice();
        if (i < 0) entities.push(m.entity);
        else entities[i] = m.entity;
        return { ...s, entities };
      }
    case 'chip': {
      const { type: _type, label, ...rest } = m;
      const i = s.chips.findIndex((c) => c.id === m.id);
      const chips = s.chips.slice();
      if (i < 0) chips.push({ ...rest, label: label ?? `chip${m.id}` });
      else chips[i] = { ...chips[i]!, ...rest, label: label ?? chips[i]!.label };
      return { ...s, chips };
    }
    case 'rules':
      return { ...s, rules: m.rules };
    case 'schedules':
      return { ...s, schedules: m.schedules };
    case 'timers':
      return { ...s, timers: m.timers };
    case 'time':
      return s.hub ? { ...s, hub: { ...s.hub, time: m.time, tz: m.tz, synced: m.synced } } : s;
    default:
      return s;
  }
}

function sameValue(a: unknown, b: unknown): boolean {
  if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((x, i) => x === b[i]);
  return a === b;
}

/** Convenience lookups for UI code. */
export function findEntity(s: HomeState, id: number): Entity | undefined {
  return s.entities.find((e) => e.id === id);
}
export function attrValue(s: HomeState, entity: number, attr: number): Entity['attrs'][number]['value'] | undefined {
  return findEntity(s, entity)?.attrs.find((a) => a.id === attr)?.value;
}
