// @vitest-environment jsdom
import './setup-dom';
import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import type { SocketLike } from '../src/protocol/client';
import { ConnectScreen } from '../src/ui/ConnectScreen';
import { Home } from '../src/ui/Home';
import { MockHub } from '../tools/mock-hub/hub';

const wsFactory = (url: string) => new NodeWebSocket(url) as unknown as SocketLike;
let hub: MockHub;

beforeEach(async () => {
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

describe('Devices', () => {
  it('shows what the hub reports', async () => {
    renderHome();
    await screen.findByText('plug1');
    expect(within(card('plug1')).getByText(/Plug · online/)).toBeTruthy();
    expect(within(card('plug1')).getByText('3 W')).toBeTruthy();
    expect(within(card('plug1')).getByText('1.28 kWh')).toBeTruthy();
    expect(within(card('lamp')).getByText('50 %')).toBeTruthy(); // brightness 128/254
    expect(screen.getByText('Motion sensor 0x0102')).toBeTruthy();
    expect(within(card('Motion sensor 0x0102')).getByText('Clear')).toBeTruthy();
  });

  it('flipping a switch sends the command and the switch follows the hub', async () => {
    renderHome();
    await screen.findByText('lamp');
    const sw = screen.getByLabelText('lamp power') as HTMLInputElement;
    expect(sw.checked).toBe(false);
    fireEvent.click(sw);
    await waitFor(() => expect((screen.getByLabelText('lamp power') as HTMLInputElement).checked).toBe(true));
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

  it('brightness buttons nudge the level by 10 %', async () => {
    renderHome();
    await screen.findByText('lamp');
    fireEvent.click(screen.getByLabelText('lamp brighter'));
    await within(card('lamp')).findByText('60 %'); // 128 + 25 = 153 -> 60 %
    // The buttons stay locked until the chip has acknowledged the first command.
    await waitFor(() => expect(screen.getByLabelText('lamp dimmer').getAttribute('aria-disabled')).not.toBe('true'));
    fireEvent.click(screen.getByLabelText('lamp dimmer'));
    await within(card('lamp')).findByText('50 %');
  });

  it('a command the device refuses shows a message that can be dismissed', async () => {
    hub.forceAck = 'fail';
    renderHome();
    await screen.findByText('lamp');
    fireEvent.click(screen.getByLabelText('lamp power'));
    const toast = await screen.findByText('The device refused the command');
    expect((screen.getByLabelText('lamp power') as HTMLInputElement).checked).toBe(false); // nothing changed
    fireEvent.click(toast);
    await waitFor(() => expect(screen.queryByText('The device refused the command')).toBeNull());
  });

  it('devices of a silent chip are marked offline and cannot be switched', async () => {
    renderHome();
    await screen.findByText('lamp');
    act(() => hub.setChipStatus(1, 'silent'));
    await within(card('lamp')).findByText(/Light · offline/);
    expect((screen.getByLabelText('lamp power') as HTMLInputElement).disabled).toBe(true);
    expect((screen.getByLabelText('lamp brighter') as HTMLButtonElement).getAttribute('aria-disabled')).toBe('true');
    act(() => hub.setChipStatus(1, 'ready'));
    await within(card('lamp')).findByText(/Light · online/);
    expect((screen.getByLabelText('lamp power') as HTMLInputElement).disabled).toBe(false);
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
    expect(within(zigbee).getByText(/368 KB free/)).toBeTruthy();
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

describe('Connection problems', () => {
  it('a wrong token is explained and can be edited', async () => {
    const onChangeHub = vi.fn();
    renderHome({ token: 'wrong-token', onChangeHub });
    await screen.findByText('The hub rejected the token');
    expect(screen.getByText('Waiting for the hub…')).toBeTruthy();
    fireEvent.click(screen.getByLabelText('Edit connection'));
    expect(onChangeHub).toHaveBeenCalled();
  });

  it('a lost connection says so, drops the stale picture and recovers by itself', async () => {
    renderHome();
    await screen.findByText('plug1');
    act(() => hub.dropAll());
    await screen.findByText('Connection lost; reconnecting…');
    expect(screen.queryByText('plug1')).toBeNull(); // no stale devices shown as current
    await screen.findByText('plug1');
    expect(screen.queryByText('Connection lost; reconnecting…')).toBeNull();
  });
});

describe('Connect screen', () => {
  it('rejects bad input and says why, then saves a normalized address', () => {
    const onSave = vi.fn();
    render(<ConnectScreen onSave={onSave} />);
    fireEvent.click(screen.getByLabelText('Connect'));
    expect(onSave).not.toHaveBeenCalled();
    expect(screen.getByText(/Enter an address like/)).toBeTruthy();

    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: '192.168.0.202' } });
    fireEvent.click(screen.getByLabelText('Connect'));
    expect(screen.getByText('The token is at least 8 characters')).toBeTruthy();
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
});

describe('Hub discovery on the connect screen', () => {
  const found = { instance: 'Keystone hub', name: 'home', host: 'keystone.local', address: '192.168.0.202', port: 8443, tls: true };

  it('shows nothing where the build cannot browse (web, Expo Go)', () => {
    render(<ConnectScreen onSave={() => {}} discovery={{ available: false, hubs: [] }} />);
    expect(screen.queryByLabelText('Hubs on this network')).toBeNull();
  });

  it('says it is searching, then lists hubs; tapping one fills the address but never the fingerprint', () => {
    const { rerender } = render(<ConnectScreen onSave={() => {}} discovery={{ available: true, hubs: [] }} />);
    expect(screen.getByText('Searching…')).toBeTruthy();
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
    await screen.findByText(/can't verify the hub's certificate fingerprint/);
    fireEvent.click(screen.getByLabelText('Edit connection'));
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
});
