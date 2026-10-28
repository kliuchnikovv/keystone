import { describe, expect, it } from 'vitest';
import { HubError } from '../src/protocol/client';
import type { Attr, Chip, Entity } from '../src/protocol/messages';
import { initialState, reduce } from '../src/protocol/store';
import { attrLabel, brightnessPercent, describeAck, describeError, formatEnergy, formatUptime, formatValue } from '../src/model/format';
import { canManageDevice, climateTiles, deviceTitle, hexId, isReachable, primarySwitch, roomSummary, sliderFor, sortedDevices, stepped, stepperFor, summaryText, widgetValue } from '../src/model/devices';
import { chipRows, chipSummary } from '../src/model/health';
import { defaultChips, defaultEntities } from '../tools/mock-hub/hub';

describe('formatValue', () => {
  const f = (name: string, value: unknown) => formatValue({ name, value: value as never });
  it('known attributes get units and words', () => {
    expect(f('on_off', true)).toBe('On');
    expect(f('on_off', false)).toBe('Off');
    expect(f('power_w', 3)).toBe('3 W');
    expect(f('energy_wh', 640)).toBe('640 Wh');
    expect(f('energy_wh', 1280)).toBe('1.28 kWh');
    expect(f('occupied', true)).toBe('Occupied');
    expect(f('occupied', false)).toBe('Clear');
    expect(f('temperature', 215)).toBe('21.5 °C');
    expect(f('temperature', -35)).toBe('-3.5 °C');
  });
  it('brightness is shown in percent of the 0..254 range', () => {
    expect(brightnessPercent(254)).toBe(100);
    expect(brightnessPercent(127)).toBe(50);
    expect(f('brightness', 254)).toBe('100 %');
    expect(f('brightness', 0)).toBe('0 %');
  });
  it('unknown values: a dash for never reported, plain text otherwise', () => {
    expect(f('on_off', null)).toBe('—');
    expect(f('power_w', null)).toBe('—');
    expect(f('mystery', 7)).toBe('7');
    expect(f('mystery', true)).toBe('Yes');
    expect(f('color_xy', [100, 200])).toBe('(100, 200)');
  });
  it('labels fall back to a readable form of the name', () => {
    expect(attrLabel('on_off')).toBe('Power');
    expect(attrLabel('fan_speed')).toBe('Fan speed');
  });
  it('uptime and energy helpers', () => {
    expect(formatUptime(45)).toBe('45s');
    expect(formatUptime(125)).toBe('2m');
    expect(formatUptime(3 * 3600 + 5 * 60)).toBe('3h 5m');
    expect(formatUptime(2 * 86400 + 3 * 3600)).toBe('2d 3h');
    expect(formatEnergy(999)).toBe('999 Wh');
    expect(formatEnergy(1000)).toBe('1.00 kWh');
  });
});

describe('messages for failures', () => {
  it('an ack other than ok is explained, ok is silent', () => {
    expect(describeAck('ok')).toBeNull();
    for (const s of ['unreachable', 'fail', 'unsupported', 'timeout'] as const) {
      expect(describeAck(s)).toMatch(/\w/);
    }
    expect(describeAck('unreachable')).toContain('unreachable');
  });
  it('hub errors map to plain language; unknown codes are still shown', () => {
    expect(describeError(new HubError('disconnected'))).toBe('Not connected to the hub');
    expect(describeError(new HubError('timeout'))).toContain('in time');
    expect(describeError(new HubError('read_only'))).toContain("can't be changed");
    expect(describeError(new HubError('bad_args', 'value does not fit u8'))).toBe('Invalid value: value does not fit u8');
    expect(describeError(new HubError('bad_args'))).toBe('Invalid value');
    expect(describeError(new HubError('internal'))).toBe('The hub reported: internal');
    expect(describeError(new Error('boom'))).toBe('boom');
    expect(describeError('?')).toBe('Something went wrong');
  });
});

describe('devices', () => {
  const ents = defaultEntities();
  const plug = ents[0]!;
  const lamp = ents[1]!;
  const sensor = ents[2]!;

  it('titles use the name, or type + id', () => {
    expect(deviceTitle(plug)).toBe('plug1');
    expect(deviceTitle(sensor)).toBe('Motion sensor 0x0102');
    expect(hexId(5)).toBe('0x0005');
  });
  it('the main switch is the writable bool on_off', () => {
    expect(primarySwitch(plug)?.id).toBe(0x01);
    expect(primarySwitch(sensor)).toBeUndefined();
    const ro: Entity = { ...plug, attrs: plug.attrs.map((a) => ({ ...a, flags: ['r'] })) };
    expect(primarySwitch(ro)).toBeUndefined();
  });
  it('brightness is a slider in percent, not a stepper', () => {
    const b = lamp.attrs.find((a) => a.name === 'brightness')!;
    expect(stepperFor(b)).toBeNull();
    const s = sliderFor(b)!;
    expect(s.min).toBe(0);
    expect(s.max).toBe(100);
    expect(s.step).toBe(10);
    expect(s.show(254)).toBe(100);
    expect(s.show(127)).toBe(50);
    expect(s.wire(100)).toBe(254);
    expect(s.wire(50)).toBe(127);
    expect(s.wire(0)).toBe(1); // 0 % would switch the lamp off
    expect(s.text(60)).toBe('60 %');
    expect(sliderFor(plug.attrs[1]!)).toBeNull();
  });
  it('colour temperature is a slider in kelvin, the wire speaks mireds', () => {
    const ct: Attr = { id: 4, name: 'color_temp', vt: 'u16', value: 250, flags: ['r', 'w', 'rep'] };
    expect(stepperFor(ct)).toBeNull();
    const s = sliderFor(ct)!;
    expect(s.min).toBe(2000);
    expect(s.max).toBe(6500);
    expect(s.show(250)).toBe(4000);
    expect(s.wire(4000)).toBe(250);
    expect(s.wire(2000)).toBe(500);
    expect(s.wire(6500)).toBe(154);
    expect(s.text(3000)).toBe('3000 K');
  });
  it('named devices come first, alphabetically; unnamed by id', () => {
    const named = (id: number, name: string | null): Entity => ({ id, name, chip: 1, device: 'switch', attrs: [] });
    const sorted = sortedDevices([named(9, null), named(3, 'zeta'), named(5, null), named(4, 'alpha')]);
    expect(sorted.map((e) => e.id)).toEqual([4, 3, 5, 9]);
  });
  it('a device is reachable only while its chip is ready', () => {
    let s = reduce(initialState, { type: 'snapshot', entities: ents, chips: defaultChips() });
    expect(isReachable(s, plug)).toBe(true);
    s = reduce(s, { type: 'chip', id: 1, status: 'silent', boot_id: 1, reboots: 0, telemetry: {} });
    expect(isReachable(s, plug)).toBe(false);
    expect(isReachable(s, { ...plug, chip: 9 })).toBe(true); // unknown chip: do not grey out
  });
});

describe('chipSummary', () => {
  const chip = (over: Partial<Chip>): Chip => ({
    id: 1, label: 'zigbee', status: 'ready', boot_id: 1, reboots: 0,
    telemetry: { heap: 377260, uptime_s: 4480, tx_frames: 10, crc_errors: 0 }, ...over,
  });
  it('a healthy chip', () => {
    expect(chipSummary(chip({}))).toEqual({
      title: 'Zigbee', tone: 'good', status: 'Online', details: ['Up 1h 14m', '368 KB free'],
    });
  });
  it('silent is bad, starting is a warning', () => {
    expect(chipSummary(chip({ status: 'silent' }))).toMatchObject({ tone: 'bad', status: 'Not responding' });
    expect(chipSummary(chip({ status: 'seen' }))).toMatchObject({ tone: 'warn', status: 'Starting' });
  });
  it('reboots and link errors are listed; many of them warn', () => {
    const s = chipSummary(chip({ reboots: 3, telemetry: { crc_errors: 355 } }));
    expect(s.tone).toBe('warn');
    expect(s.details).toEqual(['3 reboots seen', '355 link errors']);
    expect(chipSummary(chip({ reboots: 1 })).details).toContain('1 reboot seen');
  });
  it('missing telemetry is simply not shown', () => {
    expect(chipSummary(chip({ telemetry: {} })).details).toEqual([]);
  });
});

describe('chipRows', () => {
  const chip = (over: Partial<Chip>): Chip => ({
    id: 1, label: 'zigbee', status: 'ready', boot_id: 1, reboots: 0,
    telemetry: { heap: 377260, uptime_s: 4480, tx_frames: 10, crc_errors: 0 }, ...over,
  });
  it('one row per number the spec shows', () => {
    expect(chipRows(chip({}))).toEqual([
      { label: 'Uptime', value: '1h 14m' },
      { label: 'Free heap', value: '368 KB' },
      { label: 'Reboots seen', value: '0' },
      { label: 'Link errors', value: '0' },
    ]);
  });
  it('a missing number is a dash, not a lie', () => {
    expect(chipRows(chip({ telemetry: {} }))).toEqual([
      { label: 'Uptime', value: '—' },
      { label: 'Free heap', value: '—' },
      { label: 'Reboots seen', value: '0' },
      { label: 'Link errors', value: '0' },
    ]);
  });
});

describe('the room as a button grid', () => {
  const attr = (id: number, name: string, vt: Attr['vt'], value: Attr['value'], flags: Attr['flags'] = ['r', 'rep']): Attr => ({
    id,
    name,
    vt,
    value,
    flags,
  });
  const w: Attr['flags'] = ['r', 'w', 'rep'];
  const plug: Entity = { id: 0x100, name: 'plug1', chip: 1, device: 'switch', attrs: [attr(1, 'on_off', 'bool', true, w), attr(0x13, 'power_w', 'u16', 24)] };
  const lamp: Entity = { id: 0x101, name: 'lamp', chip: 1, device: 'light', attrs: [attr(1, 'on_off', 'bool', false, w), attr(2, 'brightness', 'u8', 128)] };
  const sensor: Entity = { id: 0x102, name: null, chip: 1, device: 'occupancy', attrs: [attr(0x50, 'occupied', 'bool', true)] };

  it('the summary counts what is on and what it draws', () => {
    expect(roomSummary([plug, lamp, sensor])).toEqual({ on: 1, total: 2, watts: 24 });
    expect(summaryText({ on: 1, total: 2, watts: 24 })).toBe('1 of 2 on · 24 W');
    expect(summaryText({ on: 2, total: 2, watts: null })).toBe('2 of 2 on');
    expect(summaryText({ on: 1, total: 1, watts: null })).toBe('On');
    expect(roomSummary([sensor])).toBeNull(); // nothing to switch
  });

  it('the climate tiles say only what a device reports', () => {
    expect(climateTiles([plug, lamp])).toEqual([]);
    const climate: Entity = {
      id: 0x103,
      name: 'climate',
      chip: 1,
      device: 'sensor',
      attrs: [attr(0x10, 'temperature', 'i16', 215), attr(0x11, 'humidity', 'u16', 480), attr(0x12, 'illuminance', 'u16', 320)],
    };
    expect(climateTiles([climate])).toEqual([
      { label: 'Temp.', value: '21.5 °C', kind: 'warm' },
      { label: 'Humidity', value: '48 %', kind: 'cool' },
      { label: 'Light', value: '320 lx', kind: 'good' },
    ]);
  });

  it('a button says what the device is doing, and nothing when it is off', () => {
    expect(widgetValue(plug, true)).toBe('24 W');
    expect(widgetValue(lamp, false)).toBeNull(); // off: no value line
    expect(widgetValue(lamp, true)).toBe('50 %');
    expect(widgetValue(sensor, false)).toBe('Occupied'); // a sensor has no switch
  });
});

describe('canManageDevice', () => {
  it('only devices on the Zigbee chip can be identified or removed', () => {
    const state = reduce(initialState, { type: 'snapshot', entities: defaultEntities(), chips: defaultChips(), rules: [], schedules: [], timers: [] });
    const zb = state.entities[0]!;
    expect(canManageDevice(state, zb)).toBe(true);
    expect(canManageDevice(state, { ...zb, chip: 2 })).toBe(false); // a Matter-side mirror
    expect(canManageDevice(state, { ...zb, chip: 9 })).toBe(false); // unknown chip
  });
});
