import { describe, expect, it } from 'vitest';
import type { Entity, Rule, Schedule, Timer } from '../src/protocol/messages';
import {
  attrOptions, captureScene, describeScene, sceneRequest, describeDays, describeRule, describeSchedule, describeTimer, formatCountdown, inputUnit,
  nameProblem, opText, parseInput, ruleRequest, scheduleRequest, switchTargets, timerRequest, validTime,
  type RuleForm,
} from '../src/model/automation';
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
    f.cooldown = '5';
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
    expect(bad((f) => (f.cooldown = '2000'))).toMatch(/At most once per/); // beyond 65535 s
  });
});

describe('names', () => {
  it('match the hub rule', () => {
    for (const n of ['lamp', 'kitchen_2', 'a'.repeat(23)]) expect(nameProblem(n)).toBeNull();
    for (const n of ['', 'Lamp', '1lamp', '_lamp', 'my lamp', '0x10', 'a'.repeat(24)]) expect(nameProblem(n)).not.toBeNull();
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
