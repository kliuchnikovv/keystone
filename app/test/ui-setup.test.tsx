// @vitest-environment jsdom
import './setup-dom';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import { PinUnsupportedError, type SocketFactory, type SocketLike } from '../src/protocol/client';
import type { Settings } from '../src/settings';
import { ConnectScreen } from '../src/ui/ConnectScreen';
import { SetupScreen } from '../src/ui/SetupScreen';
import { MockHub } from '../tools/mock-hub/hub';

const CODE = 'HGG5YKJN2MFU';
const wsFactory: SocketFactory = (url) => new NodeWebSocket(url) as unknown as SocketLike;
let hub: MockHub;

beforeEach(async () => {
  hub = await MockHub.start({ setupCode: CODE, token: 'factory-token-0', helloTimeoutMs: 800 });
  hub.wifi = null;
});
afterEach(async () => {
  cleanup();
  await hub.close();
});

const type = (label: string, value: string) => fireEvent.change(screen.getByLabelText(label), { target: { value } });
const click = (label: string) => fireEvent.click(screen.getByLabelText(label));

function open(over: { onDone?: (s: Settings) => void; onCancel?: () => void; socketFactory?: SocketFactory; url?: string } = {}) {
  const onDone = over.onDone ?? vi.fn();
  const onCancel = over.onCancel ?? vi.fn();
  render(<SetupScreen onDone={onDone} onCancel={onCancel} socketFactory={over.socketFactory ?? wsFactory} url={over.url ?? hub.url} />);
  return { onDone, onCancel };
}

describe('first-time setup screen', () => {
  it('walks from the code, through the networks, to a hub on the home network', async () => {
    const { onDone } = open();
    expect(screen.getByText(/join it\. Its password is the setup code/)).toBeTruthy();
    type('Setup code', 'hgg5-ykjn-2mfu');
    click('Connect to the hub');
    await screen.findByLabelText('Network home');
    expect(screen.getByLabelText('Network cafe')).toBeTruthy(); // the scan ran by itself, strongest first

    click('Network home');
    type('Wi-Fi password', 'goodpass1');
    click('Set up the hub');
    await screen.findByText(/The hub is joining home\./);

    expect(hub.setupRequests).toHaveLength(1);
    const req = hub.setupRequests[0]!;
    expect(req).toMatchObject({ ssid: 'home', password: 'goodpass1', kept: true });
    expect(req.token).toMatch(/^[0-9a-f]{32}$/); // an invented token the person never sees

    click('Connect');
    expect(onDone).toHaveBeenCalledTimes(1);
    const s = (onDone as ReturnType<typeof vi.fn>).mock.calls[0]![0] as Settings;
    expect(s.token).toBe(req.token); // what the hub now accepts is what the app will use
    expect(s.url).toBe('wss://keystone.local:8443/ws');
  });

  it('a code that cannot be one is explained without bothering the hub', async () => {
    open();
    type('Setup code', 'HGG5-YKJN');
    click('Connect to the hub');
    expect(await screen.findByText(/12 characters \(you typed 8\)/)).toBeTruthy();
    expect(hub.helloCount).toBe(0);
  });

  it('a wrong code says so and lets the person try again', async () => {
    open();
    type('Setup code', 'AAAA-AAAA-AAAA');
    click('Connect to the hub');
    expect(await screen.findByText(/not this hub’s setup code/)).toBeTruthy();
    expect(screen.getByLabelText('Setup code')).toBeTruthy(); // still on the first step
    type('Setup code', 'HGG5-YKJN-2MFU');
    click('Connect to the hub');
    await screen.findByLabelText('Network home');
  });

  it('a hub that does not answer is blamed on the Wi-Fi the phone is on', async () => {
    const url = hub.url;
    await hub.close();
    hub = await MockHub.start({ setupCode: CODE });
    open({ url });
    type('Setup code', 'HGG5-YKJN-2MFU');
    click('Connect to the hub');
    expect(await screen.findByText(/Is your phone on the hub’s Wi-Fi/, undefined, { timeout: 12_000 })).toBeTruthy();
  }, 20_000);

  it('a build that cannot learn a certificate says what to do about it', async () => {
    const refuse: SocketFactory = () => {
      throw new PinUnsupportedError();
    };
    open({ socketFactory: refuse, url: 'wss://192.168.4.1:8443/ws' });
    type('Setup code', 'HGG5-YKJN-2MFU');
    click('Connect to the hub');
    expect(await screen.findByText(/needs the native Keystone module/)).toBeTruthy();
  });

  it('a bad network name or password is caught before anything is sent', async () => {
    open();
    type('Setup code', 'HGG5-YKJN-2MFU');
    click('Connect to the hub');
    await screen.findByLabelText('Network home');
    type('Network name', 'home');
    type('Wi-Fi password', 'short');
    click('Set up the hub');
    expect(await screen.findByText(/8 to 63 characters/)).toBeTruthy();
    expect(hub.setupRequests).toEqual([]);
  });

  it('Back from the first step leaves the setup', () => {
    const { onCancel } = open();
    click('Back');
    expect(onCancel).toHaveBeenCalled();
  });

  it('a network can be typed for a hidden one', async () => {
    const { onDone } = open();
    type('Setup code', 'HGG5-YKJN-2MFU');
    click('Connect to the hub');
    await screen.findByLabelText('Network home');
    type('Network name', 'cafe');
    click('Set up the hub');
    await screen.findByText(/The hub is joining cafe\./);
    expect(hub.setupRequests[0]).toMatchObject({ ssid: 'cafe', password: '', kept: true }); // an open network
    await waitFor(() => expect(onDone).not.toHaveBeenCalled());
  });
});

describe('the connect screen offers the setup', () => {
  it('shows "Set up a new hub" only when it can start one', () => {
    const onSetup = vi.fn();
    render(<ConnectScreen onSave={() => {}} onSetup={onSetup} />);
    click('Set up a new hub');
    expect(onSetup).toHaveBeenCalled();
    cleanup();
    render(<ConnectScreen onSave={() => {}} />);
    expect(screen.queryByLabelText('Set up a new hub')).toBeNull();
  });
});
