import { describe, expect, it } from 'vitest';
import { HubError } from '../src/protocol/client';
import type { Chip, Entity } from '../src/protocol/messages';
import { initialState, reduce } from '../src/protocol/store';
import { attrLabel, brightnessPercent, describeAck, describeError, formatEnergy, formatUptime, formatValue } from '../src/model/format';
import { canManageDevice, deviceTitle, hexId, isReachable, primarySwitch, sortedDevices, stepped, stepperFor } from '../src/model/devices';
import { chipSummary } from '../src/model/health';
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
  it('brightness can be nudged in 10 % steps and clamps at the ends', () => {
    const b = lamp.attrs.find((a) => a.name === 'brightness')!;
    expect(stepperFor(b)).toMatchObject({ step: 25 });
    expect(stepped(b, 1)).toBe(153);
    expect(stepped(b, -1)).toBe(103);
    expect(stepped({ ...b, value: 250 }, 1)).toBe(254);
    expect(stepped({ ...b, value: 10 }, -1)).toBe(1);
    expect(stepped({ ...b, value: null }, 1)).toBeNull();
    expect(stepperFor(plug.attrs[1]!)).toBeNull();
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

describe('canManageDevice', () => {
  it('only devices on the Zigbee chip can be identified or removed', () => {
    const state = reduce(initialState, { type: 'snapshot', entities: defaultEntities(), chips: defaultChips(), rules: [], schedules: [], timers: [] });
    const zb = state.entities[0]!;
    expect(canManageDevice(state, zb)).toBe(true);
    expect(canManageDevice(state, { ...zb, chip: 2 })).toBe(false); // a Matter-side mirror
    expect(canManageDevice(state, { ...zb, chip: 9 })).toBe(false); // unknown chip
  });
});
