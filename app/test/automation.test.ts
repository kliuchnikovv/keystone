import { describe, expect, it } from 'vitest';
import type { Entity, Rule, Schedule, Timer } from '../src/protocol/messages';
import {
  addableSettings, draftsFromCurrentState, draftsRequest, inputText, sceneDrafts, describeWifi, signalText, wifiRequest, describeLocation, describeTimePoint, isSunPoint, locationRequest, validTimePoint, attrOptions, captureScene, describeScene, sceneRequest, describeDays, describeRule, describeSchedule, describeTimer, validDate, localDate, describeDate, formatCountdown, inputUnit,
  opText, parseInput, ruleRequest, scheduleRequest, switchTargets, timerRequest, validTime,
  type RuleForm,
} from '../src/model/automation';
import { nameProblem, sceneNameProblem } from '../src/model/names';
import { defaultEntities } from '../tools/mock-hub/hub';

const ents = defaultEntities(); // plug1 (256), lamp (257), motion sensor (258)
const opts = attrOptions(ents);
const opt = (label: string) => opts.find((o) => o.label === label)!;

describe('wording', () => {
  it('days', () => {
    expect(describeDays('daily')).toBe('Every day');
    expect(describeDays('weekdays')).toBe('Weekdays');
    expect(describeDays('weekends')).toBe('Weekends');
    expect(describeDays(['sun', 'wed', 'mon'])).toBe('Mon, Wed, Sun'); // week order, Monday first
  });
  it('countdowns', () => {
    expect(formatCountdown(0)).toBe('now');
    expect(formatCountdown(45)).toBe('45 s');
    expect(formatCountdown(600)).toBe('10 min');
    expect(formatCountdown(3600)).toBe('60 min');
    expect(formatCountdown(7200)).toBe('2 h');
    expect(formatCountdown(7200 + 25 * 60)).toBe('2 h 25 min');
  });
  it('comparisons read as English', () => {
    expect(opText('any')).toBe('changes');
    expect(opText('>')).toBe('is above');
    expect(opText('<=')).toBe('is at most');
  });
  it('schedules and timers name the device and the action', () => {
    const s: Schedule = { index: 0, time: '22:00', days: 'weekdays', entity: 256, attr: 1, action: 'off' };
    expect(describeSchedule(s, ents)).toEqual({ title: '22:00 · Off plug1', detail: 'Weekdays' });
    const t: Timer = { index: 1, entity: 257, attr: 1, action: 'toggle', left_s: 600 };
    expect(describeTimer(t, ents, 600)).toEqual({ title: 'Toggle lamp', detail: 'in 10 min' });
    // A device that no longer exists is still described, not crashed on.
    expect(describeSchedule({ ...s, entity: 999 }, ents).title).toBe('22:00 · Off device 999');
  });
  it('rules read as a sentence, with their extra conditions', () => {
    const r: Rule = {
      index: 0,
      when: { entity: 258, attr: 0x50, op: '==', value: true },
      then: { entity: 257, attr: 1, value: true },
      between: { from: '18:00', to: '23:00' },
      and: { entity: 256, attr: 0x13, op: '>', value: 5 },
      cooldown_s: 300,
    };
    expect(describeRule(r, ents)).toEqual({
      when: 'When Motion sensor 0x0102 presence is Occupied',
      then: 'turn lamp on',
      extras: ['between 18:00 and 23:00', 'only while plug1 power draw is above 5 W', 'at most once per 5 min'],
    });
    const any: Rule = { index: 1, when: { entity: 256, attr: 0x13, op: 'any' }, then: { entity: 257, attr: 2, value: 127 } };
    expect(describeRule(any, ents)).toEqual({ when: 'When plug1 power draw changes', then: 'set lamp brightness to 50 %', extras: [] });
  });
});

describe('what can be picked', () => {
  it('lists device attributes by name, writable ones for actions, and skips colour', () => {
    expect(opts.map((o) => o.label)).toContain('lamp · Brightness');
    const writable = attrOptions(ents, { writable: true });
    expect(writable.every((o) => o.writable)).toBe(true);
    expect(writable.map((o) => o.label)).not.toContain('plug1 · Power draw');
    const withColour: Entity = { id: 9, name: 'strip', chip: 1, device: 'light', attrs: [{ id: 3, name: 'color_xy', vt: 'xy', flags: ['r', 'w'], value: [1, 2] }] };
    expect(attrOptions([withColour])).toEqual([]);
  });
  it('schedules and timers can drive devices with an on/off switch', () => {
    expect(switchTargets(ents).map((e) => e.id)).toEqual([256, 257]);
  });
});

describe('parseInput', () => {
  const ok = (name: string, vt: Parameters<typeof parseInput>[1], text: string) => {
    const r = parseInput(name, vt, text);
    return r.ok ? r.value : `error: ${r.error}`;
  };
  it('booleans', () => {
    expect(ok('on_off', 'bool', 'on')).toBe(true);
    expect(ok('on_off', 'bool', ' OFF ')).toBe(false);
    expect(ok('on_off', 'bool', 'maybe')).toMatch(/error/);
  });
  it('plain numbers are range-checked per type', () => {
    expect(ok('power_w', 'u16', '1500')).toBe(1500);
    expect(ok('power_w', 'u16', '70000')).toMatch(/Out of range/);
    expect(ok('x', 'u8', '255')).toBe(255);
    expect(ok('x', 'u8', '256')).toMatch(/Out of range/);
    expect(ok('x', 'i16', '-5')).toBe(-5);
    expect(ok('x', 'u8', '-1')).toMatch(/Out of range/);
    expect(ok('x', 'u8', '1.5')).toMatch(/whole/);
    expect(ok('x', 'u8', '')).toMatch(/number/);
    expect(ok('x', 'u8', 'abc')).toMatch(/number/);
    expect(ok('x', 'f32', '1.5')).toBe(1.5);
  });
  it('brightness is typed in percent and sent as 0..254', () => {
    expect(inputUnit('brightness')).toBe('%');
    expect(ok('brightness', 'u8', '50')).toBe(127);
    expect(ok('brightness', 'u8', '100')).toBe(254);
    expect(ok('brightness', 'u8', '0')).toBe(0);
    expect(ok('brightness', 'u8', '150')).toMatch(/Out of range.*%/);
  });
  it('temperature is typed in °C and sent in tenths', () => {
    expect(ok('temperature', 'i16', '21.5')).toBe(215);
    expect(ok('temperature', 'i16', '-3')).toBe(-30);
    expect(ok('temperature', 'i16', '5000')).toMatch(/Out of range/);
  });
});

describe('request builders', () => {
  it('validates times', () => {
    expect(['00:00', '07:30', '23:59'].every(validTime)).toBe(true);
    expect(['7:30', '24:00', '12:60', '12:5', 'noon', ''].some(validTime)).toBe(false);
  });
  it('schedule', () => {
    expect(scheduleRequest({ time: '22:00', days: 'weekdays', entity: 256, action: 'off' })).toEqual({
      ok: true, value: { time: '22:00', days: 'weekdays', entity: 256, action: 'off' },
    });
    expect(scheduleRequest({ time: '2200', days: 'daily', entity: 256, action: 'off' }).ok).toBe(false);
    expect(scheduleRequest({ time: '22:00', days: [], entity: 256, action: 'off' })).toMatchObject({ error: 'Pick at least one day' });
    // a one-off sends a date and no days; "today" is allowed, yesterday is not; a scene works the same way
    expect(scheduleRequest({ time: '19:00', days: 'daily', date: '2026-10-20', today: '2026-10-12', entity: 256, action: 'on' }))
      .toEqual({ ok: true, value: { time: '19:00', date: '2026-10-20', entity: 256, action: 'on' } });
    expect(scheduleRequest({ time: '19:00', days: 'daily', date: '2026-10-12', today: '2026-10-12', entity: null, action: 'on', scene: 'night' }))
      .toEqual({ ok: true, value: { time: '19:00', date: '2026-10-12', scene: 'night' } });
    expect(scheduleRequest({ time: '19:00', days: 'daily', date: '2026-10-11', today: '2026-10-12', entity: 256, action: 'on' }))
      .toMatchObject({ ok: false, error: 'That date has already passed' });
    expect(scheduleRequest({ time: '19:00', days: 'daily', date: '2026-02-30', today: '2026-01-01', entity: 256, action: 'on' }))
      .toMatchObject({ ok: false, error: 'Date must look like 2026-10-20' });
    expect(validDate('2028-02-29')).toBe(true);
    expect(validDate('2026-02-29')).toBe(false);
    expect(validDate('1999-12-31')).toBe(false);
    expect(validDate('2026-1-5')).toBe(false);
    expect(localDate(new Date(2026, 9, 5, 23, 59))).toBe('2026-10-05');
    expect(describeDate('2026-10-05')).toBe('5 Oct 2026');
    expect(describeDate('garbage')).toBe('garbage');
    expect(describeSchedule({ index: 0, time: '19:00', days: 'once', date: '2026-10-20', entity: 256, attr: 1, action: 'on' }, ents))
      .toEqual({ title: '19:00 · On plug1', detail: 'Once, 20 Oct 2026' });
    expect(scheduleRequest({ time: '22:00', days: 'daily', entity: null, action: 'off' })).toMatchObject({ error: 'Pick a device' });
  });
  it('timer converts minutes to seconds and bounds them', () => {
    expect(timerRequest({ entity: 257, action: 'on', minutes: '10' })).toEqual({ ok: true, value: { entity: 257, action: 'on', delay_s: 600 } });
    expect(timerRequest({ entity: 257, action: 'on', minutes: '0.5' })).toMatchObject({ ok: true, value: { delay_s: 30 } });
    expect(timerRequest({ entity: 257, action: 'on', minutes: '0' }).ok).toBe(false);
    expect(timerRequest({ entity: 257, action: 'on', minutes: '-3' }).ok).toBe(false);
    expect(timerRequest({ entity: 257, action: 'on', minutes: 'soon' }).ok).toBe(false);
    expect(timerRequest({ entity: 257, action: 'on', minutes: String(7 * 24 * 60 + 1) })).toMatchObject({ error: 'At most 7 days' });
    expect(timerRequest({ entity: null, action: 'on', minutes: '5' })).toMatchObject({ error: 'Pick a device' });
  });

  const base = (): RuleForm => ({
    when: { option: opt('Motion sensor 0x0102 · Presence'), op: '==', value: 'on' },
    then: { option: opt('lamp · Power'), value: 'on' },
    between: null,
    and: null,
    cooldown: '',
  });
  it('a plain rule', () => {
    expect(ruleRequest(base())).toEqual({
      ok: true,
      value: { when: { entity: 258, attr: 0x50, op: '==', value: true }, then: { entity: 257, attr: 1, value: true } },
    });
  });
  it('a rule with every option', () => {
    const f = base();
    f.between = { from: '18:00', to: '23:00' };
    f.and = { option: opt('plug1 · Power draw'), op: '>', value: '5' };
    f.cooldown = '300';
    expect(ruleRequest(f)).toEqual({
      ok: true,
      value: {
        when: { entity: 258, attr: 0x50, op: '==', value: true },
        then: { entity: 257, attr: 1, value: true },
        between: { from: '18:00', to: '23:00' },
        and: { entity: 256, attr: 0x13, op: '>', value: 5 },
        cooldown_s: 300,
      },
    });
  });
  it('"changes" needs no value; scaled values are converted', () => {
    const f = base();
    f.when = { option: opt('plug1 · Power draw'), op: 'any', value: '' };
    f.then = { option: opt('lamp · Brightness'), value: '50' };
    expect(ruleRequest(f)).toMatchObject({ ok: true, value: { when: { op: 'any' }, then: { value: 127 } } });
  });
  it('says which part is wrong', () => {
    const bad = (mutate: (f: RuleForm) => void) => {
      const f = base();
      mutate(f);
      const r = ruleRequest(f);
      return r.ok ? 'ok' : r.error;
    };
    expect(bad((f) => (f.when.option = null))).toBe('When: pick what to look at');
    expect(bad((f) => (f.when.value = 'x'))).toBe('When: Choose on or off');
    expect(bad((f) => (f.then.option = null))).toBe('Then: pick what to set');
    expect(bad((f) => (f.then.option = opt('lamp · Brightness'))).replace(/\d+/g, 'N')).toContain('Then:');
    expect(bad((f) => (f.between = { from: '10:00', to: '10:00' }))).toMatch(/start and end must differ/);
    expect(bad((f) => (f.between = { from: '1000', to: '11:00' }))).toMatch(/times must look like/);
    expect(bad((f) => (f.and = { option: opt('plug1 · Power draw'), op: 'any', value: '' }))).toBe('Only while: pick a comparison');
    expect(bad((f) => (f.and = { option: null, op: '>', value: '1' }))).toBe('Only while: pick what to look at');
    expect(bad((f) => (f.cooldown = '0'))).toMatch(/At most once per/);
    expect(bad((f) => (f.cooldown = 'x'))).toMatch(/At most once per/);
    expect(bad((f) => (f.cooldown = '1.5'))).toMatch(/At most once per/); // seconds are whole
    expect(bad((f) => (f.cooldown = '70000'))).toMatch(/At most once per/); // beyond 65535 s
  });
});

describe('names', () => {
  it('a device name a person can read', () => {
    for (const n of ['lamp', 'Living room', 'Kitchen lamp', 'Lamp (main)', 'Кухня розетка', 'abcdefghijklmno']) {
      expect(nameProblem(n)).toBeNull();
    }
    for (const n of ['', ' lead', 'trail ', '0x10', '123', 'bad/name', 'a'.repeat(16), 'Кухня розетка длинная']) {
      expect(nameProblem(n)).not.toBeNull();
    }
  });

  it('a scene name stays machine-like', () => {
    for (const n of ['lamp', 'kitchen_2', 'a'.repeat(23)]) expect(sceneNameProblem(n)).toBeNull();
    for (const n of ['', 'Lamp', 'my lamp', 'a'.repeat(24)]) expect(sceneNameProblem(n)).not.toBeNull();
  });
});

describe('scene targets', () => {
  it('schedules, timers and rules that apply a scene read like sentences', () => {
    const sched: Schedule = { index: 0, time: '22:00', days: 'weekdays', scene: 'night' };
    expect(describeSchedule(sched, ents)).toEqual({ title: '22:00 · Scene night', detail: 'Weekdays' });
    const timer: Timer = { index: 0, scene: 'night', left_s: 600 };
    expect(describeTimer(timer, ents, 600)).toEqual({ title: 'Scene night', detail: 'in 10 min' });
    const rule: Rule = { index: 0, when: { entity: 258, attr: 0x50, op: 'any' }, then: { scene: 'night' } };
    expect(describeRule(rule, ents)).toMatchObject({ when: 'When Motion sensor 0x0102 presence changes', then: 'apply scene night' });
    expect(describeRule(rule, ents).when).toBe('When Motion sensor 0x0102 presence changes');
  });

  it('the request builders send {scene} instead of a device and action', () => {
    expect(scheduleRequest({ time: '22:00', days: 'daily', entity: null, action: 'off', scene: 'night' }))
      .toEqual({ ok: true, value: { time: '22:00', days: 'daily', scene: 'night' } });
    expect(scheduleRequest({ time: '22:00', days: 'daily', entity: null, action: 'off', scene: '' }))
      .toMatchObject({ ok: false, error: 'Pick a device' }); // an empty scene is "nothing chosen"
    expect(timerRequest({ entity: null, action: 'on', minutes: '5', scene: 'night' }))
      .toEqual({ ok: true, value: { scene: 'night', delay_s: 300 } });
    expect(timerRequest({ entity: null, action: 'on', minutes: 'x', scene: 'night' })).toMatchObject({ ok: false });
    const when = { option: opt('Motion sensor 0x0102 · Presence'), op: 'any' as const, value: '' };
    expect(ruleRequest({ when, then: { option: null, value: '', scene: 'night' }, between: null, and: null, cooldown: '' }))
      .toEqual({ ok: true, value: { when: { entity: 258, attr: 0x50, op: 'any' }, then: { scene: 'night' } } });
    expect(ruleRequest({ when, then: { option: null, value: '', scene: null }, between: null, and: null, cooldown: '' }))
      .toMatchObject({ ok: false });
  });
});

describe('scenes', () => {
  it('captureScene stores the writable settings as they are, skipping read-only and unknown values', () => {
    expect(captureScene(ents, [0x0100])).toEqual([{ entity: 0x0100, attr: 1, value: true }]); // power/energy are read-only
    expect(captureScene(ents, [0x0101])).toEqual([
      { entity: 0x0101, attr: 1, value: false },
      { entity: 0x0101, attr: 2, value: 128 },
    ]);
    expect(captureScene(ents, [0x0102])).toEqual([]); // a sensor has nothing to set
    expect(captureScene(ents, [0x9999])).toEqual([]);
    const unknown: Entity[] = [{ id: 7, name: null, chip: 1, device: 'light', attrs: [{ id: 1, name: 'on_off', vt: 'bool', flags: ['r', 'w'], value: null }] }];
    expect(captureScene(unknown, [7])).toEqual([]); // never saves a value the hub does not know
  });

  it('describeScene reads like a sentence, even for a device that is gone', () => {
    const sc = { name: 'evening', actions: [
      { entity: 0x0100, attr: 1, value: false },
      { entity: 0x0101, attr: 2, value: 254 },
      { entity: 0x7777, attr: 1, value: true },
    ] };
    expect(describeScene(sc, ents)).toEqual({ title: 'evening', detail: 'plug1 off · lamp brightness 100 % · device 30583 attribute 1 true' });
  });

  it('sceneRequest checks the name and the number of actions', () => {
    const a = { entity: 1, attr: 1, value: true as const };
    expect(sceneRequest('evening', [a])).toEqual({ ok: true, value: { name: 'evening', actions: [a] } });
    expect(sceneRequest('Evening', [a])).toMatchObject({ ok: false });
    expect(sceneRequest('evening', [])).toMatchObject({ ok: false, error: expect.stringMatching(/at least one/) });
    expect(sceneRequest('evening', Array(9).fill(a))).toMatchObject({ ok: false, error: expect.stringMatching(/at most 8/) });
    expect(sceneRequest('evening', Array(8).fill(a))).toMatchObject({ ok: true });
  });
});

describe('sun-relative times', () => {
  it('validTimePoint accepts clock times and sunrise/sunset with an offset up to 180 minutes', () => {
    for (const ok of ['00:00', '23:59', 'sunrise', 'sunset', 'sunset+30', 'sunrise-15', 'sunset+180', 'sunrise-180', 'sunset+0']) {
      expect(validTimePoint(ok), ok).toBe(true);
    }
    for (const bad of ['24:00', '7:30', 'sunset+181', 'sunrise-999', 'sunset+', 'Sunset', 'noon', 'sunset 30', 'sunset+1x', '', 'sunrise+-5']) {
      expect(validTimePoint(bad), bad).toBe(false);
    }
    expect(isSunPoint('sunset+30')).toBe(true);
    expect(isSunPoint('22:00')).toBe(false);
    expect(isSunPoint('noon')).toBe(false);
  });

  it('describes them in words', () => {
    expect(describeTimePoint('sunset')).toBe('at sunset');
    expect(describeTimePoint('sunset+0')).toBe('at sunset');
    expect(describeTimePoint('sunset+30')).toBe('30 min after sunset');
    expect(describeTimePoint('sunrise-15')).toBe('15 min before sunrise');
    expect(describeTimePoint('22:00')).toBe('22:00');
  });

  it('a schedule at the sun reads in words, and says it is waiting while the hub has no location', () => {
    const s: Schedule = { index: 0, time: 'sunset+30', days: 'daily', entity: 256, attr: 1, action: 'on' };
    expect(describeSchedule(s, ents)).toEqual({ title: '30 min after sunset · On plug1', detail: 'Every day' });
    expect(describeSchedule(s, ents, { hasLocation: true }).detail).toBe('Every day');
    expect(describeSchedule(s, ents, { hasLocation: false }).detail).toBe("Every day · waiting for the hub's location");
    const fixed: Schedule = { ...s, time: '22:00' };
    expect(describeSchedule(fixed, ents, { hasLocation: false }).detail).toBe('Every day'); // a clock time never waits
  });

  it('rule windows at the sun read in words; the request builders pass them on', () => {
    const rule: Rule = { index: 0, when: { entity: 258, attr: 0x50, op: 'any' }, then: { entity: 257, attr: 1, value: true }, between: { from: 'sunset', to: '23:00' } };
    expect(describeRule(rule, ents).extras).toEqual(['between at sunset and 23:00']);
    expect(scheduleRequest({ time: 'sunset+30', days: 'daily', entity: 256, action: 'on' }))
      .toEqual({ ok: true, value: { time: 'sunset+30', days: 'daily', entity: 256, action: 'on' } });
    expect(scheduleRequest({ time: 'dusk', days: 'daily', entity: 256, action: 'on' })).toMatchObject({ ok: false });
    const when = { option: opt('Motion sensor 0x0102 · Presence'), op: 'any' as const, value: '' };
    const then = { option: opt('lamp · Power'), value: 'on' };
    const r = ruleRequest({ when, then, between: { from: 'sunset+30', to: 'sunrise-15' }, and: null, cooldown: '' });
    expect(r).toMatchObject({ ok: true });
    expect((r as { value: Record<string, unknown> }).value.between).toEqual({ from: 'sunset+30', to: 'sunrise-15' });
    expect(ruleRequest({ when, then, between: { from: 'sunset+500', to: '23:00' }, and: null, cooldown: '' })).toMatchObject({ ok: false });
  });
});

describe('location', () => {
  it('locationRequest accepts numbers in range (a comma decimal too) and explains the rest', () => {
    expect(locationRequest('50.08', '14.43')).toEqual({ ok: true, value: { lat: 50.08, lon: 14.43 } });
    expect(locationRequest(' 50,08 ', '-14,43')).toEqual({ ok: true, value: { lat: 50.08, lon: -14.43 } });
    expect(locationRequest('90', '180')).toMatchObject({ ok: true });
    expect(locationRequest('-90', '-180')).toMatchObject({ ok: true });
    for (const [la, lo] of [['', '1'], ['1', ''], ['abc', '1'], ['91', '0'], ['-91', '0'], ['0', '181'], ['0', '-181'], ['NaN', '1'], ['Infinity', '1']]) {
      expect(locationRequest(la!, lo!), `${la},${lo}`).toMatchObject({ ok: false });
    }
  });

  it('describeLocation', () => {
    expect(describeLocation(null)).toMatch(/Not set/);
    expect(describeLocation({ location: null })).toMatch(/Not set/);
    expect(describeLocation({ location: { lat: 50.08, lon: 14.43 }, sun: { rise: '06:12', set: '20:01' } })).toBe('50.0800, 14.4300 · sunrise 06:12, sunset 20:01');
    expect(describeLocation({ location: { lat: 50.08, lon: 14.43 }, sun: null, synced: false })).toMatch(/waiting for the clock/);
    expect(describeLocation({ location: { lat: 69.6, lon: 18.9 }, sun: null, synced: true })).toMatch(/does not rise and set today/);
  });
});

describe('wifi', () => {
  it('wifiRequest applies the hub\'s rules so a typo is caught before the hub leaves its network', () => {
    expect(wifiRequest('home', 'correct horse')).toEqual({ ok: true, value: { ssid: 'home', password: 'correct horse' } });
    expect(wifiRequest('cafe', '')).toMatchObject({ ok: true });                       // open network
    expect(wifiRequest('home', '12345678')).toMatchObject({ ok: true });
    expect(wifiRequest('home', 'a'.repeat(63))).toMatchObject({ ok: true });
    expect(wifiRequest('home', 'a'.repeat(64))).toMatchObject({ ok: true });           // raw PSK (hex digits)
    expect(wifiRequest('кухня', 'пароль-ok')).toMatchObject({ ok: true });             // UTF-8: bytes, not letters
    expect(wifiRequest('s'.repeat(32), '')).toMatchObject({ ok: true });
    for (const [s, p] of [['', '12345678'], ['s'.repeat(33), ''], ['я'.repeat(17), ''], ['a\nb', '12345678'], ['home', 'short'],
                          ['home', 'x'.repeat(65)], ['home', 'z'.repeat(64)], ['home', 'tab\there1']]) {
      expect(wifiRequest(s!, p!), `${s}/${p}`).toMatchObject({ ok: false });
    }
  });

  it('describes the connection and the signal', () => {
    expect(describeWifi(null)).toBe('Not connected');
    expect(describeWifi({ wifi: null })).toBe('Not connected');
    expect(describeWifi({ wifi: { ssid: 'home', rssi: -61, ip: '192.168.0.202' } })).toBe('home · -61 dBm · 192.168.0.202');
    expect([-40, -55, -56, -70, -71, -82, -83, -95].map(signalText)).toEqual(['strong', 'strong', 'good', 'good', 'weak', 'weak', 'very weak', 'very weak']);
  });
});

describe('editing a scene', () => {
  const scene = { name: 'evening', actions: [
    { entity: 0x0100, attr: 1, value: true },      // plug1 on
    { entity: 0x0101, attr: 2, value: 254 },       // lamp brightness 100 %
    { entity: 0x7777, attr: 1, value: true },      // a device that is gone
    { entity: 0x0100, attr: 0x13, value: 5 },      // power_w: read-only, cannot be set
  ] };

  it('inputText is the inverse of parseInput, in the units a person types', () => {
    expect(inputText('on_off', 'bool', true)).toBe('on');
    expect(inputText('on_off', 'bool', false)).toBe('off');
    expect(inputText('brightness', 'u8', 254)).toBe('100');
    expect(inputText('brightness', 'u8', 127)).toBe('50');
    expect(inputText('temperature', 'i16', 215)).toBe('21.5');
    expect(inputText('something', 'u8', 7)).toBe('7');
    for (const [name, vt, v] of [['brightness', 'u8', 76], ['on_off', 'bool', true], ['temperature', 'i16', -30]] as const) {
      const text = inputText(name, vt, v);
      expect(parseInput(name, vt, text)).toEqual({ ok: true, value: v });
    }
  });

  it('sceneDrafts: what can be edited, and what is gone', () => {
    const { drafts, gone } = sceneDrafts(scene, ents);
    expect(drafts.map((d) => [d.option.label, d.value])).toEqual([['plug1 · Power', 'on'], ['lamp · Brightness', '100']]);
    expect(gone.map((a) => a.entity)).toEqual([0x7777, 0x0100]);
  });

  it('addableSettings leaves out what the scene already sets, and read-only attributes', () => {
    const { drafts } = sceneDrafts(scene, ents);
    const labels = addableSettings(ents, drafts).map((o) => o.label);
    expect(labels).toContain('lamp · Power');
    expect(labels).not.toContain('lamp · Brightness');
    expect(labels).not.toContain('plug1 · Power');
    expect(labels.some((l) => l.includes('Presence'))).toBe(false); // a sensor: nothing writable
  });

  it('draftsRequest parses every value and says which setting is wrong', () => {
    const { drafts } = sceneDrafts(scene, ents);
    expect(draftsRequest('evening', drafts)).toEqual({ ok: true, value: { name: 'evening', actions: [
      { entity: 0x0100, attr: 1, value: true }, { entity: 0x0101, attr: 2, value: 254 } ] } });
    const bad = drafts.map((d) => (d.option.name === 'brightness' ? { ...d, value: '250' } : d));
    expect(draftsRequest('evening', bad)).toMatchObject({ ok: false, error: expect.stringContaining('lamp · Brightness') });
    expect(draftsRequest('evening', [])).toMatchObject({ ok: false, error: expect.stringMatching(/at least one/) });
    expect(draftsRequest('Bad Name', drafts)).toMatchObject({ ok: false });
    const blank = drafts.map((d) => ({ ...d, value: '' }));
    expect(draftsRequest('evening', blank)).toMatchObject({ ok: false });
  });

  it('a setting the person did not touch keeps its exact value, though percent cannot show every step', () => {
    const odd = { name: 'x', actions: [{ entity: 0x0101, attr: 2, value: 77 }] };   // 77/254 = 30.3 %
    const { drafts } = sceneDrafts(odd, ents);
    expect(drafts[0]!.value).toBe('30');
    expect(draftsRequest('x', drafts)).toMatchObject({ ok: true, value: { actions: [{ value: 77 }] } });         // untouched: 77, not 76
    expect(draftsRequest('x', [{ ...drafts[0]!, value: '31' }])).toMatchObject({ ok: true, value: { actions: [{ value: 79 }] } }); // changed: parsed
    expect(draftsRequest('x', [{ ...drafts[0]!, value: '30 ' }])).toMatchObject({ ok: true, value: { actions: [{ value: 76 }] } }); // retyped (even the same digits): parsed
  });

  it('draftsFromCurrentState reads the devices now and leaves unknown ones as they were', () => {
    const { drafts } = sceneDrafts(scene, ents);
    const now = draftsFromCurrentState(drafts, ents);
    expect(now.map((d) => d.value)).toEqual(['on', '50']);   // mock home: plug1 on, lamp at 128/254
    const unknown: Entity[] = ents.map((e) => ({ ...e, attrs: e.attrs.map((a) => ({ ...a, value: null })) }));
    expect(draftsFromCurrentState(drafts, unknown).map((d) => d.value)).toEqual(['on', '100']);
  });
});
