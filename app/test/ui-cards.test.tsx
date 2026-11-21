// @vitest-environment jsdom
// The device card: what is on glows gold, what is off is quiet.
// The rooms sit at the top as chips.
import './setup-dom';
import { cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { Attr, Entity } from '../src/protocol/messages';
import { DeviceWidget } from '../src/ui/DeviceWidget';
import { RoomChips } from '../src/ui/DevicesScreen';
import { ThemeProvider } from '../src/ui/theme';

afterEach(cleanup);

const attr = (id: number, name: string, vt: Attr['vt'], value: Attr['value'], flags: Attr['flags'] = ['r', 'rep']): Attr => ({
  id,
  name,
  vt,
  value,
  flags,
});

const lamp = (on: boolean): Entity => ({
  id: 0x100,
  name: 'Floor lamp',
  room: 'Living room',
  chip: 1,
  device: 'light',
  attrs: [attr(1, 'on_off', 'bool', on, ['r', 'w', 'rep'])],
});

function renderWidget(e: Entity) {
  render(
    <ThemeProvider mode="light">
      <DeviceWidget entity={e} status="online" reachable onOpen={() => {}} />
    </ThemeProvider>,
  );
  return screen.getByLabelText(`Device ${e.name}`);
}

describe('the device card', () => {
  it('a lit device glows gold', () => {
    const el = renderWidget(lamp(true));
    // The card itself is the gold-bordered one — the label is on the inner pressable.
    const card = el.parentElement?.getAttribute('style') ?? '';
    expect(card).toContain('rgba(207, 159, 51, 0.55)'); // the gold hairline
    expect(card).toContain('rgb(255, 248, 232)'); // the warm gold wash
  });

  it('a dark device is quiet', () => {
    const el = renderWidget(lamp(false));
    const card = el.parentElement?.getAttribute('style') ?? '';
    expect(card).not.toContain('rgba(207, 159, 51, 0.55)');
    expect(card).toContain('rgb(232, 225, 208)'); // the plain hairline
  });
});

describe('the climate sensor card', () => {
  it('shows the temperature and the humidity on one line', () => {
    const sensor: Entity = {
      id: 0x300,
      name: 'Climate',
      room: 'Bedroom',
      chip: 1,
      device: 'sensor',
      attrs: [
        attr(0x10, 'temperature', 'i16', 234),
        attr(0x11, 'humidity', 'u16', 580),
        attr(0x1f, 'battery', 'u8', 72),
      ],
    };
    render(
      <ThemeProvider mode="light">
        <DeviceWidget entity={sensor} status="online" reachable onOpen={() => {}} />
      </ThemeProvider>,
    );
    const card = screen.getByLabelText('Device Climate');
    expect(within(card).getByText('23.4 °C · 58 %')).toBeTruthy();
    // The battery is under it.
    expect(within(card).getByText('72 %')).toBeTruthy();
  });
});

describe('the room chips', () => {
  it('offers All and the rooms', () => {
    render(
      <ThemeProvider mode="light">
        <RoomChips rooms={['Living room', 'Kitchen']} active={null} onPick={() => {}} />
      </ThemeProvider>,
    );
    expect(screen.getByLabelText('All rooms')).toBeTruthy();
    expect(screen.getByLabelText('Living room')).toBeTruthy();
    expect(screen.getByLabelText('Kitchen')).toBeTruthy();
  });

  it('picking a room says which one', () => {
    const onPick = vi.fn();
    render(
      <ThemeProvider mode="light">
        <RoomChips rooms={['Living room']} active={null} onPick={onPick} />
      </ThemeProvider>,
    );
    fireEvent.click(screen.getByLabelText('Living room'));
    expect(onPick).toHaveBeenCalledWith('Living room');
  });
});
