// @vitest-environment jsdom
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { modeOptions, stepped, stepperFor } from '../src/model/devices';
import { formatValue } from '../src/model/format';
import { inputUnit, parseInput } from '../src/model/automation';
import type { Attr, Entity } from '../src/protocol/messages';
import { DeviceCard } from '../src/ui/DeviceCard';

afterEach(cleanup);

const attr = (id: number, name: string, vt: Attr['vt'], value: Attr['value'], flags: Attr['flags'] = ['r', 'rep']): Attr => ({
  id,
  name,
  vt,
  value,
  flags,
});
const entity = (device: string, name: string, attrs: Attr[]): Entity => ({ id: 0x200, name, chip: 1, device, attrs });

function card(e: Entity, onSet = vi.fn()) {
  render(<DeviceCard entity={e} reachable isPending={() => false} onSet={onSet} onRename={async () => true} />);
  return onSet;
}

describe('values of the new device types', () => {
  it('reads like a person would', () => {
    expect(formatValue({ name: 'temperature', value: 215 })).toBe('21.5 °C');
    expect(formatValue({ name: 'setpoint', value: 205 })).toBe('20.5 °C');
    expect(formatValue({ name: 'humidity', value: 551 })).toBe('55 %');
    expect(formatValue({ name: 'illuminance', value: 320 })).toBe('320 lx');
    expect(formatValue({ name: 'battery', value: 87 })).toBe('87 %');
    expect(formatValue({ name: 'contact', value: true })).toBe('Closed');
    expect(formatValue({ name: 'contact', value: false })).toBe('Open');
    expect(formatValue({ name: 'color_temp', value: 250 })).toBe('4000 K');
    expect(formatValue({ name: 'mode', value: 4 })).toBe('Heat');
    expect(formatValue({ name: 'event', value: 1 })).toBe('Pressed');
  });

  it('forms take the units people use', () => {
    expect(inputUnit('setpoint')).toBe('°C');
    expect(parseInput('setpoint', 'i16', '21.5')).toEqual({ ok: true, value: 215 });
    expect(parseInput('humidity', 'u16', '60')).toEqual({ ok: true, value: 600 });
    expect(parseInput('battery', 'u8', '20')).toEqual({ ok: true, value: 20 });
    expect(parseInput('battery', 'u8', '300').ok).toBe(false);
    expect(parseInput('illuminance', 'u16', '100')).toEqual({ ok: true, value: 100 });
  });
});

describe('steppers and modes', () => {
  const w: Attr['flags'] = ['r', 'w', 'rep'];
  it('colour temperature steps in mireds and stays inside the lamp range', () => {
    const ct = attr(4, 'color_temp', 'u16', 250, w);
    expect(stepperFor(ct)?.up).toBe('warmer');
    expect(stepped(ct, 1)).toBe(275);
    expect(stepped({ ...ct, value: 490 }, 1)).toBe(500);
    expect(stepped({ ...ct, value: 160 }, -1)).toBe(153);
  });
  it('target temperature moves half a degree within 5..35', () => {
    const sp = attr(0x40, 'setpoint', 'i16', 205, w);
    expect(stepped(sp, 1)).toBe(210);
    expect(stepped({ ...sp, value: 348 }, 1)).toBe(350);
    expect(stepped({ ...sp, value: 52 }, -1)).toBe(50);
  });
  it('a read-only attribute has no stepper, a writable mode has choices', () => {
    expect(stepperFor(attr(0x40, 'setpoint', 'i16', 205))).toBeNull();
    expect(modeOptions(attr(0x41, 'mode', 'enum8', 4))).toBeNull();
    expect(modeOptions(attr(0x41, 'mode', 'enum8', 4, w))?.map((m) => m.label)).toEqual(['Heat', 'Auto', 'Cool', 'Off']);
  });
});

describe('device cards', () => {
  const w: Attr['flags'] = ['r', 'w', 'rep'];
  it('a colour lamp shows brightness and colour temperature controls', () => {
    const onSet = card(
      entity('light', 'lamp', [
        attr(1, 'on_off', 'bool', true, w),
        attr(2, 'brightness', 'u8', 127, w),
        attr(4, 'color_temp', 'u16', 250, w),
      ]),
    );
    expect(screen.getByLabelText('lamp brightness').textContent).toBe('50 %');
    expect(screen.getByLabelText('lamp color temperature').textContent).toBe('4000 K');
    fireEvent.click(screen.getByLabelText('lamp warmer'));
    expect(onSet).toHaveBeenCalledWith(4, 275);
    fireEvent.click(screen.getByLabelText('lamp brighter'));
    expect(onSet).toHaveBeenCalledWith(2, 152);
  });

  it('a thermostat shows the temperature, steps the target and switches mode', () => {
    const onSet = card(
      entity('climate', 'hall', [
        attr(0x10, 'temperature', 'i16', 205),
        attr(0x40, 'setpoint', 'i16', 210, w),
        attr(0x41, 'mode', 'enum8', 4, w),
      ]),
    );
    expect(screen.getByText('20.5 °C')).toBeTruthy();
    expect(screen.getByLabelText('hall target temperature').textContent).toBe('21.0 °C');
    fireEvent.click(screen.getByLabelText('hall higher'));
    expect(onSet).toHaveBeenCalledWith(0x40, 215);
    fireEvent.click(screen.getByLabelText('hall mode off'));
    expect(onSet).toHaveBeenCalledWith(0x41, 0);
  });

  it('a sensor lists its readings, a contact sensor says open or closed', () => {
    card(
      entity('sensor', 'bedroom', [
        attr(0x10, 'temperature', 'i16', 195),
        attr(0x11, 'humidity', 'u16', 480),
        attr(0x1f, 'battery', 'u8', 64),
      ]),
    );
    expect(screen.getByText('19.5 °C')).toBeTruthy();
    expect(screen.getByText('48 %')).toBeTruthy();
    expect(screen.getByText('64 %')).toBeTruthy();
    cleanup();
    card(entity('contact', 'door', [attr(0x51, 'contact', 'bool', false)]));
    expect(screen.getByText('Open')).toBeTruthy();
  });

  it('a button card hides the pulse and explains itself', () => {
    card(entity('button', 'remote', [attr(0x60, 'event', 'enum8', 0), attr(0x1f, 'battery', 'u8', 90)]));
    expect(screen.queryByText('Idle')).toBeNull();
    expect(screen.getByText(/start automations/)).toBeTruthy();
    expect(screen.getByText('90 %')).toBeTruthy();
  });
});
