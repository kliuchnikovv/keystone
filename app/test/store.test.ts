import { describe, expect, it } from 'vitest';
import type { Entity, ServerMessage } from '../src/protocol/messages';
import { attrValue, findEntity, initialState, markedStale, reduce, withStatus } from '../src/protocol/store';
import { defaultChips, defaultEntities } from '../tools/mock-hub/hub';

const snapshot = (): ServerMessage => ({
  type: 'snapshot',
  entities: defaultEntities(),
  chips: defaultChips(),
  rules: [],
  schedules: [],
  timers: [],
});

describe('reduce', () => {
  it('a snapshot fills the state and marks it ready', () => {
    const s = reduce(withStatus(initialState, 'authenticating', 'busy'), snapshot());
    expect(s.status).toBe('ready');
    expect(s.error).toBeNull();
    expect(s.entities).toHaveLength(3);
    expect(s.chips.map((c) => c.label)).toEqual(['zigbee', 'thread']);
  });

  it('a split snapshot is only ready after the last part, and part 0 starts over', () => {
    const ents = defaultEntities();
    let s = withStatus(initialState, 'authenticating');
    s = reduce(s, { type: 'snapshot', part: 0, last: false, entities: ents.slice(0, 2) });
    expect(s.status).toBe('authenticating');
    expect(s.entities).toHaveLength(2);
    s = reduce(s, { type: 'snapshot', part: 1, last: true, entities: ents.slice(2), chips: defaultChips() });
    expect(s.status).toBe('ready');
    expect(s.entities.map((e) => e.id)).toEqual(ents.map((e) => e.id));
    expect(s.chips).toHaveLength(2);
    // A resync (part 0 again) replaces, it does not duplicate.
    s = reduce(s, { type: 'snapshot', part: 0, last: true, entities: ents.slice(0, 1), chips: [] });
    expect(s.entities).toHaveLength(1);
    expect(s.chips).toEqual([]);
    expect(s.status).toBe('ready');
  });

  it('a state event changes one attribute and keeps untouched objects identical', () => {
    const a = reduce(initialState, snapshot());
    const b = reduce(a, { type: 'state', entity: 0x0100, attr: 0x13, value: 42 });
    expect(attrValue(b, 0x0100, 0x13)).toBe(42);
    expect(findEntity(b, 0x0101)).toBe(findEntity(a, 0x0101)); // other entity: same object
    expect(b).not.toBe(a);
  });

  it('a state event with the same value, or for an unknown target, returns the same state', () => {
    const a = reduce(initialState, snapshot());
    expect(reduce(a, { type: 'state', entity: 0x0100, attr: 0x01, value: true })).toBe(a);
    expect(reduce(a, { type: 'state', entity: 0x9999, attr: 0x01, value: true })).toBe(a);
    expect(reduce(a, { type: 'state', entity: 0x0100, attr: 0x77, value: 1 })).toBe(a);
  });

  it('compares xy values by content', () => {
    const xy: Entity = {
      id: 7, name: null, chip: 1, device: 'light',
      attrs: [{ id: 3, name: 'color_xy', vt: 'xy', flags: ['r', 'w'], value: [100, 200] }],
    };
    const a = reduce(initialState, { type: 'entity', op: 'add', entity: xy });
    expect(reduce(a, { type: 'state', entity: 7, attr: 3, value: [100, 200] })).toBe(a);
    expect(attrValue(reduce(a, { type: 'state', entity: 7, attr: 3, value: [1, 2] }), 7, 3)).toEqual([1, 2]);
  });

  it('entity add is an upsert, remove deletes', () => {
    const a = reduce(initialState, snapshot());
    const renamed = { ...findEntity(a, 0x0100)!, name: 'kitchen' };
    const b = reduce(a, { type: 'entity', op: 'add', entity: renamed });
    expect(b.entities).toHaveLength(3);
    expect(findEntity(b, 0x0100)!.name).toBe('kitchen');
    const fresh: Entity = { id: 0x0200, name: null, chip: 2, device: 'switch', attrs: [] };
    expect(reduce(b, { type: 'entity', op: 'add', entity: fresh }).entities).toHaveLength(4);
    const c = reduce(b, { type: 'entity', op: 'remove', id: 0x0100 });
    expect(c.entities.map((e) => e.id)).toEqual([0x0101, 0x0102]);
    expect(reduce(c, { type: 'entity', op: 'remove', id: 0x0100 })).toBe(c); // already gone
  });

  it('a chip event merges into the known chip and can introduce a new one', () => {
    const a = reduce(initialState, snapshot());
    const b = reduce(a, { type: 'chip', id: 1, status: 'silent', boot_id: 129, reboots: 0, telemetry: {} });
    expect(b.chips.find((c) => c.id === 1)).toMatchObject({ status: 'silent', label: 'zigbee' });
    const c = reduce(b, { type: 'chip', id: 3, status: 'seen', boot_id: 1, reboots: 0, telemetry: {} });
    expect(c.chips.find((x) => x.id === 3)!.label).toBe('chip3');
  });

  it('list events replace the lists; time updates the hub info', () => {
    let s = reduce(initialState, { type: 'welcome', v: 1, hub: { name: 'h', fw: 'x', time: 1, tz: 'UTC', synced: false } });
    s = reduce(s, { type: 'time', time: 99, tz: 'Europe/Prague', synced: true });
    expect(s.hub).toMatchObject({ time: 99, tz: 'Europe/Prague', synced: true, name: 'h' });
    s = reduce(s, { type: 'timers', timers: [{ index: 0, entity: 1, attr: 1, action: 'on', left_s: 5 }] });
    s = reduce(s, { type: 'schedules', schedules: [{ index: 0, time: '22:00', days: 'daily', entity: 1, attr: 1, action: 'off' }] });
    s = reduce(s, { type: 'rules', rules: [{ index: 0, when: { entity: 1, attr: 1, op: 'any' }, then: { entity: 2, attr: 1, value: true } }] });
    expect([s.timers.length, s.schedules.length, s.rules.length]).toEqual([1, 1, 1]);
  });

  it('messages without state (ack, error) leave the state untouched', () => {
    const a = reduce(initialState, snapshot());
    expect(reduce(a, { type: 'ack', id: 1, status: 'ok' })).toBe(a);
    expect(reduce(a, { type: 'error', code: 'busy' })).toBe(a);
  });

  it('the state kept when the link drops is marked as not live', () => {
    const a = withStatus(reduce(initialState, snapshot()), 'reconnecting', 'busy');
    const c = markedStale(a, 1000);
    expect(c.entities).toHaveLength(3); // the devices stay on screen
    expect(c.stale).toBe(true);
    expect(c.savedAt).toBe(1000);
    expect(c).toMatchObject({ status: 'reconnecting', error: 'busy' });
    // An already stale state keeps the moment it was stale at.
    expect(markedStale(c, 2000).savedAt).toBe(1000);
  });

  it('a snapshot makes the state live again', () => {
    const a = markedStale(reduce(initialState, snapshot()), 1000);
    expect(reduce(a, snapshot()).stale).toBe(false);
    expect(reduce(a, snapshot()).savedAt).toBeNull();
  });
});
