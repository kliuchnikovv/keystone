import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import { HubClient, HubError, type HubClientOptions, type SocketLike } from '../src/protocol/client';
import { attrValue, type HomeState } from '../src/protocol/store';
import { MockHub, MAX_CLIENTS } from '../tools/mock-hub/hub';

const wsFactory = (url: string) => new NodeWebSocket(url) as unknown as SocketLike;

let hub: MockHub;
const clients: HubClient[] = [];

function client(over: Partial<HubClientOptions> = {}): HubClient {
  const c = new HubClient({
    url: hub.url,
    token: hub.token,
    socketFactory: wsFactory,
    backoffMs: { min: 20, max: 60 },
    requestTimeoutMs: 2000,
    ...over,
  });
  clients.push(c);
  return c;
}

async function until(fn: () => boolean, what: string, ms = 3000): Promise<void> {
  const t0 = Date.now();
  while (!fn()) {
    if (Date.now() - t0 > ms) throw new Error(`timed out waiting for: ${what}`);
    await new Promise((r) => setTimeout(r, 10));
  }
}
const ready = (c: HubClient) => until(() => c.getSnapshot().status === 'ready', 'status ready');

beforeEach(async () => {
  hub = await MockHub.start({ helloTimeoutMs: 300, ackDelayMs: 10, lockoutMs: 500 });
});
afterEach(async () => {
  for (const c of clients.splice(0)) c.close();
  await hub.close();
});

describe('handshake', () => {
  it('connects, authenticates and receives the snapshot', async () => {
    const c = client();
    const seen: string[] = [];
    c.subscribe(() => seen.push(c.getSnapshot().status));
    c.connect();
    await ready(c);
    const s = c.getSnapshot();
    expect(s.hub).toMatchObject({ name: 'mock-home', synced: true });
    expect(s.entities.map((e) => e.name)).toEqual(['plug1', 'lamp', null]);
    expect(seen).toContain('authenticating');
  });

  it('works with the platform global WebSocket (what React Native / browsers provide)', async () => {
    const c = client({ socketFactory: undefined });
    c.connect();
    await ready(c);
    expect(c.getSnapshot().entities).toHaveLength(3);
  });

  it('a wrong token is fatal: status unauthorized and no reconnect loop', async () => {
    const c = client({ token: 'nope' });
    c.connect();
    await until(() => c.getSnapshot().status === 'unauthorized', 'unauthorized');
    expect(c.getSnapshot().error).toBe('unauthorized');
    await new Promise((r) => setTimeout(r, 200));
    expect(c.getSnapshot().status).toBe('unauthorized');
    expect(hub.clientCount).toBe(0);
  });

  it('the hub locks an address out after repeated failures, then lets it back in', async () => {
    for (let i = 0; i < 5; i++) {
      const bad = client({ token: 'nope' });
      bad.connect();
      await until(() => bad.getSnapshot().status === 'unauthorized', `bad attempt ${i}`);
    }
    const locked = client(); // correct token, but the address is locked out
    locked.connect();
    await until(() => locked.getSnapshot().status === 'unauthorized', 'locked out');
    await new Promise((r) => setTimeout(r, 600)); // lockoutMs = 500
    const ok = client();
    ok.connect();
    await ready(ok);
  });

  it('a protocol version the hub does not speak is reported as incompatible', async () => {
    const sock = new NodeWebSocket(hub.url);
    const reply = await new Promise<string>((resolve) => {
      sock.on('open', () => sock.send(JSON.stringify({ type: 'hello', v: 99, token: hub.token })));
      sock.on('message', (d) => resolve(d.toString()));
    });
    expect(JSON.parse(reply)).toEqual({ type: 'error', code: 'version' });
  });

  it('the hub drops a connection that never says hello', async () => {
    const sock = new NodeWebSocket(hub.url);
    const msgs: string[] = [];
    sock.on('message', (d) => msgs.push(d.toString()));
    await new Promise<void>((resolve) => sock.on('close', () => resolve()));
    expect(JSON.parse(msgs[0]!)).toMatchObject({ type: 'error', code: 'unauthorized' });
  });

  it('the 4th client gets "busy" and keeps retrying; it gets in once a slot frees', async () => {
    const first = Array.from({ length: MAX_CLIENTS }, () => client());
    first.forEach((c) => c.connect());
    await Promise.all(first.map(ready));
    const extra = client();
    extra.connect();
    await until(() => extra.getSnapshot().error === 'busy', 'busy error');
    expect(extra.getSnapshot().status).not.toBe('ready');
    first[0]!.close();
    await ready(extra);
    expect(extra.getSnapshot().error).toBeNull();
  });
});

describe('split snapshot', () => {
  it('a snapshot in several parts is assembled and only then ready', async () => {
    await hub.close();
    hub = await MockHub.start({ helloTimeoutMs: 300, ackDelayMs: 10, snapshotChunk: 7 });
    hub.addEntities(30); // 3 default + 30 = 33 entities -> 5 frames of 7
    const c = client({ url: hub.url });
    const statuses: string[] = [];
    const sizes: number[] = [];
    c.subscribe(() => {
      statuses.push(c.getSnapshot().status);
      sizes.push(c.getSnapshot().entities.length);
    });
    c.connect();
    await ready(c);
    const s = c.getSnapshot();
    expect(s.entities).toHaveLength(33);
    expect(new Set(s.entities.map((e) => e.id)).size).toBe(33);
    expect(s.chips).toHaveLength(2);
    // Never 'ready' while only part of the home was known.
    const firstReady = statuses.indexOf('ready');
    expect(sizes[firstReady]).toBe(33);
    expect(sizes.some((n) => n > 0 && n < 33)).toBe(true);
  });

  it('a resync replaces the lists instead of duplicating them', async () => {
    await hub.close();
    hub = await MockHub.start({ helloTimeoutMs: 300, ackDelayMs: 10, snapshotChunk: 5 });
    hub.addEntities(12);
    const c = client({ url: hub.url });
    c.connect();
    await ready(c);
    hub.setValue(0x0200, 0x01, true);
    await until(() => attrValue(c.getSnapshot(), 0x0200, 0x01) === true, 'value before resync');
    hub.resync();
    await new Promise((r) => setTimeout(r, 150));
    const s = c.getSnapshot();
    expect(s.entities).toHaveLength(15);
    expect(s.chips).toHaveLength(2);
    expect(s.status).toBe('ready');
    expect(attrValue(s, 0x0200, 0x01)).toBe(true);
  });
});

describe('events', () => {
  it('a device-side change reaches every connected client', async () => {
    const a = client();
    const b = client();
    a.connect();
    b.connect();
    await Promise.all([ready(a), ready(b)]);
    hub.setValue(0x0100, 0x13, 25);
    await until(() => attrValue(a.getSnapshot(), 0x0100, 0x13) === 25 && attrValue(b.getSnapshot(), 0x0100, 0x13) === 25, 'power update');
  });

  it('subscribers are notified once per change and can unsubscribe', async () => {
    const c = client();
    c.connect();
    await ready(c);
    let n = 0;
    const off = c.subscribe(() => n++);
    hub.setValue(0x0100, 0x13, 11);
    await until(() => n === 1, 'one notification');
    hub.setValue(0x0100, 0x13, 11); // same value: no event at all
    off();
    hub.setValue(0x0100, 0x13, 12);
    await until(() => attrValue(c.getSnapshot(), 0x0100, 0x13) === 12, 'state after unsubscribe');
    expect(n).toBe(1);
  });

  it('a chip going silent is pushed', async () => {
    const c = client();
    c.connect();
    await ready(c);
    hub.setChipStatus(1, 'silent');
    await until(() => c.getSnapshot().chips.find((x) => x.id === 1)?.status === 'silent', 'chip silent');
  });
});

describe('commands', () => {
  it('cmd resolves with the chip ACK and the new value arrives as an event', async () => {
    const c = client();
    c.connect();
    await ready(c);
    expect(await c.cmd(0x0101, 0x01, true)).toBe('ok');
    await until(() => attrValue(c.getSnapshot(), 0x0101, 0x01) === true, 'lamp on');
  });

  it('a command to a silent chip completes as unreachable and changes nothing', async () => {
    const c = client();
    c.connect();
    await ready(c);
    hub.setChipStatus(1, 'silent');
    expect(await c.cmd(0x0101, 0x01, true)).toBe('unreachable');
    expect(attrValue(c.getSnapshot(), 0x0101, 0x01)).toBe(false);
  });

  it.each([
    ['unknown entity', 0x9999, 0x01, true, 'not_found'],
    ['unknown attribute', 0x0100, 0x77, true, 'not_found'],
    ['read-only attribute', 0x0100, 0x13, 5, 'read_only'],
    ['wrong value type', 0x0100, 0x01, 'yes', 'bad_args'],
    ['out of range', 0x0101, 0x02, 300, 'bad_args'],
  ] as const)('rejects %s', async (_name, entity, attr, value, code) => {
    const c = client();
    c.connect();
    await ready(c);
    await expect(c.cmd(entity, attr, value as never)).rejects.toMatchObject({ code });
  });

  it('several commands in flight are matched to their own replies', async () => {
    const c = client();
    c.connect();
    await ready(c);
    const [a, b, d] = await Promise.all([
      c.cmd(0x0101, 0x01, true),
      c.cmd(0x0101, 0x02, 10),
      c.cmd(0x0100, 0x13, 1).catch((e: HubError) => e.code),
    ]);
    expect([a, b, d]).toEqual(['ok', 'ok', 'read_only']);
  });

  it('cmd while not connected fails fast', async () => {
    const c = client();
    await expect(c.cmd(0x0100, 0x01, true)).rejects.toMatchObject({ code: 'disconnected' });
  });

  it('a request times out when the hub never answers', async () => {
    const c = client({ requestTimeoutMs: 80 });
    c.connect();
    await ready(c);
    hub.opts.ackDelayMs = 10_000; // the ACK will not come in time
    await expect(c.cmd(0x0101, 0x01, true)).rejects.toMatchObject({ code: 'timeout' });
  });
});

describe('management requests', () => {
  it('rename, then the renamed entity is pushed to other clients', async () => {
    const a = client();
    const b = client();
    a.connect();
    b.connect();
    await Promise.all([ready(a), ready(b)]);
    await a.req('name.set', { entity: 0x0102, name: 'hall_sensor' });
    await until(() => b.getSnapshot().entities.find((e) => e.id === 0x0102)?.name === 'hall_sensor', 'rename pushed');
    await expect(a.req('name.set', { entity: 0x0101, name: 'hall_sensor' })).rejects.toMatchObject({ code: 'bad_args' });
    // Names start with a letter and use a-z 0-9 _ (at most 23 characters), so they can
    // never be mistaken for a numeric entity id.
    for (const name of ['Bad Name', '1abc', '_abc', '0x10', '', 'a'.repeat(24)]) {
      await expect(a.req('name.set', { entity: 0x0101, name })).rejects.toMatchObject({ code: 'bad_args' });
    }
    await a.req('name.set', { entity: 0x0101, name: 'a'.repeat(23) }); // the longest allowed
    await a.req('name.set', { entity: 0x0101, name: 'lamp' });
    await a.req('name.clear', { entity: 0x0102 });
    await until(() => b.getSnapshot().entities.find((e) => e.id === 0x0102)?.name === null, 'name cleared');
  });

  it('rules: add with all constraints, validate, delete and re-index', async () => {
    const c = client();
    c.connect();
    await ready(c);
    const rule = {
      when: { entity: 0x0102, attr: 0x50, op: '==', value: true },
      then: { entity: 0x0101, attr: 0x01, value: true },
      between: { from: '18:00', to: '23:00' },
      and: { entity: 0x0100, attr: 0x13, op: '>', value: 1 },
      cooldown_s: 300,
    };
    expect(await c.req('rule.add', rule)).toEqual({ index: 0 });
    expect(await c.req('rule.add', { ...rule, cooldown_s: undefined })).toEqual({ index: 1 });
    await until(() => c.getSnapshot().rules.length === 2, 'two rules');
    expect(c.getSnapshot().rules[0]).toMatchObject({ between: { from: '18:00', to: '23:00' }, cooldown_s: 300 });

    for (const bad of [
      { ...rule, between: { from: '10:00', to: '10:00' } },
      { ...rule, between: { from: '25:00', to: '10:00' } },
      { ...rule, cooldown_s: 0 },
      { ...rule, when: { ...rule.when, op: '=~' } },
      { when: rule.when },
    ]) {
      await expect(c.req('rule.add', bad)).rejects.toMatchObject({ code: 'bad_args' });
    }
    // Unknown targets and read-only actions have their own codes.
    await expect(c.req('rule.add', { ...rule, and: { ...rule.and, entity: 0x9999 } })).rejects.toMatchObject({ code: 'not_found' });
    await expect(c.req('rule.add', { ...rule, then: { entity: 0x0100, attr: 0x13, value: 5 } })).rejects.toMatchObject({ code: 'read_only' });
    await expect(c.req('rule.add', { ...rule, then: { entity: 0x0101, attr: 0x01, value: 'on' } })).rejects.toMatchObject({ code: 'bad_args' });

    await c.req('rule.del', { index: 0 });
    await until(() => c.getSnapshot().rules.length === 1, 'one rule left');
    expect(c.getSnapshot().rules[0]!.index).toBe(0);
    await expect(c.req('rule.del', { index: 5 })).rejects.toMatchObject({ code: 'not_found' });
  });

  it('rule table has a limit', async () => {
    const c = client();
    c.connect();
    await ready(c);
    const rule = { when: { entity: 0x0102, attr: 0x50, op: 'any' }, then: { entity: 0x0101, attr: 0x01, value: true } };
    for (let i = 0; i < 16; i++) await c.req('rule.add', rule);
    await expect(c.req('rule.add', rule)).rejects.toMatchObject({ code: 'table_full' });
  });

  it('schedules: a one-off needs a real date that is not past, and excludes days', async () => {
    const c = client();
    c.connect();
    await ready(c);
    expect(await c.req('schedule.add', { time: '19:00', date: '2099-10-20', entity: 0x0100, action: 'on' })).toEqual({ index: 0 });
    await until(() => c.getSnapshot().schedules.length === 1, 'the one-off');
    expect(c.getSnapshot().schedules[0]).toMatchObject({ time: '19:00', days: 'once', date: '2099-10-20' });
    for (const bad of [
      { time: '19:00', date: '2001-01-01', entity: 0x0100, action: 'on' }, // past
      { time: '19:00', date: '2099-02-30', entity: 0x0100, action: 'on' }, // not a date
      { time: '19:00', date: 'tomorrow', entity: 0x0100, action: 'on' },
      { time: '19:00', date: '2099-10-20', days: 'daily', entity: 0x0100, action: 'on' }, // both
    ]) {
      await expect(c.req('schedule.add', bad)).rejects.toMatchObject({ code: 'bad_args' });
    }
  });

  it('schedules: add, validate, delete', async () => {
    const c = client();
    c.connect();
    await ready(c);
    expect(await c.req('schedule.add', { time: '22:00', days: 'weekdays', entity: 0x0100, action: 'off' })).toEqual({ index: 0 });
    expect(await c.req('schedule.add', { time: '06:30', days: ['mon', 'fri'], entity: 0x0101, action: 'toggle' })).toEqual({ index: 1 });
    await until(() => c.getSnapshot().schedules.length === 2, 'two schedules');
    for (const bad of [
      { time: '24:00', days: 'daily', entity: 0x0100, action: 'off' },
      { time: '22:00', days: 'someday', entity: 0x0100, action: 'off' },
      { time: '22:00', days: [], entity: 0x0100, action: 'off' },
      { time: '22:00', days: 'daily', entity: 0x0102, action: 'off' }, // sensor: no on_off
      { time: '22:00', days: 'daily', entity: 0x0100, action: 'dim' },
    ]) {
      await expect(c.req('schedule.add', bad)).rejects.toMatchObject({ code: 'bad_args' });
    }
    await expect(c.req('schedule.add', { time: '22:00', days: 'daily', entity: 0x9999, action: 'off' })).rejects.toMatchObject({ code: 'not_found' });
    await c.req('schedule.del', { index: 0 });
    await until(() => c.getSnapshot().schedules.length === 1, 'one schedule left');
    expect(c.getSnapshot().schedules[0]).toMatchObject({ index: 0, time: '06:30' });
  });

  it('timers: a timer fires, changes the device and disappears', async () => {
    const c = client();
    c.connect();
    await ready(c);
    await c.req('timer.add', { entity: 0x0101, action: 'on', delay_s: 1 });
    await until(() => c.getSnapshot().timers.length === 1, 'timer listed');
    expect(c.getSnapshot().timers[0]!.left_s).toBeGreaterThan(0);
    await until(() => attrValue(c.getSnapshot(), 0x0101, 0x01) === true, 'timer fired', 3000);
    await until(() => c.getSnapshot().timers.length === 0, 'timer removed');
  });

  it('timers can be cancelled; bad arguments are rejected', async () => {
    const c = client();
    c.connect();
    await ready(c);
    await c.req('timer.add', { entity: 0x0101, action: 'on', delay_s: 60 });
    await c.req('timer.cancel', { index: 0 });
    await until(() => c.getSnapshot().timers.length === 0, 'cancelled');
    for (const bad of [
      { entity: 0x0101, action: 'on', delay_s: 0 },
      { entity: 0x0101, action: 'on', delay_s: 1.5 },
      { entity: 0x0101, action: 'blink', delay_s: 5 },
      { entity: 0x0102, action: 'on', delay_s: 5 },
    ]) {
      await expect(c.req('timer.add', bad)).rejects.toMatchObject({ code: 'bad_args' });
    }
    await expect(c.req('timer.add', { entity: 0x9999, action: 'on', delay_s: 5 })).rejects.toMatchObject({ code: 'not_found' });
  });

  it('scenes: put, list for everyone, apply (counts), replace, delete', async () => {
    const c = client();
    c.connect();
    await ready(c);
    const actions = [{ entity: 0x0100, attr: 1, value: false }, { entity: 0x0101, attr: 1, value: true }];
    await c.req('scene.put', { name: 'night', actions });
    await until(() => c.getSnapshot().scenes.length === 1, 'scene listed');
    expect(c.getSnapshot().scenes[0]).toEqual({ name: 'night', actions });

    expect(await c.req('scene.apply', { name: 'night' })).toEqual({ sent: 2, skipped: 0 });
    await until(() => attrValue(c.getSnapshot(), 0x0101, 0x01) === true, 'lamp on');
    expect(attrValue(c.getSnapshot(), 0x0100, 0x01)).toBe(false);

    await c.req('scene.put', { name: 'night', actions: [actions[0]!] }); // same name: replaced
    await until(() => c.getSnapshot().scenes[0]?.actions.length === 1, 'scene replaced');
    expect(c.getSnapshot().scenes).toHaveLength(1);

    hub.setChipStatus(1, 'silent');
    expect(await c.req('scene.apply', { name: 'night' })).toEqual({ sent: 0, skipped: 1 });
    await c.req('scene.del', { name: 'night' });
    await until(() => c.getSnapshot().scenes.length === 0, 'scene deleted');
  });

  it('schedules, timers and rules can apply a scene, and a scene in use cannot be deleted', async () => {
    const c = client();
    c.connect();
    await ready(c);
    await c.req('scene.put', { name: 'night', actions: [{ entity: 0x0101, attr: 1, value: true }] });
    await c.req('schedule.add', { time: '22:00', days: 'weekdays', scene: 'night' });
    await c.req('timer.add', { delay_s: 1, scene: 'night' });
    await c.req('rule.add', { when: { entity: 0x0102, attr: 0x50, op: 'any' }, then: { scene: 'night' } });
    await until(() => c.getSnapshot().schedules.length === 1 && c.getSnapshot().timers.length === 1 && c.getSnapshot().rules.length === 1, 'all listed');
    expect(c.getSnapshot().schedules[0]).toMatchObject({ time: '22:00', days: 'weekdays', scene: 'night' });
    expect(c.getSnapshot().timers[0]).toMatchObject({ scene: 'night' });
    expect(c.getSnapshot().rules[0]!.then).toEqual({ scene: 'night' });

    await expect(c.req('scene.del', { name: 'night' })).rejects.toMatchObject({ code: 'bad_args' });
    await until(() => attrValue(c.getSnapshot(), 0x0101, 0x01) === true, 'the timer applied the scene', 3000);
    await c.req('schedule.del', { index: 0 });
    await c.req('rule.del', { index: 0 });
    await until(() => c.getSnapshot().timers.length === 0, 'timer gone');
    await c.req('scene.del', { name: 'night' }); // nothing uses it any more
  });

  it('a scene target is checked: unknown scene, mixed with a device, or malformed', async () => {
    const c = client();
    c.connect();
    await ready(c);
    await c.req('scene.put', { name: 'night', actions: [{ entity: 0x0101, attr: 1, value: true }] });
    const cases: [string, Record<string, unknown>, string][] = [
      ['schedule.add', { time: '22:00', days: 'daily', scene: 'nope' }, 'not_found'],
      ['schedule.add', { time: '22:00', days: 'daily', scene: 'night', entity: 0x0100 }, 'bad_args'],
      ['schedule.add', { time: '22:00', days: 'daily', scene: 'night', action: 'on' }, 'bad_args'],
      ['schedule.add', { time: '22:00', days: 'daily', scene: 'Bad Name' }, 'bad_args'],
      ['timer.add', { delay_s: 5, scene: 'nope' }, 'not_found'],
      ['timer.add', { delay_s: 5, scene: 'night', entity: 0x0100 }, 'bad_args'],
      ['rule.add', { when: { entity: 0x0102, attr: 0x50, op: 'any' }, then: { scene: 'nope' } }, 'not_found'],
      ['rule.add', { when: { entity: 0x0102, attr: 0x50, op: 'any' }, then: { scene: 'night', entity: 0x0100 } }, 'bad_args'],
    ];
    for (const [op, args, code] of cases) {
      await expect(c.req(op, args)).rejects.toMatchObject({ code });
    }
    expect([hub.schedules.length, hub.timersCount(), hub.rules.length]).toEqual([0, 0, 0]);
  });

  it('scenes: bad input gets specific codes and stores nothing', async () => {
    const c = client();
    c.connect();
    await ready(c);
    const ok = { entity: 0x0100, attr: 1, value: true };
    const cases: [Record<string, unknown>, string][] = [
      [{ actions: [ok] }, 'bad_args'],
      [{ name: 'Bad', actions: [ok] }, 'bad_args'],
      [{ name: 'x', actions: [] }, 'bad_args'],
      [{ name: 'x', actions: [ok, ok] }, 'bad_args'],
      [{ name: 'x', actions: [{ ...ok, value: 5 }] }, 'bad_args'],
      [{ name: 'x', actions: [{ entity: 0x0100, attr: 0x13, value: 5 }] }, 'read_only'],
      [{ name: 'x', actions: [{ ...ok, entity: 0x9999 }] }, 'not_found'],
      [{ name: 'x', actions: [{ ...ok, attr: 99 }] }, 'not_found'],
    ];
    for (const [args, code] of cases) {
      await expect(c.req('scene.put', args)).rejects.toMatchObject({ code });
    }
    await expect(c.req('scene.apply', { name: 'nope' })).rejects.toMatchObject({ code: 'not_found' });
    await expect(c.req('scene.del', { name: 'nope' })).rejects.toMatchObject({ code: 'not_found' });
    expect(hub.scenes).toEqual([]);
  });

  it('wifi.scan lists the networks, strongest first; wifi.set answers at once and the hub then moves (or not)', async () => {
    const c = client({ backoffMs: { min: 20, max: 40 } });
    c.connect();
    await ready(c);
    expect(c.getSnapshot().hub?.wifi).toMatchObject({ ssid: 'home' });
    const res = await c.req('wifi.scan');
    expect((res?.networks as { ssid: string }[]).map((n) => n.ssid)).toEqual(['home', 'cafe', 'neighbour']);

    // An open network that exists: the hub answers, drops the connection while it switches, and the client comes back.
    await c.req('wifi.set', { ssid: 'cafe', password: '' });
    await until(() => hub.wifiRequests.length === 1, 'request seen');
    await until(() => c.getSnapshot().hub?.wifi?.ssid === 'cafe', 'moved to cafe and reconnected', 4000);
    expect(hub.wifiRequests[0]).toMatchObject({ ssid: 'cafe', kept: true });

    // A secured network with the wrong password: the hub could not join, so it stays where it was.
    await c.req('wifi.set', { ssid: 'neighbour', password: 'wrongpass1' });
    await until(() => hub.wifiRequests.length === 2, 'second request');
    await until(() => c.getSnapshot().status === 'ready', 'reconnected', 4000);
    expect(hub.wifiRequests[1]).toMatchObject({ kept: false });
    expect(c.getSnapshot().hub?.wifi?.ssid).toBe('cafe');
  });

  it('wifi.set refuses what the hub would refuse, before touching the network', async () => {
    const c = client();
    c.connect();
    await ready(c);
    for (const bad of [{}, { ssid: 'home' }, { password: '12345678' }, { ssid: '', password: '12345678' }, { ssid: 'home', password: 'short' },
                       { ssid: 's'.repeat(33), password: '' }, { ssid: 5, password: '12345678' }, { ssid: 'home', password: null }]) {
      await expect(c.req('wifi.set', bad)).rejects.toMatchObject({ code: 'bad_args' });
    }
    expect(hub.wifiRequests).toHaveLength(0);
    expect(c.getSnapshot().status).toBe('ready'); // still connected: nothing was dropped
  });

  it('the hub location: set, pushed to every client with today\'s sun, cleared; bad input refused', async () => {
    const a = client();
    const b = client();
    a.connect();
    b.connect();
    await ready(a);
    await ready(b);
    expect(a.getSnapshot().hub?.location).toBeNull();
    await a.req('location.set', { lat: 50.08, lon: 14.43 });
    await until(() => b.getSnapshot().hub?.location?.lat === 50.08, 'b sees the location');
    expect(b.getSnapshot().hub).toMatchObject({ location: { lat: 50.08, lon: 14.43 }, sun: { rise: '06:00', set: '18:00' } });
    const c = client();
    c.connect();
    await ready(c);
    expect(c.getSnapshot().hub?.location).toEqual({ lat: 50.08, lon: 14.43 }); // a newcomer gets it in the welcome
    await a.req('location.clear');
    await until(() => b.getSnapshot().hub?.location === null && b.getSnapshot().hub?.sun === null, 'cleared');
    for (const bad of [{}, { lat: 91, lon: 0 }, { lat: 0, lon: 181 }, { lat: '1', lon: 2 }, { lat: 1 }]) {
      await expect(a.req('location.set', bad)).rejects.toMatchObject({ code: 'bad_args' });
    }
    expect(hub.location).toBeNull();
  });

  it('schedules and rule windows can be relative to the sun', async () => {
    const c = client();
    c.connect();
    await ready(c);
    await c.req('schedule.add', { time: 'sunset+30', days: 'daily', entity: 0x0100, action: 'on' });
    await c.req('rule.add', { when: { entity: 0x0102, attr: 0x50, op: 'any' }, then: { entity: 0x0101, attr: 1, value: true }, between: { from: 'sunset', to: '23:00' } });
    await until(() => c.getSnapshot().schedules.length === 1 && c.getSnapshot().rules.length === 1, 'listed');
    expect(c.getSnapshot().schedules[0]!.time).toBe('sunset+30');
    expect(c.getSnapshot().rules[0]!.between).toEqual({ from: 'sunset', to: '23:00' });
    for (const time of ['sunset+181', 'dusk', 'Sunset']) {
      await expect(c.req('schedule.add', { time, days: 'daily', entity: 0x0100, action: 'on' })).rejects.toMatchObject({ code: 'bad_args' });
    }
    await expect(c.req('rule.add', { when: { entity: 0x0102, attr: 0x50, op: 'any' }, then: { entity: 0x0101, attr: 1, value: true }, between: { from: 'sunset', to: 'sunset' } }))
      .rejects.toMatchObject({ code: 'bad_args' });
  });

  it('an unknown operation is reported as unsupported', async () => {
    const c = client();
    c.connect();
    await ready(c);
    await expect(c.req('reboot')).rejects.toMatchObject({ code: 'unsupported' });
  });
});

describe('a connection that went silent', () => {
  it('is dropped and re-made when nothing arrives for staleAfterMs, though the socket never closed', async () => {
    let freeze = false;
    let made = 0;
    const freezing = (url: string): SocketLike => {
      made++;
      const ws = new NodeWebSocket(url) as unknown as SocketLike;
      const wrapped: SocketLike = {
        onopen: null, onmessage: null, onclose: null, onerror: null,
        send: (d) => ws.send(d),
        close: () => ws.close(),
      };
      ws.onopen = (e) => wrapped.onopen?.(e);
      ws.onmessage = (e) => { if (!freeze) wrapped.onmessage?.(e); };   // the link "dies": data stops, no close
      ws.onclose = (e) => { if (!freeze) wrapped.onclose?.(e); };
      ws.onerror = (e) => wrapped.onerror?.(e);
      return wrapped;
    };
    const c = client({ socketFactory: freezing, staleAfterMs: 300, backoffMs: { min: 20, max: 40 } });
    c.connect();
    await ready(c);
    expect(made).toBe(1);
    freeze = true;
    await until(() => made === 2, 'a second socket was opened', 3000);
    freeze = false;
    await ready(c); // and the new connection works: a fresh welcome and snapshot
    expect(c.getSnapshot().entities.length).toBeGreaterThan(0);
  });

  it('a healthy connection that keeps hearing from the hub is left alone; 0 turns the watch off', async () => {
    const made: number[] = [];
    const counting = (url: string): SocketLike => { made.push(1); return wsFactory(url); };
    const a = client({ socketFactory: counting, staleAfterMs: 400 });
    a.connect();
    await ready(a);
    for (let i = 0; i < 6; i++) {            // 6 × 150 ms of live traffic, longer than staleAfterMs
      hub.setValue(0x0100, 0x13, 10 + i);
      await new Promise((r) => setTimeout(r, 150));
    }
    expect(made).toHaveLength(1);

    let frozen = false;
    const mute = (url: string): SocketLike => {
      const ws = new NodeWebSocket(url) as unknown as SocketLike;
      const w: SocketLike = { onopen: null, onmessage: null, onclose: null, onerror: null, send: (d) => ws.send(d), close: () => ws.close() };
      ws.onopen = (e) => w.onopen?.(e);
      ws.onmessage = (e) => { if (!frozen) w.onmessage?.(e); };
      ws.onclose = (e) => w.onclose?.(e);
      return w;
    };
    const b = client({ socketFactory: (u) => { made.push(2); return mute(u); }, staleAfterMs: 0 });
    b.connect();
    await ready(b);
    frozen = true;
    await new Promise((r) => setTimeout(r, 700));
    expect(made.filter((x) => x === 2)).toHaveLength(1); // never replaced
  });
});

describe('reconnecting', () => {
  it('after the connection drops the client reconnects and gets a fresh snapshot', async () => {
    const c = client();
    c.connect();
    await ready(c);
    const states: HomeState['status'][] = [];
    c.subscribe(() => states.push(c.getSnapshot().status));
    hub.setValue(0x0100, 0x13, 99);
    await until(() => attrValue(c.getSnapshot(), 0x0100, 0x13) === 99, 'before drop');
    hub.dropAll();
    await until(() => c.getSnapshot().status === 'reconnecting', 'reconnecting');
    expect(c.getSnapshot().entities).toEqual([]); // stale data is not shown as current
    await ready(c);
    expect(attrValue(c.getSnapshot(), 0x0100, 0x13)).toBe(99); // the hub's current value
    expect(states).toContain('reconnecting');
  });

  it('requests in flight when the connection drops are rejected, not left hanging', async () => {
    const c = client();
    c.connect();
    await ready(c);
    hub.opts.ackDelayMs = 10_000;
    const p = c.cmd(0x0101, 0x01, true);
    const rejected = expect(p).rejects.toMatchObject({ code: 'disconnected' });
    await new Promise((r) => setTimeout(r, 50));
    hub.dropAll();
    await rejected;
  });

  it('keeps retrying while the hub is down and recovers when it is back', async () => {
    const port = hub.port;
    const c = client({ url: `ws://127.0.0.1:${port}/ws` });
    c.connect();
    await ready(c);
    await hub.close();
    await until(() => c.getSnapshot().status === 'reconnecting', 'reconnecting');
    await new Promise((r) => setTimeout(r, 200)); // several failed attempts
    hub = await MockHub.start({ port, helloTimeoutMs: 300, ackDelayMs: 10 });
    await ready(c);
  });

  it('close() stops everything for good', async () => {
    const c = client();
    c.connect();
    await ready(c);
    c.close();
    expect(c.getSnapshot().status).toBe('closed');
    await until(() => hub.clientCount === 0, 'hub sees the disconnect');
    await new Promise((r) => setTimeout(r, 150));
    expect(c.getSnapshot().status).toBe('closed');
    expect(hub.clientCount).toBe(0);
    await expect(c.cmd(0x0100, 0x01, true)).rejects.toBeInstanceOf(HubError);
  });

  it('connect() after close() starts a closed client again (effects re-run in React)', async () => {
    const c = client();
    c.connect();
    await ready(c);
    c.close();
    expect(c.getSnapshot().status).toBe('closed');
    c.connect();
    await ready(c);
    expect(c.getSnapshot().entities).toHaveLength(3);
  });

  it('reconnect: false leaves it disconnected after a drop', async () => {
    const c = client({ reconnect: false });
    c.connect();
    await ready(c);
    hub.dropAll();
    await until(() => c.getSnapshot().status === 'reconnecting', 'dropped');
    await new Promise((r) => setTimeout(r, 200));
    expect(hub.clientCount).toBe(0);
  });
});

describe('history', () => {
  type Page = { now: number; latest: number; oldest: number; more: boolean; events: { seq: number; age: number; kind: string; what?: string; entity?: number; attr?: number; value?: unknown }[] };
  const hist = async (c: HubClient, args?: Record<string, unknown>) => (await c.req('history', args)) as unknown as Page;

  it('starts with the hub starting, then records commands and the changes they cause, newest first', async () => {
    const c = client();
    c.connect();
    await ready(c);
    let p = await hist(c);
    expect(p.events.map((e) => e.kind)).toEqual(['boot']);
    expect(p.events[0]).toMatchObject({ seq: 1, what: 'power_on' });
    expect(p.now).toBeGreaterThan(1_700_000_000);

    await c.cmd(0x0101, 0x01, true); // lamp on
    p = await hist(c);
    expect(p.events.map((e) => e.kind)).toEqual(['state', 'command', 'boot']);
    expect(p.events[0]).toMatchObject({ entity: 0x0101, attr: 1, value: true });
    expect(p.events[1]).toMatchObject({ entity: 0x0101, attr: 1, value: true });
    expect(p.latest).toBe(3);
    expect(p.oldest).toBe(1);
  });

  it('a device changing by itself is recorded, the noisy measurements are not', async () => {
    const c = client();
    c.connect();
    await ready(c);
    hub.setValue(0x0102, 0x50, true); // motion
    hub.setValue(0x0100, 0x13, 77); // power
    hub.setValue(0x0100, 0x14, 9); // energy
    const p = await hist(c);
    expect(p.events.map((e) => e.kind)).toEqual(['state', 'boot']);
    expect(p.events[0]).toMatchObject({ entity: 0x0102, attr: 0x50, value: true });
  });

  it('a command that fails is recorded as failed, with why', async () => {
    hub.forceAck = 'unreachable';
    const c = client();
    c.connect();
    await ready(c);
    await c.cmd(0x0101, 0x01, true);
    const p = await hist(c);
    expect(p.events.map((e) => e.kind)).toEqual(['command_failed', 'command', 'boot']);
    expect(p.events[0]).toMatchObject({ what: 'unreachable', entity: 0x0101 });
  });

  it('chips going quiet and coming back, and timers that ran, are in it', async () => {
    const c = client();
    c.connect();
    await ready(c);
    hub.setChipStatus(1, 'silent');
    hub.setChipStatus(1, 'ready');
    const t = await c.req('timer.add', { entity: 0x0100, action: 'off', delay_s: 1 });
    expect(t.index).toBe(0);
    await until(() => hub.timersCount() === 0, 'timer fired', 3000);
    const p = await hist(c);
    expect(p.events.map((e) => `${e.kind}${e.what ? ':' + e.what : ''}`)).toEqual(['state', 'timer', 'chip:resumed', 'chip:lost', 'boot:power_on']);
  });

  it('pages back with `before`, and `limit` caps a page', async () => {
    const c = client();
    c.connect();
    await ready(c);
    for (let i = 0; i < 6; i++) hub.setValue(0x0102, 0x50, i % 2 === 0);
    const first = await hist(c, { limit: 4 });
    expect(first.events.map((e) => e.seq)).toEqual([7, 6, 5, 4]);
    expect(first.more).toBe(true);
    const second = await hist(c, { limit: 4, before: 4 });
    expect(second.events.map((e) => e.seq)).toEqual([3, 2, 1]);
    expect(second.more).toBe(false);
    expect((await hist(c, { limit: 3, before: 4 })).more).toBe(false); // exactly the rest: nothing beyond
    expect((await hist(c, { before: 1 })).events).toEqual([]);
  });

  it('only one device: its changes and commands, not the others', async () => {
    const c = client();
    c.connect();
    await ready(c);
    await c.cmd(0x0101, 0x01, true);
    hub.setValue(0x0102, 0x50, true);
    hub.setChipStatus(1, 'silent');
    const p = await hist(c, { entity: 0x0101 });
    expect(p.events.map((e) => e.kind)).toEqual(['state', 'command']);
    expect(p.more).toBe(false); // the ring has older events, but none about this device
    expect((await hist(c, { entity: 0x0777 })).events).toEqual([]);
  });

  it('refuses arguments the hub would refuse', async () => {
    const c = client();
    c.connect();
    await ready(c);
    for (const bad of [{ limit: 0 }, { limit: 25 }, { limit: '5' }, { before: -1 }, { entity: 70000 }, { entity: 'x' }]) {
      await expect(c.req('history', bad)).rejects.toMatchObject({ code: 'bad_args' });
    }
  });

  it('holds the last 256 events; older ones are overwritten and `oldest` moves', async () => {
    const c = client();
    c.connect();
    await ready(c);
    for (let i = 0; i < 300; i++) hub.setValue(0x0102, 0x50, i % 2 === 0);
    const p = await hist(c);
    expect(p.latest).toBe(301);
    expect(p.oldest).toBe(46);
    expect(p.events).toHaveLength(24);
  });
});
