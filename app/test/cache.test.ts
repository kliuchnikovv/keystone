import { describe, expect, it } from 'vitest';
import { cachedState, CACHE_VERSION, decodeCache, encodeCache, sameHub } from '../src/protocol/cache';
import { initialState, reduce, withStatus } from '../src/protocol/store';
import { defaultChips, defaultEntities } from '../tools/mock-hub/hub';

const URL = 'ws://192.168.0.202:8080/ws';

function liveState() {
  return reduce(initialState, {
    type: 'snapshot',
    entities: defaultEntities(),
    chips: defaultChips(),
    rules: [{ index: 0, when: { entity: 0x0100, attr: 0x01, op: 'any' }, then: { entity: 0x0101, attr: 0x01, value: true } }],
    schedules: [{ index: 0, time: '22:00', days: 'daily', entity: 0x0100, attr: 0x01, action: 'off' }],
    timers: [{ index: 0, entity: 0x0100, attr: 0x01, action: 'on', left_s: 30 }],
    scenes: [{ name: 'night', actions: [{ entity: 0x0100, attr: 0x01, value: false }] }],
  });
}

describe('the cache format', () => {
  it('keeps the whole home and reads it back', () => {
    const s = liveState();
    const text = encodeCache(s, URL, 1700000000000);
    const back = decodeCache(text);
    expect(back).not.toBeNull();
    expect(back!.url).toBe(URL);
    expect(back!.savedAt).toBe(1700000000000);
    expect(back!.entities.map((e) => e.id)).toEqual(s.entities.map((e) => e.id));
    expect(back!.chips.map((c) => c.label)).toEqual(['zigbee', 'thread']);
    expect(back!.rules).toHaveLength(1);
    expect(back!.schedules).toHaveLength(1);
    expect(back!.timers).toHaveLength(1);
    expect(back!.scenes[0]!.actions[0]).toEqual({ entity: 0x0100, attr: 0x01, value: false });
  });

  it('the state it gives back is the last known one, not live', () => {
    const s = cachedState(decodeCache(encodeCache(liveState(), URL, 1700000000000))!);
    expect(s.stale).toBe(true);
    expect(s.savedAt).toBe(1700000000000);
    expect(s.status).toBe('idle');
    expect(s.entities).toHaveLength(3);
  });

  it('a stale state keeps its own moment: writing it again does not make it fresher', () => {
    const cached = cachedState(decodeCache(encodeCache(liveState(), URL, 1700000000000))!);
    const again = decodeCache(encodeCache(cached, URL, 1800000000000))!;
    expect(again.savedAt).toBe(1700000000000);
  });

  it('a live state is stamped with the moment it was written', () => {
    const back = decodeCache(encodeCache(liveState(), URL, 1800000000000))!;
    expect(back.savedAt).toBe(1800000000000);
  });

  it('a broken cache is nothing, not a crash', () => {
    expect(decodeCache(null)).toBeNull();
    expect(decodeCache('')).toBeNull();
    expect(decodeCache('{not json')).toBeNull();
    expect(decodeCache('[]')).toBeNull();
    expect(decodeCache('{"v":999,"url":"x","savedAt":1}')).toBeNull(); // another version
    expect(decodeCache(`{"v":${CACHE_VERSION},"savedAt":1}`)).toBeNull(); // no url
    expect(decodeCache(`{"v":${CACHE_VERSION},"url":"x"}`)).toBeNull(); // no moment
  });

  it('items that make no sense are dropped, the rest is kept', () => {
    const raw = JSON.stringify({
      v: CACHE_VERSION,
      savedAt: 1700000000000,
      url: URL,
      entities: [
        { id: 1, name: null, chip: 1, device: 'light', attrs: [{ id: 2, name: 'on_off', vt: 'bool', flags: ['r', 'w'], value: true }] },
        { id: 'nope', name: null, chip: 1, device: 'light', attrs: [] }, // no id
        { id: 3, name: null, chip: 1, device: 'light', attrs: [{ id: 2, name: 'x', vt: 'bool', flags: [], value: 'text' }] }, // bad value
      ],
      chips: [{ id: 1, label: 'zigbee', status: 'ready', boot_id: 3, reboots: 0, telemetry: {} }],
    });
    const back = decodeCache(raw)!;
    expect(back.entities.map((e) => e.id)).toEqual([1, 3]); // the broken one is gone, the good ones stay
    expect(back.entities[1]!.attrs).toEqual([]); // its broken attribute was dropped
    expect(back.chips).toHaveLength(1);
  });

  it('a cache of another home is not this home', () => {
    expect(sameHub('ws://hub.local:8080/ws', 'ws://hub.local:8080/ws')).toBe(true);
    expect(sameHub('ws://hub.local:8080/ws/', 'WS://HUB.LOCAL:8080/ws')).toBe(true);
    expect(sameHub('ws://hub.local:8080/ws', 'ws://other.local:8080/ws')).toBe(false);
    expect(sameHub('wss://relay.example.com/hub/abc', 'wss://relay.example.com/hub/abc')).toBe(true);
    expect(sameHub('wss://relay.example.com/hub/abc', 'wss://relay.example.com/hub/xyz')).toBe(false);
  });

  it('a state that is not ready is still worth keeping', () => {
    const s = withStatus(liveState(), 'reconnecting');
    const back = decodeCache(encodeCache(s, URL, 1700000000000))!;
    expect(back.entities).toHaveLength(3);
  });
});
