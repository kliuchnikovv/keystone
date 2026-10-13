import { describe, expect, it } from 'vitest';
import {
  dayOf,
  describeEvent,
  emptyHistory,
  formatAgo,
  formatEventTime,
  MAX_KEPT,
  mergeNewest,
  mergeOlder,
  parseHistoryPage,
  secondsAgo,
  type Recorded,
} from '../src/model/history';
import type { Entity, HistoryEvent, HistoryPage } from '../src/protocol/messages';
import { defaultChips, defaultEntities } from '../tools/mock-hub/hub';

const NOW = 1_700_000_000;
const ev = (seq: number, over: Partial<HistoryEvent> = {}): HistoryEvent => ({ seq, age: (100 - seq) * 2, kind: 'state', entity: 0x0100, attr: 1, value: true, ...over });
/** A page like the hub sends: newest first. */
const page = (seqs: number[], latest = Math.max(...seqs), oldest = 1, now = NOW, more = false): HistoryPage => ({
  now,
  latest,
  oldest,
  more,
  events: [...seqs].sort((a, b) => b - a).map((s) => ev(s)),
});
const home = { entities: defaultEntities(), chips: defaultChips() };
const rec = (e: Partial<Recorded> & { kind: string }): Recorded => ({ seq: 1, age: 0, time: NOW, fetchedAt: 0, ...e });

describe('parseHistoryPage', () => {
  it('reads what the hub sends', () => {
    const p = parseHistoryPage({
      now: NOW, latest: 9, oldest: 3,
      events: [{ seq: 9, age: 4, kind: 'state', entity: 256, attr: 1, value: true }, { seq: 8, age: 6, kind: 'chip', what: 'lost', ref: 1, value: 20 }, { seq: 7, age: 9, kind: 'state', entity: 256, attr: 3, value: [10, 20] }],
    });
    expect(p?.events).toHaveLength(3);
    expect(p?.events[0]).toEqual({ seq: 9, age: 4, kind: 'state', entity: 256, attr: 1, value: true });
    expect(p?.events[1]).toMatchObject({ what: 'lost', ref: 1, value: 20 });
    expect(p?.events[2]?.value).toEqual([10, 20]);
    expect(p?.more).toBe(false);
    expect(parseHistoryPage({ now: 1, latest: 1, oldest: 1, more: true, events: [] })?.more).toBe(true);
  });

  it('refuses what is not a page and drops malformed events instead of failing', () => {
    expect(parseHistoryPage(null)).toBeNull();
    expect(parseHistoryPage({ now: 'x', latest: 1, oldest: 1, events: [] })).toBeNull();
    expect(parseHistoryPage({ now: 1, latest: 1, oldest: 1, events: 'no' })).toBeNull();
    const p = parseHistoryPage({ now: 1, latest: 3, oldest: 1, events: [null, 5, { seq: 'a' }, { seq: 2, age: 1 }, { seq: 3, age: 1, kind: 'boot', value: { x: 1 } }] });
    expect(p?.events).toEqual([{ seq: 3, age: 1, kind: 'boot' }]);
  });
});

describe('merging pages', () => {
  it('the first page becomes the list; hasMore says whether the hub holds older ones', () => {
    const l = mergeNewest(emptyHistory, page([7, 8, 9], 9, 1, NOW, true), 1000);
    expect(l.events.map((e) => e.seq)).toEqual([9, 8, 7]);
    expect(l.latest).toBe(9);
    expect(l.hasMore).toBe(true);
    expect(mergeNewest(emptyHistory, page([1, 2], 2, 1), 1000).hasMore).toBe(false);
  });

  it('events get a wall time from the hub clock, or none when it is not synced', () => {
    expect(mergeNewest(emptyHistory, page([9], 9, 1, NOW), 0).events[0]?.time).toBe(NOW - 182);
    expect(mergeNewest(emptyHistory, page([9], 9, 1, 0), 0).events[0]?.time).toBeNull();
  });

  it('a newer page puts only the new events on top', () => {
    let l = mergeNewest(emptyHistory, page([7, 8, 9], 9, 1, NOW, true), 1000);
    l = mergeNewest(l, page([8, 9, 10, 11], 11, 1, NOW, true), 2000);
    expect(l.events.map((e) => e.seq)).toEqual([11, 10, 9, 8, 7]);
    expect(l.hasMore).toBe(true); // the older ones we never fetched are still there
    expect(l.latest).toBe(11);
    expect(l.events.find((e) => e.seq === 9)?.fetchedAt).toBe(1000); // untouched
    expect(l.events.find((e) => e.seq === 11)?.fetchedAt).toBe(2000);
    // nothing new
    const same = mergeNewest(l, page([9, 10, 11], 11, 1, NOW, true), 3000);
    expect(same.events).toEqual(l.events);
  });

  it('when more happened than one page holds, the list starts over from the page', () => {
    let l = mergeNewest(emptyHistory, page([7, 8, 9], 9, 1), 1000);
    l = mergeNewest(l, page([40, 41, 42], 42, 1, NOW, true), 2000); // 10..39 are not in the page
    expect(l.events.map((e) => e.seq)).toEqual([42, 41, 40]);
    expect(l.hasMore).toBe(true);
  });

  it('a hub that restarted (sequence numbers began again) replaces the list', () => {
    let l = mergeNewest(emptyHistory, page([50, 51], 51, 1), 1000);
    l = mergeNewest(l, page([1, 2, 3], 3, 1), 2000);
    expect(l.events.map((e) => e.seq)).toEqual([3, 2, 1]);
    expect(l.latest).toBe(3);
  });

  it('older pages go on the end, and none is added twice', () => {
    let l = mergeNewest(emptyHistory, page([7, 8, 9], 9, 1, NOW, true), 1000);
    l = mergeOlder(l, page([4, 5, 6, 7], 9, 1, NOW, true), 2000);
    expect(l.events.map((e) => e.seq)).toEqual([9, 8, 7, 6, 5, 4]);
    expect(l.hasMore).toBe(true);
    l = mergeOlder(l, page([1, 2, 3], 9, 1), 3000);
    expect(l.events).toHaveLength(9);
    expect(l.hasMore).toBe(false);
  });

  it('the list is capped', () => {
    const many = Array.from({ length: MAX_KEPT + 50 }, (_, i) => i + 1);
    const l = mergeNewest(emptyHistory, page(many, many.length, 1), 0);
    expect(l.events).toHaveLength(MAX_KEPT);
    expect(l.events[0]?.seq).toBe(many.length);
    // growing past the cap by new events: the oldest fall off, and there is more to ask for only if the hub says so
    const grown = mergeNewest(l, page([many.length + 1], many.length + 1, 1), 1);
    expect(grown.events).toHaveLength(MAX_KEPT);
    expect(grown.hasMore).toBe(true);
    // the older pages stop at the cap
    expect(mergeOlder(mergeNewest(emptyHistory, page(many.slice(-MAX_KEPT), many.length, 1, NOW, true), 0), page([1], many.length, 1, NOW, true), 0).hasMore).toBe(false);
  });
});

describe('time', () => {
  it('ages the event by how long ago the page arrived', () => {
    const e = rec({ kind: 'state', age: 30, fetchedAt: 1000, time: null });
    expect(secondsAgo(e, 1000)).toBe(30);
    expect(secondsAgo(e, 11_000)).toBe(40);
    expect(secondsAgo(rec({ kind: 'state', age: 0, fetchedAt: 5000 }), 1000)).toBe(0); // a clock that stepped back
  });

  it('says how long ago in words', () => {
    expect(formatAgo(2)).toBe('just now');
    expect(formatAgo(42)).toBe('42 s ago');
    expect(formatAgo(125)).toBe('2m ago');
    expect(formatAgo(7500)).toBe('2h 5m ago');
  });

  it('shows a clock time when the hub knows the time, how long ago when not', () => {
    const d = new Date(2026, 9, 13, 14, 5, 9);
    expect(formatEventTime(rec({ kind: 'state', time: d.getTime() / 1000 }), 0)).toBe('14:05:09');
    expect(formatEventTime(rec({ kind: 'state', time: null, age: 90, fetchedAt: 0 }), 0)).toBe('1m ago');
  });

  it('groups by day: Today, Yesterday, then a date; no heading without a clock', () => {
    const now = new Date(2026, 9, 13, 10, 0, 0).getTime();
    const at = (d: Date) => rec({ kind: 'state', time: d.getTime() / 1000 });
    expect(dayOf(at(new Date(2026, 9, 13, 0, 5)), now)).toBe('Today');
    expect(dayOf(at(new Date(2026, 9, 12, 23, 55)), now)).toBe('Yesterday');
    expect(dayOf(at(new Date(2026, 9, 1, 12, 0)), now)).toMatch(/1/);
    expect(dayOf(rec({ kind: 'state', time: null }), now)).toBeNull();
  });
});

describe('describeEvent', () => {
  const say = (e: Partial<HistoryEvent> & { kind: string }, h: { entities: Entity[]; chips: typeof home.chips } = home) => describeEvent({ seq: 1, age: 0, ...e }, h).text;

  it('a device changing', () => {
    expect(say({ kind: 'state', entity: 0x0100, attr: 1, value: true })).toBe('plug1 turned on');
    expect(say({ kind: 'state', entity: 0x0100, attr: 1, value: false })).toBe('plug1 turned off');
    expect(say({ kind: 'state', entity: 0x0102, attr: 0x50, value: true })).toBe('Motion sensor 0x0102 detected someone');
    expect(say({ kind: 'state', entity: 0x0101, attr: 2, value: 127 })).toBe('lamp brightness: 50 %');
    expect(say({ kind: 'state', entity: 0x0100, attr: 0x10, value: 215 })).toBe('plug1 temperature: 21.5 °C');
  });

  it('contacts and buttons read naturally', () => {
    const doors: Entity[] = [{ id: 0x0200, name: 'front_door', chip: 1, device: 'contact', attrs: [{ id: 0x51, name: 'contact', vt: 'bool', flags: ['r'], value: true }] },
      { id: 0x0201, name: 'remote', chip: 1, device: 'button', attrs: [{ id: 0x60, name: 'event', vt: 'enum8', flags: ['r'], value: 0 }] }];
    const h = { entities: doors, chips: [] };
    expect(say({ kind: 'state', entity: 0x0200, attr: 0x51, value: false }, h)).toBe('front_door opened');
    expect(say({ kind: 'state', entity: 0x0200, attr: 0x51, value: true }, h)).toBe('front_door closed');
    expect(say({ kind: 'state', entity: 0x0201, attr: 0x60, value: 1 }, h)).toBe('remote button: pressed');
  });

  it('a device that is gone is still described, from the attribute id', () => {
    expect(say({ kind: 'state', entity: 0x0999, attr: 1, value: true })).toBe('Device 0x0999 turned on');
    expect(say({ kind: 'state', entity: 0x0999, attr: 0x41, value: 4 })).toBe('Device 0x0999 mode: Heat');
  });

  it('commands and their failures', () => {
    expect(say({ kind: 'command', entity: 0x0100, attr: 1, value: false })).toBe('Sent to plug1: turned off');
    const failed = describeEvent({ seq: 1, age: 0, kind: 'command_failed', what: 'unreachable', entity: 0x0100, attr: 1 }, home);
    expect(failed.text).toBe('plug1: power command failed (unreachable)');
    expect(failed.tone).toBe('bad');
  });

  it('rules, schedules, timers and scenes say what they did', () => {
    expect(say({ kind: 'rule', ref: 0, entity: 0x0101, attr: 1, value: true })).toBe('Rule 1 ran: lamp: turned on');
    expect(say({ kind: 'schedule', ref: 2, entity: 0x0100, attr: 1, value: false })).toBe('Schedule 3 ran: plug1: turned off');
    expect(say({ kind: 'timer', ref: 0, entity: 0xfffe, scene: 'night' })).toBe('Timer 1 ran: scene “night”');
    expect(say({ kind: 'scene', what: 'schedule', ref: 2, scene: 'night' })).toBe('Applied scene “night” by a schedule');
    expect(say({ kind: 'scene', what: 'app', ref: 2 })).toBe('Applied a scene that has since been deleted from the app');
  });

  it('chips, restarts, reconciliation and pairing', () => {
    expect(describeEvent({ seq: 1, age: 0, kind: 'chip', what: 'lost', ref: 1, value: 20 }, home)).toMatchObject({ text: 'The zigbee chip went silent (20s without a sign)', tone: 'warn' });
    expect(say({ kind: 'chip', what: 'resumed', ref: 2, value: 95 })).toBe('The thread chip is back after 1m');
    expect(describeEvent({ seq: 1, age: 0, kind: 'chip', what: 'flapping', ref: 1, value: 3 }, home).tone).toBe('bad');
    expect(say({ kind: 'chip', what: 'rebooted', ref: 7, value: 2 })).toBe('The chip 7 restarted (restart 2)');
    expect(say({ kind: 'boot', what: 'power_on' })).toBe('Hub started (power on)');
    expect(say({ kind: 'boot', what: 'unknown' })).toBe('Hub started');
    expect(describeEvent({ seq: 1, age: 0, kind: 'boot', what: 'panic' }, home).tone).toBe('bad');
    expect(say({ kind: 'reconcile', what: 'corrected', entity: 0x0100, attr: 1, value: false })).toBe('plug1 had turned off behind the hub’s back; putting it back');
    expect(say({ kind: 'reconcile', what: 'adopted', entity: 0x0100, attr: 1, value: true })).toBe('plug1 turned on outside the hub');
    expect(say({ kind: 'pairing', what: 'opened', ref: 180 })).toBe('Network opened for new devices (180 s)');
    expect(say({ kind: 'pairing', what: 'unpair', entity: 0x0100 })).toBe('Removing plug1 from the network');
    expect(say({ kind: 'pairing', what: 'identify', entity: 0x0100 })).toBe('Blinking plug1');
  });

  it('a kind from a newer hub still shows something', () => {
    expect(say({ kind: 'firmware', what: 'updated' })).toBe('firmware updated');
  });
});
