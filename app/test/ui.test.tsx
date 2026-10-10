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
