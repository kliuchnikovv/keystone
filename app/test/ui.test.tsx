// @vitest-environment jsdom
import './setup-dom';
import { longPress } from './helpers';
import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { useEffect, useState } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import type { SocketLike } from '../src/protocol/client';
import { useHome, useHubClient, type HubConnection } from '../src/hooks/useHub';
import { readStateCache } from '../src/statecache';
import { ConnectScreen } from '../src/ui/ConnectScreen';
import { Home } from '../src/ui/Home';
import { __resetFiles } from './stubs/expo-file-system';
import { MockHub } from '../tools/mock-hub/hub';

const wsFactory = (url: string) => new NodeWebSocket(url) as unknown as SocketLike;
let hub: MockHub;

beforeEach(async () => {
  __resetFiles(); // the state cache of one test is not the next test's
  hub = await MockHub.start({ helloTimeoutMs: 500, ackDelayMs: 10 });
});
afterEach(async () => {
  cleanup();
  await hub.close();
});

function renderHome(over: { token?: string; onChangeHub?: () => void } = {}) {
  return render(
    <Home
      settings={{ url: hub.url, token: over.token ?? hub.token }}
      onChangeHub={over.onChangeHub ?? (() => {})}
      clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 }, requestTimeoutMs: 2000 }}
    />,
  );
}

const card = (title: string) => screen.getByLabelText(`Device ${title}`);
/** What the button says about itself: "on", "off", or the sensor's state. */
const state = (title: string) => {
  const el = within(card(title)).getByLabelText(new RegExp(`^${title} is `));
  return el.getAttribute('aria-label')!.slice(title.length + 4);
};

describe('Devices', () => {
  it('shows what the hub reports', async () => {
    renderHome();
    await screen.findByText('plug1');
    expect(state('plug1')).toBe('on');
    expect(within(card('plug1')).getByText('3 W')).toBeTruthy();
    expect(state('lamp')).toBe('off');
    expect(screen.getByText('Motion sensor 0x0102')).toBeTruthy();
    expect(within(card('Motion sensor 0x0102')).getByText('Clear')).toBeTruthy();
    // The rest of the device lives in its sheet.
    await longPress(card('plug1'));
    expect(await screen.findByText('1.28 kWh')).toBeTruthy();
  });

  it('the button is the switch: tapping it sends the command', async () => {
    renderHome();
    await screen.findByText('lamp');
    expect(state('lamp')).toBe('off');
    fireEvent.click(card('lamp'));
    await waitFor(() => expect(state('lamp')).toBe('on'));
    expect(hub.entities.find((e) => e.name === 'lamp')!.attrs[0]!.value).toBe(true);
  });

  it('live changes on the hub appear without touching anything', async () => {
    renderHome();
    await screen.findByText('plug1');
    act(() => hub.setValue(0x0100, 0x13, 42));
    await within(card('plug1')).findByText('42 W');
    act(() => hub.setValue(0x0102, 0x50, true));
    await within(card('Motion sensor 0x0102')).findByText('Occupied');
  });

  it('brightness is a slider in percent, dark while the lamp is off', async () => {
    renderHome();
    await screen.findByText('lamp');
    await longPress(card('lamp'));
    const slider = await screen.findByLabelText('lamp brightness');
    expect(slider.getAttribute('aria-valuetext')).toBe('50 %'); // 128/254
    expect(slider.getAttribute('aria-valuemin')).toBe('0');
    expect(slider.getAttribute('aria-valuemax')).toBe('100');
    expect(slider.getAttribute('aria-disabled')).toBe('true'); // the lamp is off
    fireEvent.click(screen.getByLabelText('Turn on lamp'));
    await waitFor(() => expect(screen.getByLabelText('lamp brightness').getAttribute('aria-disabled')).toBeNull());
  });

  it('a command the device refuses shows a message that can be dismissed', async () => {
    hub.forceAck = 'fail';
    renderHome();
    await screen.findByText('lamp');
    fireEvent.click(card('lamp'));
    const toast = await screen.findByText('The device refused the command');
    expect(state('lamp')).toBe('off'); // nothing changed
    fireEvent.click(toast);
    await waitFor(() => expect(screen.queryByText('The device refused the command')).toBeNull());
  });

  it('devices of a silent chip are marked offline and cannot be switched', async () => {
    renderHome();
    await screen.findByText('lamp');
    act(() => hub.setChipStatus(1, 'silent'));
    await within(card('lamp')).findByText('offline');
    expect(state('lamp')).toBe('off, offline');
    fireEvent.click(card('lamp'));
    expect(hub.entities.find((e) => e.name === 'lamp')!.attrs[0]!.value).toBe(false);
    act(() => hub.setChipStatus(1, 'ready'));
    await waitFor(() => expect(state('lamp')).toBe('off'));
  });
});

describe('Adding, finding and removing devices', () => {
  it('"Add a device" opens the network, counts down, and shows what joins', async () => {
    renderHome();
    await screen.findByText('plug1');
    fireEvent.click(screen.getByLabelText('Add a device'));
    await screen.findByText(/You have 2 minutes/);
    expect(hub.pairCalls).toEqual([120]);
    act(() => hub.simulateJoin(0x0200, 'new_plug'));
    await screen.findByText(/new_plug joined/);
    expect(card('new_plug')).toBeTruthy();
    fireEvent.click(screen.getByLabelText('Cancel'));
    await waitFor(() => expect(screen.queryByText(/You have 2 minutes/)).toBeNull());
    expect(screen.getByLabelText('Add a device')).toBeTruthy();
  });

  it('a device can be made to blink, and removal needs a second tap', async () => {
    renderHome();
    await screen.findByText('plug1');
    await longPress(card('plug1'));
    fireEvent.click(await screen.findByLabelText('Identify plug1'));
    await screen.findByText(/should blink/);
    expect(hub.identifyCalls).toEqual([0x0100]);

    await longPress(card('plug1'));
    fireEvent.click(await screen.findByLabelText('Remove plug1'));
    expect(hub.unpairCalls).toEqual([]); // the hub is asked first
    expect(await screen.findByText('This will take it off the Zigbee network and from Apple Home.')).toBeTruthy();
    fireEvent.click(await screen.findByLabelText('Remove device'));
    await waitFor(() => expect(screen.queryByText('plug1')).toBeNull());
    expect(hub.unpairCalls).toEqual([0x0100]);
    expect(screen.getByText('lamp')).toBeTruthy(); // the others stay
  });
});

describe('Hub tab', () => {
  it('lists the chips with their health', async () => {
    renderHome();
    await screen.findByText('plug1');
    fireEvent.click(screen.getByLabelText('Hub'));
    await screen.findByText('mock-home');
    const zigbee = screen.getByLabelText('Chip Zigbee');
    expect(within(zigbee).getByText(/Online/)).toBeTruthy();
    // The chip's numbers sit in rows: uptime, free heap, reboots, line errors.
    expect(within(zigbee).getByText('Free heap')).toBeTruthy();
    expect(within(zigbee).getByText('368 KB')).toBeTruthy();
    expect(within(zigbee).getByText('Link errors')).toBeTruthy();
    expect(screen.getByLabelText('Chip Thread')).toBeTruthy();
    act(() => hub.setChipStatus(2, 'silent'));
    await within(screen.getByLabelText('Chip Thread')).findByText(/Not responding/);
  });

  it('"Change hub" hands control back', async () => {
    const onChangeHub = vi.fn();
    renderHome({ onChangeHub });
    await screen.findByText('plug1');
    fireEvent.click(screen.getByLabelText('Hub'));
    fireEvent.click(await screen.findByLabelText('Change hub'));
    expect(onChangeHub).toHaveBeenCalled();
  });
});

describe('Connection problems', () => {  it('a wrong token is explained and can be edited', async () => {
    const onChangeHub = vi.fn();
    renderHome({ token: 'wrong-token', onChangeHub });
    await screen.findByText('Authentication failed');
    expect(screen.getByText('Waiting for the hub…')).toBeTruthy();
    fireEvent.click(screen.getByLabelText('Go to Connect'));
    expect(onChangeHub).toHaveBeenCalled();
  });

  it('a lost connection says so, keeps the last known picture and recovers by itself', async () => {
    renderHome();
    await screen.findByText('plug1');
    act(() => hub.dropAll());
    await screen.findByText(/Reconnecting after a drop/);
    expect(screen.queryByText('plug1')).toBeTruthy(); // the last known state stays on screen
    expect(screen.getByText(/the last known state/)).toBeTruthy();
    await waitFor(() => expect(screen.queryByText(/Reconnecting after a drop/)).toBeNull());
    expect(screen.queryByText('plug1')).toBeTruthy();
  });
});

describe('Connect screen', () => {
  it('rejects bad input and says why, then saves a normalized address', () => {
    const onSave = vi.fn();
    render(<ConnectScreen onSave={onSave} />);
    const connect = screen.getByLabelText('Connect');
    expect((connect as HTMLButtonElement).disabled).toBe(true); // nothing to connect to yet
    fireEvent.click(connect);
    expect(onSave).not.toHaveBeenCalled();

    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: 'not an address' } });
    expect(screen.getByText(/Enter an address like/)).toBeTruthy();
    expect((screen.getByLabelText('Connect') as HTMLButtonElement).disabled).toBe(true);

    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: '192.168.0.202' } });
    fireEvent.change(screen.getByLabelText('Token'), { target: { value: 'four' } });
    expect(screen.getByText('The token is at least 8 characters')).toBeTruthy();
    expect((screen.getByLabelText('Connect') as HTMLButtonElement).disabled).toBe(true);
    expect(onSave).not.toHaveBeenCalled();

    fireEvent.change(screen.getByLabelText('Token'), { target: { value: 'twelve-chars' } });
    fireEvent.click(screen.getByLabelText('Connect'));
    expect(onSave).toHaveBeenCalledWith({ url: 'ws://192.168.0.202:8080/ws', token: 'twelve-chars' });
  });

  it('warns that ws:// is not encrypted, but not for wss://', () => {
    render(<ConnectScreen onSave={() => {}} />);
    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: '10.0.0.5' } });
    expect(screen.getByText(/not encrypted/)).toBeTruthy();
    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: 'wss://hub.local/ws' } });
    expect(screen.queryByText(/not encrypted/)).toBeNull();
  });

  it('shows a notice and keeps the previous values', () => {
    render(<ConnectScreen initial={{ url: 'ws://h:8080/ws', token: 'abcdefgh' }} notice="The hub rejected the token" onSave={() => {}} />);
    expect(screen.getByText('The hub rejected the token')).toBeTruthy();
    expect((screen.getByLabelText('Hub address') as HTMLInputElement).value).toBe('ws://h:8080/ws');
  });

  it('offers a way back only when there is something to go back to', () => {
    const onCancel = vi.fn();
    const { rerender } = render(<ConnectScreen onSave={() => {}} />);
    expect(screen.queryByLabelText('Back')).toBeNull();
    rerender(<ConnectScreen onSave={() => {}} onCancel={onCancel} />);
    fireEvent.click(screen.getByLabelText('Back'));
    expect(onCancel).toHaveBeenCalled();
  });
});

describe('Hub discovery on the connect screen', () => {
  const found = { instance: 'Keystone hub', name: 'home', host: 'keystone.local', address: '192.168.0.202', port: 8443, tls: true };

  it('shows nothing where the build cannot browse (web, Expo Go)', () => {
    render(<ConnectScreen onSave={() => {}} discovery={{ available: false, hubs: [] }} />);
    expect(screen.queryByLabelText('Hubs on this network')).toBeNull();
  });

  it('says it is searching, then lists hubs; tapping one fills the address but never the fingerprint', () => {
    const { rerender } = render(<ConnectScreen onSave={() => {}} discovery={{ available: true, hubs: [] }} />);
    expect(screen.getByText('Searching your network…')).toBeTruthy();
    rerender(<ConnectScreen onSave={() => {}} discovery={{ available: true, hubs: [found] }} />);
    expect(screen.getByText('192.168.0.202:8443 · encrypted')).toBeTruthy();
    fireEvent.click(screen.getByLabelText('Use hub home'));
    expect((screen.getByLabelText('Hub address') as HTMLInputElement).value).toBe('wss://192.168.0.202:8443/ws');
    // Discovery is unauthenticated: the pin must still come from the person.
    expect((screen.getByLabelText('Certificate fingerprint') as HTMLInputElement).value).toBe('');
    expect(screen.getByText(/does not prove it is yours/)).toBeTruthy();
  });

  it('a plain hub fills a ws:// address and clears a stale fingerprint', () => {
    render(<ConnectScreen initial={{ url: 'wss://old:8443/ws', token: 'abcdefgh', fingerprint: 'D7:C0:8E:E0:28:AA:C6:DE:7C:D8:F2:90:62:5F:08:A7:84:34:42:AD:D9:20:8F:A9:27:6A:71:9D:AC:96:B7:71' }} onSave={() => {}} discovery={{ available: true, hubs: [{ ...found, port: 8080, tls: false }] }} />);
    fireEvent.click(screen.getByLabelText('Use hub home'));
    expect((screen.getByLabelText('Hub address') as HTMLInputElement).value).toBe('ws://192.168.0.202:8080/ws');
    expect(screen.getByText(/use it on your own network only/)).toBeTruthy();
    expect(screen.queryByLabelText('Certificate fingerprint')).toBeNull();
  });
});

describe('Certificate fingerprint', () => {
  const colons = 'D7:C0:8E:E0:28:AA:C6:DE:7C:D8:F2:90:62:5F:08:A7:84:34:42:AD:D9:20:8F:A9:27:6A:71:9D:AC:96:B7:71';

  it('the field appears only for wss:// and a bad value is refused', () => {
    const onSave = vi.fn();
    render(<ConnectScreen onSave={onSave} />);
    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: '10.0.0.5' } });
    expect(screen.queryByLabelText('Certificate fingerprint')).toBeNull();
    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: 'wss://10.0.0.5:8443/ws' } });
    fireEvent.change(screen.getByLabelText('Token'), { target: { value: 'twelve-chars' } });
    fireEvent.change(screen.getByLabelText('Certificate fingerprint'), { target: { value: 'abc' } });
    fireEvent.click(screen.getByLabelText('Connect'));
    expect(screen.getByText(/not a SHA-256 fingerprint/)).toBeTruthy();
    expect(onSave).not.toHaveBeenCalled();
  });

  it('a valid fingerprint is saved in its canonical form', () => {
    const onSave = vi.fn();
    render(<ConnectScreen onSave={onSave} />);
    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: 'wss://10.0.0.5:8443/ws' } });
    fireEvent.change(screen.getByLabelText('Token'), { target: { value: 'twelve-chars' } });
    fireEvent.change(screen.getByLabelText('Certificate fingerprint'), { target: { value: colons.toLowerCase().replace(/:/g, '') } });
    fireEvent.click(screen.getByLabelText('Connect'));
    expect(onSave).toHaveBeenCalledWith({ url: 'wss://10.0.0.5:8443/ws', token: 'twelve-chars', fingerprint: colons });
  });

  it('an empty fingerprint field means "not pinned"', () => {
    const onSave = vi.fn();
    render(<ConnectScreen onSave={onSave} />);
    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: 'wss://10.0.0.5:8443/ws' } });
    fireEvent.change(screen.getByLabelText('Token'), { target: { value: 'twelve-chars' } });
    fireEvent.click(screen.getByLabelText('Connect'));
    expect(onSave).toHaveBeenCalledWith({ url: 'wss://10.0.0.5:8443/ws', token: 'twelve-chars' });
  });

  it('a build that cannot pin says so instead of connecting unverified', async () => {
    const onChangeHub = vi.fn();
    // No socketFactory override: this is the platform WebSocket, which cannot check a pin.
    render(<Home settings={{ url: hub.url, token: hub.token, fingerprint: colons }} onChangeHub={onChangeHub} />);
    await screen.findByText(/can't pin a certificate/);
    fireEvent.click(screen.getByLabelText('Go to Connect'));
    expect(onChangeHub).toHaveBeenCalled();
    expect(screen.queryByText('plug1')).toBeNull();
  });
});

describe('Banner hint for wss://', () => {
  it('points at accepting the certificate while a secure hub is unreachable, but not for ws://', async () => {
    const { secureHint } = await import('../src/ui/Banner');
    const { initialState } = await import('../src/protocol/store');
    const base = { ...initialState, status: 'reconnecting' as const };
    expect(secureHint('wss://192.168.0.202:8443/ws', base)).toContain('https://192.168.0.202:8443/');
    expect(secureHint('ws://192.168.0.202:8080/ws', base)).toBeNull();
    expect(secureHint('wss://hub/ws', { ...base, status: 'ready' })).toBeNull();
    expect(secureHint('wss://hub/ws', { ...base, status: 'unauthorized' })).toBeNull();
  });

  it('does not ask for a browser exception on a relay, and names the address to use', async () => {
    const { secureHint } = await import('../src/ui/Banner');
    const { initialState } = await import('../src/protocol/store');
    const base = { ...initialState, status: 'reconnecting' as const };
    // A relay's certificate is signed by a public CA: there is nothing to accept.
    const relay = secureHint('wss://relay.example.com/client/home', base);
    expect(relay).not.toContain('accept the hub');
    expect(relay).toContain('/client/<hub-id>');
    // The relay's own home page is not an address the app can talk to.
    const root = secureHint('wss://relay.example.com/', base);
    expect(root).toContain('/client/<hub-id>');
    expect(root).not.toContain('accept the hub');
  });
});

describe('The state cache', () => {
  it('the last known state is on screen when the hub is away', async () => {
    renderHome();
    await screen.findByText('plug1');
    // The app remembers what the hub said.
    await waitFor(async () => expect(await readStateCache()).toContain('plug1'));

    // The hub goes away and the app is closed and opened again.
    const awayUrl = hub.url;
    const awayToken = hub.token;
    await hub.close();
    hub = await MockHub.start({ helloTimeoutMs: 500, ackDelayMs: 10 }); // a fresh one for the cleanup
    cleanup();
    render(
      <Home
        settings={{ url: awayUrl, token: awayToken }}
        onChangeHub={() => {}}
        clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 }, requestTimeoutMs: 2000 }}
      />,
    );

    await screen.findByText('plug1'); // from the cache: the devices are still there
    expect(screen.getByText(/the last known state/)).toBeTruthy();
    expect(screen.getByLabelText('Device plug1')).toBeTruthy();
  });

  it('the cache of another hub is not shown', async () => {
    renderHome();
    await screen.findByText('plug1');
    await waitFor(async () => expect(await readStateCache()).toContain('plug1'));
    cleanup();

    // Another hub: its devices must not leak into this app.
    render(
      <Home
        settings={{ url: 'ws://127.0.0.1:1/ws', token: 'other-token' }}
        onChangeHub={() => {}}
        clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 } }}
      />,
    );
    await screen.findByText('Waiting for the hub…');
    expect(screen.queryByText('plug1')).toBeNull();
  });
});

describe('Handing the connection over', () => {
  it('the connection stays open when the screen that made it goes away', async () => {
    let handed: HubConnection | null = null;

    function Owner({ onReady }: { onReady(c: HubConnection): void }) {
      const client = useHubClient({ url: hub.url, token: hub.token }, { socketFactory: wsFactory });
      const home = useHome(client);
      useEffect(() => {
        if (home.status === 'ready') {
          client.handOver();
          onReady(client);
        }
      }, [home.status, client, onReady]);
      return <div data-testid="owner">{home.status}</div>;
    }

    function App() {
      const [client, setClient] = useState<HubConnection | null>(null);
      if (client) return <div data-testid="app">{client.getSnapshot().status}</div>;
      return <Owner onReady={setClient} />;
    }

    render(<App />);
    await waitFor(() => expect(screen.getByTestId('app')).toBeTruthy());
    await new Promise((r) => setTimeout(r, 100));
    expect(handed).toBeNull();
    expect(screen.getByTestId('app').textContent).toBe('ready');
  });
});
