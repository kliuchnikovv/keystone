// @vitest-environment jsdom
// The "Find devices" screen: what is around the phone, and what the hub is opening.
//
// The Bluetooth radio is faked — a computer has no Bluetooth, and the point of the
// screen is what it does with what it hears.
import './setup-dom';
import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import type { SocketLike } from '../src/protocol/client';
import { byProximity, type BleDevice } from '../src/model/bleDevices';
import { MATTER_PAYLOAD_MANUFACTURER, MATTER_PAYLOAD_SERVICE, type MatterIdentity } from '../src/model/matter';
import { proximityFromRssi } from '../src/model/proximity';
import type { BleSnapshot, BleSource } from '../src/native/ble';
import type { NativeBleStatus } from '../src/native/types';
import { Home } from '../src/ui/Home';
import { MockHub } from '../tools/mock-hub/hub';

const wsFactory = (url: string) => new NodeWebSocket(url) as unknown as SocketLike;

/** A radio that only knows what the test tells it. */
class FakeBle implements BleSource {
  status: NativeBleStatus = { state: 'ready', authorization: 'allowed' };
  devices: BleDevice[] = [];
  private listeners = new Set<(s: BleSnapshot) => void>();

  snapshot(): BleSnapshot {
    return { status: this.status, devices: byProximity(this.devices) };
  }
  subscribe(cb: (s: BleSnapshot) => void): () => void {
    this.listeners.add(cb);
    return () => {
      this.listeners.delete(cb);
    };
  }
  start(): void {
    this.emit();
  }
  stop(): void {}
  dispose(): void {
    this.listeners.clear();
  }
  /** Something was heard. */
  push(device: BleDevice): void {
    this.devices = [...this.devices, device];
    this.emit();
  }
  emit(): void {
    const s = this.snapshot();
    for (const cb of this.listeners) cb(s);
  }
}

const radio = new FakeBle();

// The module is replaced before anything imports it.
vi.mock('../src/native/ble', () => ({ bleSource: () => radio }));

/** The bytes of the enhanced advertisement of a Matter device. */
const ONBOARDING = '0001f1ff0080000fa1b2c3d4';

function matterDevice(id: string, rssi: number, discriminator: number, source: 'manufacturer' | 'service'): BleDevice {
  const matter: MatterIdentity =
    source === 'manufacturer'
      ? {
          source,
          discriminator,
          setupHash: 'a1b2c3d4',
          kind: MATTER_PAYLOAD_MANUFACTURER,
          payload: [],
          payloadHex: ONBOARDING,
        }
      : {
          source,
          discriminator,
          setupHash: 'a1b2c3d4',
          kind: MATTER_PAYLOAD_SERVICE,
          payload: [],
          payloadHex: '01000fa1b2c3d4',
        };
  return {
    id,
    name: null,
    rssi,
    median: rssi,
    proximity: proximityFromRssi(rssi),
    lastSeen: Date.now(),
    samples: [rssi],
    matter,
  };
}

let hub: MockHub;

beforeEach(async () => {
  radio.devices = [];
  radio.status = { state: 'ready', authorization: 'allowed' };
  hub = await MockHub.start({ helloTimeoutMs: 500, ackDelayMs: 10 });
});
afterEach(async () => {
  cleanup();
  await hub.close();
});

function renderHome() {
  return render(
    <Home
      settings={{ url: hub.url, token: hub.token }}
      onChangeHub={() => {}}
      clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 }, requestTimeoutMs: 2000 }}
    />,
  );
}

/** Open the search screen and wait until it is up. */
async function openSearch() {
  renderHome();
  await screen.findByText('plug1');
  fireEvent.click(screen.getByLabelText('Add a device'));
  await screen.findByText('Find devices');
}

/** What the countdown on the screen says, in seconds. */
function secondsLeft(): number {
  const text = screen.getByText(/^\d:\d\d$/).textContent ?? '0:00';
  const [m, s] = text.split(':');
  return Number(m) * 60 + Number(s);
}

describe('Find devices', () => {
  it('shows the devices around the phone, the closest first', async () => {
    radio.push(matterDevice('far', -90, 1111, 'manufacturer'));
    radio.push(matterDevice('close', -50, 2222, 'manufacturer'));
    await openSearch();
    const rows = await screen.findAllByText(/Matter device · disc/);
    expect(rows.map((r) => r.textContent)).toEqual([
      'Matter device · disc 2222', // the stronger signal comes first
      'Matter device · disc 1111',
    ]);
    expect(screen.getByText('very close')).toBeTruthy();
    expect(screen.getByText('far')).toBeTruthy();
  });

  it('will not offer to add a device that is too far away', async () => {
    radio.push(matterDevice('far', -90, 1111, 'manufacturer'));
    await openSearch();
    expect(await screen.findByText(/Too far from the device/)).toBeTruthy();
    expect(screen.queryByLabelText('Add to the hub')).toBeNull();
  });

  it('hands the bytes it read to the hub', async () => {
    radio.push(matterDevice('close', -50, 2222, 'manufacturer'));
    await openSearch();
    fireEvent.click(await screen.findByLabelText('Add to the hub'));
    expect(await screen.findByText('The hub is adding the device')).toBeTruthy();
    expect(hub.matterCalls).toEqual([{ kind: MATTER_PAYLOAD_MANUFACTURER, payload: ONBOARDING }]);
  });

  it('shows what the hub says about the attempt', async () => {
    radio.push(matterDevice('close', -50, 2222, 'manufacturer'));
    await openSearch();
    fireEvent.click(await screen.findByLabelText('Add to the hub'));
    expect(await screen.findByText('The hub is adding the device')).toBeTruthy();
    // The Matter chip answers: the device is in the hub.
    act(() => hub.simulateMatter('done', 'ok', 0x0300));
    expect(await screen.findByText('The device is in the hub now.')).toBeTruthy();
  });

  it('says why the attempt failed, in words a person can act on', async () => {
    radio.push(matterDevice('close', -50, 2222, 'manufacturer'));
    await openSearch();
    fireEvent.click(await screen.findByLabelText('Add to the hub'));
    await screen.findByText('The hub is adding the device');
    act(() => hub.simulateMatter('failed', 'no_device'));
    expect(await screen.findByText(/The device did not answer\. Wake it up and try again\./)).toBeTruthy();
  });

  it('stops offering the hub once the hub says it cannot do it', async () => {
    radio.push(matterDevice('close', -50, 2222, 'manufacturer'));
    await openSearch();
    fireEvent.click(await screen.findByLabelText('Add to the hub'));
    await screen.findByText('The hub is adding the device');
    // The Matter chip is alive but has no commissioner.
    act(() => hub.simulateMatter('failed', 'unsupported'));
    expect(await screen.findByText(/This hub's Matter chip cannot add devices yet\./)).toBeTruthy();
    expect(screen.queryByLabelText('Add to the hub')).toBeNull();
    // And the way that does work today is named.
    expect(screen.getAllByText(/open Home → Add Accessory/).length).toBeGreaterThan(0);
  });

  it('knows on the next visit that the hub cannot add Matter devices', async () => {
    radio.push(matterDevice('close', -50, 2222, 'manufacturer'));
    await openSearch();
    fireEvent.click(await screen.findByLabelText('Add to the hub'));
    await screen.findByText('The hub is adding the device');
    act(() => hub.simulateMatter('failed', 'unsupported'));
    await screen.findByText(/This hub's Matter chip cannot add devices yet\./);
    // Leave the screen and come back: the hub is asked what it can do.
    fireEvent.click(screen.getByLabelText('Back'));
    await waitFor(() => expect(screen.queryByText('Find devices')).toBeNull());
    fireEvent.click(screen.getByLabelText('Add a device'));
    await screen.findByText('Find devices');
    expect(await screen.findByText(/This hub's Matter chip cannot add devices yet\./)).toBeTruthy();
    expect(screen.queryByLabelText('Add to the hub')).toBeNull();
  });

  it('opens the Zigbee network, counts down, and shows what joined', async () => {
    await openSearch();
    expect(await screen.findByText(/Press the pair button/)).toBeTruthy();
    expect(hub.pairCalls).toEqual([120]);
    expect(screen.getByText('2:00')).toBeTruthy();
    // The clock runs.
    await waitFor(() => expect(secondsLeft()).toBeLessThan(120), { timeout: 4000 });
    // A device joins: it shows up with the way to make sure it is the right one.
    act(() => hub.simulateJoin(0x0200, 'new_plug'));
    await screen.findByText('new_plug');
    expect(screen.getByText(/joined/)).toBeTruthy();
    // Blink: the hub is asked to make it flash, so the person can recognize it.
    fireEvent.click(screen.getByLabelText('Blink'));
    await waitFor(() => expect(hub.identifyCalls).toEqual([0x0200]));
    // And it can be named right here.
    fireEvent.change(screen.getByLabelText('Name for new_plug'), { target: { value: 'shelf_plug' } });
    fireEvent.click(screen.getByLabelText('Save'));
    await waitFor(() => expect(hub.entities.find((e) => e.id === 0x0200)?.name).toBe('shelf_plug'));
  });

  it('says plainly that a device without Bluetooth cannot be added here', async () => {
    radio.push({
      id: 'other',
      name: 'Some speaker',
      rssi: -60,
      median: -60,
      proximity: 'close',
      lastSeen: Date.now(),
      samples: [-60],
      matter: null,
    });
    await openSearch();
    expect(await screen.findByText('Some speaker')).toBeTruthy();
    expect(screen.getByText(/Only Matter devices can be added over Bluetooth/)).toBeTruthy();
  });

  it('goes back to the devices', async () => {
    await openSearch();
    fireEvent.click(screen.getByLabelText('Back'));
    await waitFor(() => expect(screen.queryByText('Find devices')).toBeNull());
    expect(screen.getByLabelText('Add a device')).toBeTruthy();
  });
});
