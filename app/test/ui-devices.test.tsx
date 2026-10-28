// @vitest-environment jsdom
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { modeOptions, sliderFor, stepped, stepperFor } from '../src/model/devices';
import { formatValue } from '../src/model/format';
import { inputUnit, parseInput } from '../src/model/automation';
import type { Attr, Entity } from '../src/protocol/messages';
import { DeviceSheet } from '../src/ui/DeviceSheet';
import { sliderValueAt } from '../src/ui/Slider';

afterEach(cleanup);

const attr = (id: number, name: string, vt: Attr['vt'], value: Attr['value'], flags: Attr['flags'] = ['r', 'rep']): Attr => ({
  id,
  name,
  vt,
  value,
  flags,
});
const entity = (device: string, name: string, attrs: Attr[]): Entity => ({ id: 0x200, name, chip: 1, device, attrs });

function sheet(e: Entity, onSet = vi.fn()) {
  render(
    <DeviceSheet
      entity={e}
      status="online"
      reachable
      visible
      isPending={() => false}
      onSet={onSet}
      onClose={() => {}}
      onRename={async () => true}
    />,
  );
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
  it('colour temperature is a slider in kelvin, the wire speaks mireds', () => {
    const ct = attr(4, 'color_temp', 'u16', 250, w);
    expect(stepperFor(ct)).toBeNull();
    const s = sliderFor(ct);
    expect(s?.min).toBe(2000);
    expect(s?.max).toBe(6500);
    expect(s?.show(250)).toBe(4000);
    expect(s?.wire(4000)).toBe(250);
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

describe('device sheet', () => {
  const w: Attr['flags'] = ['r', 'w', 'rep'];
  it('a colour lamp shows brightness and colour temperature sliders', () => {
    sheet(
      entity('light', 'lamp', [
        attr(1, 'on_off', 'bool', true, w),
        attr(2, 'brightness', 'u8', 127, w),
        attr(4, 'color_temp', 'u16', 250, w),
      ]),
    );
    const brightness = screen.getByLabelText('lamp brightness');
    expect(brightness.getAttribute('aria-valuetext')).toBe('50 %');
    expect(brightness.getAttribute('aria-valuemin')).toBe('0');
    expect(brightness.getAttribute('aria-valuemax')).toBe('100');
    const colour = screen.getByLabelText('lamp color temperature');
    expect(colour.getAttribute('aria-valuetext')).toBe('4000 K');
    expect(colour.getAttribute('aria-valuemin')).toBe('2000');
    expect(colour.getAttribute('aria-valuemax')).toBe('6500');
  });

  it('a touch on the track lands on the right value', () => {
    expect(sliderValueAt(0, 200, 0, 100, 10)).toBe(0);
    expect(sliderValueAt(100, 200, 0, 100, 10)).toBe(50);
    expect(sliderValueAt(199, 200, 0, 100, 10)).toBe(100);
    expect(sliderValueAt(100, 200, 2000, 6500, 100)).toBe(4300);
    expect(sliderValueAt(500, 0, 0, 100, 10)).toBe(0); // no width yet: the minimum
  });

  it('a thermostat shows the temperature, steps the target and switches mode', () => {
    const onSet = sheet(
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
    sheet(
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
    sheet(entity('contact', 'door', [attr(0x51, 'contact', 'bool', false)]));
    expect(screen.getByText('Open')).toBeTruthy();
  });

  it('a button hides the pulse and explains itself', () => {
    sheet(entity('button', 'remote', [attr(0x60, 'event', 'enum8', 0), attr(0x1f, 'battery', 'u8', 90)]));
    expect(screen.queryByText('Idle')).toBeNull();
    expect(screen.getByText('This button has no settings here — it can trigger rules.')).toBeTruthy();
    expect(screen.getByText('90 %')).toBeTruthy();
  });
});
