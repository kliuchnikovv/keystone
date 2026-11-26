// @vitest-environment jsdom
// The suggested automations: a slim banner on the Automations tab.
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Suggestions } from '../src/ui/Suggestions';
import { suggestionsFor } from '../src/model/suggestions';
import type { Attr, Entity } from '../src/protocol/messages';
import type { HomeState } from '../src/protocol/store';

afterEach(cleanup);

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

const motion = entity(0x200, 'occupancy', 'Hall sensor', [attr(0x50, 'occupied', 'bool', false)], 'Hallway');
const lamp = entity(0x100, 'light', 'Hallway lamp', [attr(1, 'on_off', 'bool', true, ['r', 'w', 'rep'])], 'Hallway');

const home = { entities: [motion, lamp] } as unknown as HomeState;

describe('the suggestions', () => {
  it('shows what the home can do', () => {
    const items = suggestionsFor(home);
    expect(items.length).toBeGreaterThan(0);
    render(<Suggestions items={items} onPick={() => {}} />);
    expect(screen.getByText(/Hallway lamp on when there is motion/)).toBeTruthy();
    expect(screen.getByText(/You have a motion sensor/)).toBeTruthy();
  });

  it('picking one hands the rule over', () => {
    const items = suggestionsFor(home);
    const onPick = vi.fn();
    render(<Suggestions items={items} onPick={onPick} />);
    fireEvent.click(screen.getByLabelText(/^Add Hallway lamp/));
    expect(onPick).toHaveBeenCalled();
    // The rule is already written: the trigger and the action are both set.
    const picked = onPick.mock.calls[0]?.[0];
    expect(picked.draft.trigger).toBeTruthy();
    expect(picked.draft.action).toBeTruthy();
  });

  it('a home with nothing to suggest shows nothing', () => {
    const empty = { entities: [] } as unknown as HomeState;
    expect(suggestionsFor(empty)).toEqual([]);
    const { container } = render(<Suggestions items={[]} onPick={() => {}} />);
    expect(container.firstChild).toBeNull();
  });
});
