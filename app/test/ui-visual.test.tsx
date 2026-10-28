// @vitest-environment jsdom
// The visual contract of the brief: a status dot by the name, a menu in the corner,
// an empty state with a picture, a token you can look at, and the words of the toasts.
import './setup-dom';
import { longPress } from './helpers';
import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import type { SocketLike } from '../src/protocol/client';
import { ConnectScreen } from '../src/ui/ConnectScreen';
import { DeviceSheet } from '../src/ui/DeviceSheet';
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

describe('Device button', () => {
  it('says how the device is doing', async () => {
    renderHome();
    await screen.findByText('plug1');
    expect(within(card('plug1')).getByLabelText('plug1 is on')).toBeTruthy();
    act(() => hub.setChipStatus(1, 'silent'));
    await within(card('plug1')).findByText('offline');
    act(() => hub.setChipStatus(1, 'ready'));
    await waitFor(() => expect(within(card('plug1')).queryByText('offline')).toBeNull());
  });

  it('lights up while the finger rests, so the long press is counted', async () => {
    renderHome();
    await screen.findByText('plug1');
    const el = card('plug1');
    const chip = screen.getByLabelText('plug1 is on');
    const before = chip.getAttribute('style') ?? '';
    fireEvent.mouseDown(el);
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 320));
    });
    // The button says that it is holding the press: the icon chip fills with the accent.
    const during = chip.getAttribute('style') ?? '';
    expect(during).not.toBe(before);
    expect(during).toContain('rgb(196, 81, 42)');
    fireEvent.mouseUp(el);
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 20));
    });
    expect(chip.getAttribute('style') ?? '').toBe(before);
  });

  it('has a sheet with Rename, Blink and Remove device', async () => {
    renderHome();
    await screen.findByText('plug1');
    await longPress(card('plug1'));
    expect(await screen.findByLabelText('Rename plug1')).toBeTruthy();
    expect(screen.getByLabelText('Identify plug1')).toBeTruthy();
    expect(screen.getByLabelText('Remove plug1')).toBeTruthy();
    // The removal asks first, in words.
    fireEvent.click(screen.getByLabelText('Remove plug1'));
    expect(await screen.findByText('This will take it off the Zigbee network and from Apple Home.')).toBeTruthy();
  });

  it('nudges the brightness with the big buttons', async () => {
    const onSet = vi.fn();
    render(
      <DeviceSheet
        entity={{ id: 1, name: 'lamp', chip: 1, device: 'light', attrs: [{ id: 2, name: 'brightness', vt: 'u8', flags: ['r', 'w'], value: 128 }] }}
        status="online"
        reachable
        visible
        isPending={() => false}
        onSet={onSet}
        onClose={() => {}}
        onRename={async () => true}
      />,
    );
    expect(onSet).not.toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('lamp brightness up'));
    expect(onSet).toHaveBeenCalledWith(2, 152); // 128 = 50 % -> 60 %
    fireEvent.click(screen.getByLabelText('lamp brightness down'));
    expect(onSet).toHaveBeenCalledWith(2, 127); // 60 % -> 50 %
  });
});

describe('Devices list', () => {
  it('offers to add a device at the bottom, in a sheet', async () => {
    renderHome();
    await screen.findByText('plug1');
    const add = screen.getByLabelText('Add a device');
    expect(add).toBeTruthy();
    fireEvent.click(add);
    expect(await screen.findByText(/You have 2 minutes/)).toBeTruthy();
    expect(hub.pairCalls).toEqual([120]);
  });

  it('shows a picture and a sentence when there is nothing yet', async () => {
    hub.entities = [];
    renderHome();
    expect(await screen.findByText('No devices yet')).toBeTruthy();
    expect(screen.getByText('Tap below to add your first one.')).toBeTruthy();
  });
});

describe('Scenes', () => {
  it('says in a toast how many settings went out', async () => {
    hub.scenes.push({ name: 'night', actions: [{ entity: 0x0100, attr: 1, value: false }] });
    renderHome();
    await screen.findByLabelText('Apply scene night');
    fireEvent.click(screen.getByLabelText('Apply scene night'));
    expect(await screen.findByText("Applied 'night': 1 settings sent")).toBeTruthy();
  });
});

describe('Connect screen', () => {
  it('lets the person look at the token', () => {
    render(<ConnectScreen onSave={() => {}} />);
    const token = screen.getByLabelText('Token') as HTMLInputElement;
    expect(token.type).toBe('password');
    fireEvent.click(screen.getByLabelText('Show token'));
    expect((screen.getByLabelText('Token') as HTMLInputElement).type).toBe('text');
    fireEvent.click(screen.getByLabelText('Hide token'));
    expect((screen.getByLabelText('Token') as HTMLInputElement).type).toBe('password');
  });

  it('keeps Connect dark until there is something to connect to', () => {
    render(<ConnectScreen onSave={() => {}} />);
    expect((screen.getByLabelText('Connect') as HTMLButtonElement).disabled).toBe(true);
    fireEvent.change(screen.getByLabelText('Hub address'), { target: { value: '192.168.0.202' } });
    fireEvent.change(screen.getByLabelText('Token'), { target: { value: 'twelve-chars' } });
    expect((screen.getByLabelText('Connect') as HTMLButtonElement).disabled).toBe(false);
  });
});

describe('Banner', () => {
  it('offers a way back to Connect when the token is wrong', async () => {
    const onChangeHub = vi.fn();
    renderHome({ token: 'wrong-token', onChangeHub });
    await screen.findByText('Authentication failed');
    fireEvent.click(screen.getByLabelText('Go to Connect'));
    expect(onChangeHub).toHaveBeenCalled();
  });
});
