// The automations the home can already do — and only those.
import { describe, expect, it } from 'vitest';
import { suggestionsFor } from '../src/model/suggestions';
import type { Attr, Entity } from '../src/protocol/messages';
import type { HomeState } from '../src/protocol/store';

const attr = (id: number, name: string, vt: Attr['vt'], value: Attr['value'], flags: Attr['flags'] = ['r', 'rep']): Attr => ({
  id,
  name,
  vt,
  value,
  flags,
});

const entity = (id: number, device: string, name: string, attrs: Attr[], room: string | null = null): Entity => ({
  id,
  name,
  room,
  chip: 1,
  device,
  attrs,
});

const home = (entities: Entity[]): HomeState => ({ entities } as unknown as HomeState);

const onOff = (id = 1): Attr => attr(id, 'on_off', 'bool', true, ['r', 'w', 'rep']);

describe('the suggested automations', () => {
  it('a home with nothing gets nothing', () => {
    expect(suggestionsFor(home([]))).toEqual([]);
  });

  it('a motion sensor and a light in the same room make one', () => {
    const motion = entity(0x200, 'occupancy', 'Hall sensor', [attr(0x50, 'occupied', 'bool', false)], 'Hallway');
    const light = entity(0x100, 'light', 'Hallway lamp', [onOff()], 'Hallway');
    const out = suggestionsFor(home([motion, light]));
    const first = out.find((s) => s.id.startsWith('motion-'));
    expect(first).toBeTruthy();
    expect(first!.title).toContain('Hallway lamp');
    // The rule is already filled in: only Save is left.
    expect(first!.draft.trigger).toBeTruthy();
    expect(first!.draft.action).toBeTruthy();
  });

  it('without a motion sensor there is no motion suggestion', () => {
    const light = entity(0x100, 'light', 'Floor lamp', [onOff()], 'Living room');
    const out = suggestionsFor(home([light]));
    expect(out.find((s) => s.id.startsWith('motion-'))).toBeUndefined();
  });

  it('a plug that reports its power makes a warning', () => {
    const plug = entity(0x300, 'switch', 'Heater', [onOff(), attr(0x13, 'power_w', 'u16', 0)]);
    const out = suggestionsFor(home([plug]));
    expect(out.find((s) => s.id.startsWith('power-'))).toBeTruthy();
  });

  it('a plug without a power reading makes no warning', () => {
    const plug = entity(0x300, 'switch', 'Heater', [onOff()]);
    const out = suggestionsFor(home([plug]));
    expect(out.find((s) => s.id.startsWith('power-'))).toBeUndefined();
  });

  it('a light can be switched off in the morning', () => {
    const light = entity(0x100, 'light', 'Floor lamp', [onOff()]);
    const out = suggestionsFor(home([light]));
    expect(out.find((s) => s.id === 'sunrise-off')).toBeTruthy();
  });
});
