// @vitest-environment jsdom
import './setup-dom';
import { longPress } from './helpers';
import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import type { SocketLike } from '../src/protocol/client';
import { locatePhone } from '../src/native/position';
import { Home } from '../src/ui/Home';
import { MockHub } from '../tools/mock-hub/hub';

const wsFactory = (url: string) => new NodeWebSocket(url) as unknown as SocketLike;
vi.mock('../src/native/position', () => ({ locatePhone: vi.fn() }));
let hub: MockHub;

beforeEach(async () => {
  hub = await MockHub.start({ helloTimeoutMs: 500, ackDelayMs: 10 });
});
afterEach(async () => {
  cleanup();
  await hub.close();
});

async function openApp() {
  render(
    <Home
      settings={{ url: hub.url, token: hub.token }}
      onChangeHub={() => {}}
      clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 }, requestTimeoutMs: 2000 }}
    />,
  );
  await screen.findByText('plug1');
}
const type = (label: string, value: string) => fireEvent.change(screen.getByLabelText(label), { target: { value } });
const click = (label: string) => fireEvent.click(screen.getByLabelText(label));
/** Scroll a column of the duration wheel to a position. */
const scrollWheel = (column: 'hours' | 'minutes', y: number) => {
  const el = screen.getByTestId(`wheel-${column}`);
  el.scrollTop = y;
  fireEvent.scroll(el);
};
/** The automations tab folds: a section that is closed must be opened first. */
const openSection = (title: string) => {
  const expand = screen.queryByLabelText(`Expand ${title}`);
  if (expand) fireEvent.click(expand);
};
const gotoAutomations = async () => {
  click('Automations');
  await screen.findByLabelText('Add scene'); // scenes are the first section
  openSection('Schedules');
  await screen.findByLabelText('Add schedule');
};

describe('Renaming a device', () => {
  it('gives a device a name, which every client then sees', async () => {
    await openApp();
    expect(screen.getByLabelText('Device Motion sensor 0x0102')).toBeTruthy();
    await longPress(screen.getByLabelText('Device Motion sensor 0x0102'));
    click('Rename Motion sensor 0x0102');
    type('Name', 'hall_motion');
    click('Save name');
    await waitFor(() => expect(screen.getByLabelText('Device hall_motion')).toBeTruthy());
    expect(hub.entities.find((e) => e.id === 0x0102)!.name).toBe('hall_motion');
    expect(screen.queryByLabelText('Name')).toBeNull(); // the editor closed
  });

  it('explains a bad name without asking the hub', async () => {
    await openApp();
    await longPress(screen.getByLabelText('Device lamp'));
    click('Rename lamp');
    type('Name', 'bad/name');
    click('Save name');
    expect(await screen.findByText(/Letters, digits, spaces/)).toBeTruthy();
    expect(hub.entities.find((e) => e.id === 0x0101)!.name).toBe('lamp');
  });

  it('shows the hub\'s refusal (a name already in use) and keeps the editor open', async () => {
    await openApp();
    await longPress(screen.getByLabelText('Device lamp'));
    click('Rename lamp');
    type('Name', 'plug1');
    click('Save name');
    expect(await screen.findByText(/Invalid value: name in use/)).toBeTruthy();
    expect(screen.getByLabelText('Name')).toBeTruthy();
  });

  it('removes a name', async () => {
    await openApp();
    await longPress(screen.getByLabelText('Device lamp'));
    click('Rename lamp');
    click('Remove name');
    await waitFor(() => expect(screen.getByLabelText('Device Light 0x0101')).toBeTruthy());
    expect(hub.entities.find((e) => e.id === 0x0101)!.name).toBeNull();
  });
});

describe('Rooms', () => {
  it('moves a device into a room, which the hub keeps and every client then sees', async () => {
    await openApp();
    await longPress(screen.getByLabelText('Device lamp'));
    click('Move lamp to another room');
    click('Move to Kitchen');
    await waitFor(() => expect(hub.entities.find((e) => e.id === 0x0101)!.room).toBe('Kitchen'));
    // The device is renamed nowhere: a room is not a name.
    expect(hub.entities.find((e) => e.id === 0x0101)!.name).toBe('lamp');
    await waitFor(() => expect(screen.getByLabelText('Room Kitchen')).toBeTruthy());
    expect(screen.getByLabelText('Room Unassigned')).toBeTruthy();
  });

  it('takes a device out of its room', async () => {
    await openApp();
    await longPress(screen.getByLabelText('Device plug1'));
    click('Move plug1 to another room');
    click('Move to Office');
    await waitFor(() => expect(hub.entities.find((e) => e.id === 0x0100)!.room).toBe('Office'));
    click('Move plug1 to another room'); // the picker closes after a choice
    click('Remove room');
    await waitFor(() => expect(hub.entities.find((e) => e.id === 0x0100)!.room).toBeNull());
  });

  it('marks the room the device is in when the picker opens', async () => {
    hub.entities[0]!.room = 'Hallway';
    await openApp();
    await longPress(screen.getByLabelText('Device plug1'));
    click('Move plug1 to another room');
    expect(screen.getByLabelText('Move to Hallway (current room)')).toBeTruthy();
    expect(screen.getByLabelText('Move to Kitchen')).toBeTruthy();
  });
});

describe('Schedules', () => {
  it('creates a schedule and deletes it after a second tap', async () => {
    await openApp();
    await gotoAutomations();
    expect(screen.getByText(/No schedules/)).toBeTruthy();

    click('Add schedule');
    click('Save schedule'); // nothing filled in
    expect(await screen.findByText(/Time must look like 07:30/)).toBeTruthy();
    type('Time', '22:00');
    click('Save schedule');
    expect(await screen.findByText('Pick a device')).toBeTruthy();
    click('Device');
    click('Choose lamp');
    click('Weekdays');
    click('Action Off');
    click('Save schedule');

    const item = await screen.findByLabelText('schedule 22:00 · Off lamp');
    expect(within(item).getByText('Weekdays')).toBeTruthy();
    expect(hub.schedules).toHaveLength(1);
    // The section header carries the count.
    expect(screen.getByText('SCHEDULES').parentElement?.textContent ?? '').toContain('1');

    click('Delete schedule 22:00 · Off lamp');
    expect(hub.schedules).toHaveLength(1); // the first tap only arms it
    fireEvent.click(screen.getByText('Confirm'));
    await waitFor(() => expect(hub.schedules).toHaveLength(0));
    await screen.findByText(/No schedules/);
  });

  it('a one-off on a date: the form asks for the date, refuses a past or invalid one, the card says "Once"', async () => {
    await openApp();
    await gotoAutomations();
    click('Add schedule');
    type('Time', '19:00');
    click('Device');
    click('Choose lamp');
    click('Once, on a date');
    type('Date', '2001-01-01');
    click('Save schedule');
    expect(await screen.findByText('That date has already passed')).toBeTruthy();
    type('Date', '2099-02-30');
    click('Save schedule');
    expect(await screen.findByText(/Date must look like/)).toBeTruthy();
    type('Date', '2099-10-20');
    click('Save schedule');
    const item = await screen.findByLabelText('schedule 19:00 · Off lamp');
    expect(within(item).getByText('Once, 20 Oct 2099')).toBeTruthy();
    expect(hub.schedules[0]).toMatchObject({ days: 'once', date: '2099-10-20' });
  });

  it('picking individual days works, and none selected is refused', async () => {
    await openApp();
    await gotoAutomations();
    click('Add schedule');
    type('Time', '06:30');
    click('Device');
    click('Choose plug1');
    click('Pick days');
    click('Save schedule');
    expect(await screen.findByText('Pick at least one day')).toBeTruthy();
    click('Day Mon');
    click('Day Fri');
    click('Save schedule');
    await screen.findByLabelText('schedule 06:30 · Off plug1');
    expect(hub.schedules[0]!.days).toEqual(['mon', 'fri']);
    expect(screen.getByText('Mon, Fri')).toBeTruthy();
  });
});

describe('Timers', () => {
  it('starts a timer from a preset, shows the countdown and cancels it', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Timers');
    click('Add timer');
    click('Device');
    click('Choose lamp');
    click('Action On');
    // The wheel: scroll the minutes to 15.
    scrollWheel('minutes', 102); // 15 min
    click('Start timer');
    const item = await screen.findByLabelText('timer On lamp');
    expect(within(item).getByText('in 15 min')).toBeTruthy();
    click('Delete timer On lamp');
    fireEvent.click(screen.getByText('Confirm'));
    await screen.findByText('No timers running.');
  });

  it('a timer with no time is refused', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Timers');
    click('Add timer');
    click('Device');
    click('Choose lamp');
    // The wheel says zero minutes: there is nothing to wait for.
    scrollWheel('minutes', 0);
    click('Start timer');
    expect(await screen.findByText('Enter how many minutes')).toBeTruthy();
    expect(hub.timersCount()).toBe(0);
  });

  it('a timer that fires disappears from the list and switches the device', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Timers');
    await act(async () => {
      // 1 s is below the form's smallest preset, so ask the hub directly, as another client would.
      const ws = new NodeWebSocket(hub.url);
      await new Promise<void>((r) => ws.on('open', () => r()));
      ws.send(JSON.stringify({ type: 'hello', v: 1, token: hub.token }));
      await new Promise((r) => setTimeout(r, 100));
      ws.send(JSON.stringify({ type: 'req', id: 1, op: 'timer.add', args: { entity: 257, action: 'on', delay_s: 1 } }));
      await new Promise((r) => setTimeout(r, 100));
      ws.close();
    });
    await screen.findByLabelText('timer On lamp');
    await waitFor(() => expect(screen.queryByLabelText('timer On lamp')).toBeNull(), { timeout: 3000 });
    click('Home');
    await waitFor(() => expect(screen.getByLabelText('lamp is on')).toBeTruthy());
  });
});

describe('Rules', () => {
  it('a rule can watch the phone: "when I am home, the hall light on"', async () => {
    // The phone reports first, so it becomes a device the rule can point at.
    hub.entities.push({
      id: 0x0f80,
      name: 'My phone',
      room: null,
      chip: 0xfe,
      device: 'phone',
      attrs: [{ id: 0x52, name: 'home', vt: 'bool', flags: ['r', 'rep'], value: true }],
    });
    await openApp();
    await gotoAutomations();
    openSection('Rules');
    click('Add rule');

    click('When: what to look at');
    click('Choose My phone · Home');
    click('When: comparison');
    click('When is');
    click('When: At home');

    click('Then: what to set');
    click('Choose lamp · Power');
    click('Then: On');
    click('Save rule');

    await screen.findByLabelText('rule 1');
    expect(hub.rules[0]).toMatchObject({
      when: { entity: 0x0f80, attr: 0x52, op: '==', value: true },
      then: { entity: 0x0101, attr: 1, value: true },
    });
    // The rule reads in words, like every other rule.
    expect(screen.getByText(/When My phone At home/)).toBeTruthy();
  });

  it('builds a rule with every option and reads it back as a sentence', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Rules');
    expect(screen.getByText(/No rules/)).toBeTruthy();
    click('Add rule');

    // The rule is built from blocks: WHEN, then AND, then THEN.
    click('When: what to look at');
    click('Choose Motion sensor 0x0102 · Presence');
    click('When: comparison');
    click('When is');
    click('When: Someone is there');

    click('Then: what to set');
    click('Choose lamp · Power');
    click('Then: On');
    click('More options');
    click('Between times');
    type('From', '18:00');
    type('To', '23:00');
    click('Add a condition');
    click('Only while 1: what to look at');
    click('Choose plug1 · Power draw');
    click('Only while 1: comparison');
    click('Only while 1 is above');
    type('Only while 1 value', '5');
    type('Cooldown (seconds)', '300');
    click('Save rule');

    const item = await screen.findByLabelText('rule 1');
    expect(within(item).getByText(/When Motion sensor 0x0102 Someone is there, turn lamp on/)).toBeTruthy();
    expect(within(item).getByText(/between 18:00 and 23:00 · only while plug1 power draw is above 5 W · at most once per 5 min/)).toBeTruthy();
    expect(hub.rules[0]).toMatchObject({
      when: { entity: 258, attr: 0x50, op: '==', value: true },
      then: { entity: 257, attr: 1, value: true },
      between: { from: '18:00', to: '23:00' },
      and: { entity: 256, attr: 0x13, op: '>', value: 5 },
      cooldown_s: 300,
    });

    click('Delete rule 1');
    fireEvent.click(screen.getByText('Confirm'));
    await screen.findByText(/No rules/);
    expect(hub.rules).toHaveLength(0);
  });

  it('shows the rule as a sentence while it is being built', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Rules');
    click('Add rule');
    // A half-built rule says what is missing rather than pretending to be complete.
    expect(screen.getByText('When … → then …')).toBeTruthy();
    click('When: what to look at');
    click('Choose Motion sensor 0x0102 · Presence');
    click('When: comparison');
    click('When is');
    click('When: Someone is there');
    expect(screen.getByText(/When Motion sensor 0x0102 Someone is there/)).toBeTruthy();
  });

  it('says what is missing, in the order the form reads', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Rules');
    click('Add rule');
    click('Save rule');
    expect(await screen.findByText('When: pick what starts the rule')).toBeTruthy();
    click('When: what to look at');
    click('Choose plug1 · Power draw');
    click('When: comparison');
    click('When is above');
    type('When value', 'lots');
    click('Save rule');
    expect(await screen.findByText('When: Enter a number')).toBeTruthy();
    type('When value', '1500');
    click('Save rule');
    expect(await screen.findByText('Then: pick what to set')).toBeTruthy();
    expect(hub.rules).toHaveLength(0);
  });

  it('brightness is entered in percent', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Rules');
    click('Add rule');
    click('When: what to look at');
    click('Choose Motion sensor 0x0102 · Presence');
    click('Then: what to set');
    click('Choose lamp · Brightness');
    type('Then value', '50');
    click('Save rule');
    await screen.findByLabelText('rule 1');
    expect(hub.rules[0]!.then).toMatchObject({ attr: 0x02, value: 127 });
    expect(screen.getByText(/set lamp brightness to 50 %/)).toBeTruthy();
  });

  it('only writable attributes are offered for the action', async () => {
    await openApp();
    await gotoAutomations();
    openSection('Rules');
    click('Add rule');
    click('Then: what to set');
    expect(screen.getByLabelText('Choose lamp · Brightness')).toBeTruthy();
    expect(screen.queryByLabelText('Choose plug1 · Power draw')).toBeNull();
    expect(screen.queryByLabelText('Choose Motion sensor 0x0102 · Presence')).toBeNull();
  });
});

describe('Editing a scene', () => {
  const evening = () => ({ name: 'evening', actions: [
    { entity: 0x0100, attr: 1, value: true },
    { entity: 0x0101, attr: 2, value: 254 },
  ] });

  it('changes values, removes a setting, adds another, and saves under the same name', async () => {
    hub.scenes.push(evening());
    await openApp();
    click('Automations');
    click('Edit scene evening');
    await screen.findByText('Edit scene evening');
    click('plug1 · Power value Off');                     // flip plug1 to off
    type('lamp · Brightness value', '40');
    click('Add setting');
    fireEvent.click(await screen.findByText('lamp · Power'));
    click('lamp · Power value On');
    click('Save changes');
    await waitFor(() => expect(hub.scenes[0]!.actions).toEqual([
      { entity: 0x0100, attr: 1, value: false },
      { entity: 0x0101, attr: 2, value: 102 },            // 40 % of 254
      { entity: 0x0101, attr: 1, value: true },
    ]));
    expect(hub.scenes).toHaveLength(1);                    // replaced, not duplicated
    await screen.findByLabelText('scene evening');         // back to the list
    expect(screen.queryByText('Edit scene evening')).toBeNull();
  });

  it('removing a setting works; a scene must keep at least one', async () => {
    hub.scenes.push(evening());
    await openApp();
    click('Automations');
    click('Edit scene evening');
    click('Remove setting lamp · Brightness');
    click('Save changes');
    await waitFor(() => expect(hub.scenes[0]!.actions).toHaveLength(1));
    click('Edit scene evening');
    click('Remove setting plug1 · Power');
    click('Save changes');
    expect(await screen.findByText(/at least one/)).toBeTruthy();
    expect(hub.scenes[0]!.actions).toHaveLength(1);
  });

  it('a wrong value is named and nothing is sent; Cancel leaves the scene alone', async () => {
    hub.scenes.push(evening());
    await openApp();
    click('Automations');
    click('Edit scene evening');
    type('lamp · Brightness value', '250');
    click('Save changes');
    expect(await screen.findByText(/lamp · Brightness: Out of range/)).toBeTruthy();
    click('Cancel');
    expect(hub.scenes[0]!.actions[1]!.value).toBe(254);
    await screen.findByLabelText('scene evening');
  });

  it('"use what the devices are doing now" refreshes the values, and settings of vanished devices are dropped with a note', async () => {
    hub.scenes.push({ name: 'evening', actions: [
      { entity: 0x0100, attr: 1, value: false },
      { entity: 0x0101, attr: 2, value: 254 },
      { entity: 0x7777, attr: 1, value: true },
    ] });
    await openApp();
    click('Automations');
    click('Edit scene evening');
    expect(await screen.findByText(/1 setting refer to devices that are gone/)).toBeTruthy();
    click('Use current state');
    click('Save changes');
    await waitFor(() => expect(hub.scenes[0]!.actions).toEqual([
      { entity: 0x0100, attr: 1, value: true },            // plug1 is on right now
      { entity: 0x0101, attr: 2, value: 128 },             // the lamp is at 128
    ]));
  });
});

describe('Wi-Fi', () => {
  it('scans, picks a network, asks for the password and sends it; the app reconnects by itself', async () => {
    await openApp();
    click('Settings');
    click('Hub & health');
    await screen.findByText(/home · -58 dBm · 192.168.0.202/);
    click('Change network');
    click('Scan for networks');
    await screen.findByLabelText('Network cafe');
    expect(screen.getByLabelText('Network cafe').textContent).toContain('open');
    click('Network neighbour');
    expect((screen.getByLabelText('Network name') as HTMLInputElement).value).toBe('neighbour');
    type('Wi-Fi password', 'goodpass1');
    click('Connect');
    await screen.findByText(/Switching to neighbour/);
    expect(hub.wifiRequests).toEqual([{ ssid: 'neighbour', password: 'goodpass1', kept: true }]);
    // The hub dropped the connection while it switched; the app came back and shows the new network.
    await screen.findByText(/neighbour · -77 dBm · 192.168.0.77/, {}, { timeout: 4000 });
  });

  it('an open network needs no password, and a too-short password is explained without bothering the hub', async () => {
    await openApp();
    click('Settings');
    click('Hub & health');
    click('Change network');
    type('Network name', 'home');
    type('Wi-Fi password', 'short');
    click('Connect');
    expect(await screen.findByText(/The password is 8 to 63 characters/)).toBeTruthy();
    expect(hub.wifiRequests).toHaveLength(0);
    type('Wi-Fi password', '');
    type('Network name', 'cafe');
    click('Connect');
    await screen.findByText(/Switching to cafe/);
    expect(hub.wifiRequests[0]).toMatchObject({ ssid: 'cafe', password: '' });
  });
});

describe('The sun', () => {
  it('the Hub tab sets and clears the location; the sun times show up', async () => {
    await openApp();
    click('Settings');
    click('Hub & health');
    // The hub's own facts arrive as a separate event: give it room on a loaded machine.
    await screen.findByText(/Not set/, undefined, { timeout: 5000 });
    click('Set location');
    type('Latitude', '50.08');
    type('Longitude', '14.43');
    click('Save location');
    await screen.findByText(/50\.0800, 14\.4300 · sunrise 06:00, sunset 18:00/);
    expect(hub.location).toEqual({ lat: 50.08, lon: 14.43 });
    click('Clear location');
    await screen.findByText(/Not set/);
    expect(hub.location).toBeNull();
  });

  it('the phone can give the location in one tap', async () => {
    vi.mocked(locatePhone).mockResolvedValue({ ok: true, lat: 50.09, lon: 14.42 });
    await openApp();
    click('Settings');
    click('Hub & health');
    click('Use phone location');
    await screen.findByText(/50\.0900, 14\.4200/);
    expect(hub.location).toEqual({ lat: 50.09, lon: 14.42 });
  });

  it('a refused phone location says so and leaves the hub alone', async () => {
    vi.mocked(locatePhone).mockResolvedValue({ ok: false, error: 'Location access was not allowed. Type the coordinates instead.' });
    await openApp();
    click('Settings');
    click('Hub & health');
    click('Use phone location');
    expect(await screen.findByText(/not allowed/)).toBeTruthy();
    expect(hub.location).toBeNull();
  });

  it('a bad location is explained without asking the hub', async () => {
    await openApp();
    click('Settings');
    click('Hub & health');
    click('Set location');
    type('Latitude', '95');
    type('Longitude', '14');
    click('Save location');
    expect(await screen.findByText(/Latitude is between -90 and 90/)).toBeTruthy();
    expect(hub.location).toBeNull();
  });

  it('a schedule can be set at sunset; without a location the card says it waits, with one it does not', async () => {
    await openApp();
    await gotoAutomations();
    click('Add schedule');
    click('Time: sunset');
    expect((screen.getByLabelText('Time') as HTMLInputElement).value).toBe('sunset');
    expect(screen.getByText(/needs its location first/)).toBeTruthy();
    type('Time', 'sunset+30');
    expect(screen.getByText(/30 min after sunset/)).toBeTruthy();
    click('Device');
    fireEvent.click(await screen.findByText('plug1'));
    click('Save schedule');
    await screen.findByLabelText('schedule 30 min after sunset · Off plug1');
    expect(hub.schedules[0]).toMatchObject({ time: 'sunset+30', entity: 0x0100 });
    expect(screen.getByText(/waiting for the hub's location/)).toBeTruthy();

    click('Settings');
    click('Hub & health');
    click('Set location');
    type('Latitude', '50');
    type('Longitude', '14');
    click('Save location');
    await screen.findByText(/sunrise 06:00/);
    // Back out of the Hub screen into the dock.
    click('Back to Settings');
    click('Automations');
    openSection('Schedules');
    await screen.findByLabelText('schedule 30 min after sunset · Off plug1');
    expect(screen.queryByText(/waiting for the hub's location/)).toBeNull();
  });
});

describe('Automations that apply a scene', () => {
  const night = { name: 'night', actions: [{ entity: 0x0101, attr: 1, value: true }] };

  it('without scenes the forms offer only devices', async () => {
    await openApp();
    await gotoAutomations();
    click('Add schedule');
    expect(screen.queryByLabelText('Target a scene')).toBeNull();
  });

  it('a schedule, a timer and a rule can each target a scene', async () => {
    hub.scenes.push(night);
    await openApp();
    await gotoAutomations();

    click('Add schedule');
    click('Target a scene');
    click('Scene night');
    type('Time', '22:00');
    click('Save schedule');
    await screen.findByLabelText('schedule 22:00 · Scene night');
    expect(hub.schedules[0]).toMatchObject({ time: '22:00', scene: 'night' });

    openSection('Timers');
    click('Add timer');
    click('Target a scene');
    click('Scene night');
    scrollWheel('minutes', 34); // 5 min
    click('Start timer');
    await screen.findByLabelText('timer Scene night');
    expect(hub.timersCount()).toBe(1);

    openSection('Rules');
    click('Add rule');
    click('Apply a scene');
    click('Scene night');
    fireEvent.click(screen.getByLabelText('When: what to look at')); // pick the trigger
    fireEvent.click(await screen.findByText('Motion sensor 0x0102 · Presence'));
    click('Save rule');
    await screen.findByLabelText('rule 1');
    expect(hub.rules[0]!.then).toEqual({ scene: 'night' });
    expect(screen.getByLabelText('rule 1').textContent).toContain('apply scene night');
  });

  it('a scene that something uses cannot be deleted, and the hub says why', async () => {
    hub.scenes.push(night);
    hub.schedules.push({ index: 0, time: '22:00', days: 'daily', scene: 'night' });
    await openApp();
    click('Automations');
    await screen.findByLabelText('scene night');
    click('Delete scene night');
    click('Delete scene night');
    expect(await screen.findByText(/scene is used by a schedule, timer or rule/)).toBeTruthy();
    expect(hub.scenes).toHaveLength(1);
  });
});

describe('Scenes', () => {
  it('saves the chosen devices as they are now, shows the scene, and deletes it after a second tap', async () => {
    await openApp();
    click('Automations');
    click('Add scene');
    type('Scene name', 'evening');
    click('Include plug1');
    click('Include lamp');
    // plug1 is on, the lamp is off at 50 % in the mock home.
    expect(screen.getByLabelText('Scene contents').textContent).toBe('Will save: plug1 on · lamp off · lamp brightness 50 %');
    click('Save scene');
    await screen.findByLabelText('scene evening');
    expect(hub.scenes).toEqual([
      { name: 'evening', actions: [
        { entity: 0x0100, attr: 1, value: true },
        { entity: 0x0101, attr: 1, value: false },
        { entity: 0x0101, attr: 2, value: 128 },
      ] },
    ]);
    expect(screen.getByText('SCENES').parentElement?.textContent ?? '').toContain('1');

    click('Delete scene evening');
    expect(hub.scenes).toHaveLength(1); // the first tap only arms it
    click('Delete scene evening');
    await waitFor(() => expect(screen.queryByLabelText('scene evening')).toBeNull());
    expect(hub.scenes).toHaveLength(0);
  });

  it('refuses a bad name or no devices without asking the hub', async () => {
    await openApp();
    click('Automations');
    click('Add scene');
    type('Scene name', 'Evening');
    click('Include plug1');
    click('Save scene');
    expect(await screen.findByText(/Use a-z, 0-9 and _/)).toBeTruthy();
    type('Scene name', 'evening');
    click('Include plug1'); // un-pick
    click('Save scene');
    expect(await screen.findByText(/Choose at least one device/)).toBeTruthy();
    expect(hub.scenes).toHaveLength(0);
  });

  it('a read-only device is not offered (the motion sensor has nothing to set)', async () => {
    await openApp();
    click('Automations');
    click('Add scene');
    expect(screen.queryByLabelText('Include Motion sensor 0x0102')).toBeNull();
    expect(screen.getByLabelText('Include plug1')).toBeTruthy();
  });

  it('applies a scene from the Devices tab: the devices follow and the count is shown', async () => {
    hub.scenes.push({ name: 'night', actions: [
      { entity: 0x0100, attr: 1, value: false },
      { entity: 0x0101, attr: 1, value: true },
    ] });
    await openApp();
    await screen.findByLabelText('Apply scene night');
    click('Apply scene night');
    await screen.findByText("Applied 'night': 2 settings sent");
    expect(hub.scenesApplied).toEqual(['night']);
    await waitFor(() => expect(screen.getByLabelText('lamp is on')).toBeTruthy());
    expect(screen.getByLabelText('plug1 is off')).toBeTruthy();
  });

  it('says so when part of a scene could not be applied (the chip is silent)', async () => {
    hub.scenes.push({ name: 'night', actions: [{ entity: 0x0100, attr: 1, value: false }] });
    await openApp();
    act(() => hub.setChipStatus(1, 'silent'));
    await screen.findByLabelText('Apply scene night');
    click('Apply scene night');
    await screen.findByText(/Applied 'night': 0 sent, 1 skipped/);
  });
});
