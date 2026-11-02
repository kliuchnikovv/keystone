// @vitest-environment jsdom
import './setup-dom';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import type { SocketLike } from '../src/protocol/client';
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

const click = (label: string) => fireEvent.click(screen.getByLabelText(label));
const type = (label: string, value: string) => fireEvent.change(screen.getByLabelText(label), { target: { value } });

async function openHub(props: { onUseRelay?(url: string): void } = {}) {
  render(
    <Home
      settings={{ url: hub.url, token: hub.token }}
      onChangeHub={() => {}}
      {...props}
      clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 }, requestTimeoutMs: 2000 }}
    />,
  );
  await screen.findByText('plug1');
  click('Hub');
  await screen.findByLabelText('Firmware');
}

describe('the hub tab', () => {
  it('shows the firmware and the co-processors', async () => {
    await openHub();
    expect(screen.getByLabelText('Firmware')).toBeTruthy();
    expect(screen.getByText('mock-home')).toBeTruthy();
  });

  it('the token is hidden until it is asked for', async () => {
    await openHub();
    await screen.findByLabelText('Show the token');
    expect(screen.queryByText(hub.token)).toBeNull();
    click('Show the token');
    expect(screen.getByText(hub.token)).toBeTruthy();
    click('Hide the token');
    expect(screen.queryByText(hub.token)).toBeNull();
  });

  it('sets up a relay and takes it away again', async () => {
    await openHub();
    click('Set up relay');
    await screen.findByLabelText('Relay server');
    type('Relay server', 'relay.example.com');
    type('Hub ID', 'myhub');
    // The secret is only for a relay the person runs themselves.
    click('I run my own relay');
    type('Secret', 'shared-secret');
    click('Save');
    await waitFor(() => expect(screen.getByText('wss://relay.example.com/tunnel/myhub')).toBeTruthy());
    click('Remove relay');
    await waitFor(() => expect(screen.getByText('not set up')).toBeTruthy());
  });

  it('shows the address the app uses through the relay', async () => {
    const used: string[] = [];
    await openHub({ onUseRelay: (u) => used.push(u) });
    click('Set up relay');
    await screen.findByLabelText('Relay server');
    type('Relay server', 'relay.example.com');
    type('Hub ID', 'home');
    click('Save');
    // The hub's own address ends in /tunnel/, the app's one in /client/.
    await waitFor(() => expect(screen.getByText('wss://relay.example.com/client/home')).toBeTruthy());
    click('Connect through the relay');
    expect(used).toEqual(['wss://relay.example.com/client/home']);
  });

  it('sets up a relay without a secret — the hub uses the one in its firmware', async () => {
    await openHub();
    click('Set up relay');
    await screen.findByLabelText('Relay server');
    type('Relay server', 'relay.example.com');
    type('Hub ID', 'home');
    // No secret: the person who received the box never types one.
    click('Save');
    await waitFor(() => expect(screen.getByText('wss://relay.example.com/client/home')).toBeTruthy());
  });

  it('starts an update and follows it', async () => {
    await openHub();
    click('Update from URL');
    await screen.findByLabelText('Image URL');
    type('Image URL', 'https://example.com/firmware.bin');
    type('SHA-256', 'a'.repeat(64));
    click('Start update');
    await waitFor(() => expect(screen.getByText(/downloading/)).toBeTruthy());
  });

  it('refuses an update without a proper image', async () => {
    await openHub();
    click('Update from URL');
    await screen.findByLabelText('Image URL');
    type('Image URL', 'not-a-url');
    type('SHA-256', 'zz');
    click('Start update');
    expect(await screen.findByText(/Invalid value/)).toBeTruthy();
  });

  it('reboots only after a second tap', async () => {
    await openHub();
    click('Reboot the hub');
    expect(screen.getByText('Tap again to reboot')).toBeTruthy();
    expect(hub.rebootCalls).toBe(0);
    click('Tap again to reboot the hub');
    await waitFor(() => expect(hub.rebootCalls).toBe(1));
  });

  it('a factory reset asks first and says what it does', async () => {
    await openHub();
    click('Factory reset the hub');
    click('Tap again to erase everything');
    expect(await screen.findByText('Factory reset the hub?')).toBeTruthy();
    expect(screen.getByText(/forgotten/)).toBeTruthy();
    click('Cancel');
    expect(hub.factoryResetCalls).toBe(0);
    click('Factory reset the hub');
    click('Tap again to erase everything');
    click('Factory reset');
    await waitFor(() => expect(hub.factoryResetCalls).toBe(1));
  });

  it('forgetting the Wi-Fi asks first', async () => {
    await openHub();
    click('Forget the Wi-Fi network');
    expect(await screen.findByText('Forget the Wi-Fi network?')).toBeTruthy();
    click('Forget Wi-Fi');
    await waitFor(() => expect(hub.wifiForgetCalls).toBe(1));
  });
});
