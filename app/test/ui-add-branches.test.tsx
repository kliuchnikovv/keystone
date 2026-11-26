// @vitest-environment jsdom
// The «+» menu has three doors, and each opens its own screen.
import './setup-dom';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { WebSocket as NodeWebSocket } from 'ws';
import type { SocketLike } from '../src/protocol/client';
import { Home } from '../src/ui/Home';
import { MockHub } from '../tools/mock-hub/hub';

const wsFactory = (url: string) => new NodeWebSocket(url) as unknown as SocketLike;

let hub: MockHub;

afterEach(async () => {
  cleanup();
  await hub?.close();
});

async function openApp() {
  hub = await MockHub.start({ helloTimeoutMs: 500, ackDelayMs: 10 });
  render(
    <Home
      settings={{ url: hub.url, token: hub.token }}
      onChangeHub={() => {}}
      clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 }, requestTimeoutMs: 2000 }}
    />,
  );
  await screen.findByText('plug1');
}

/** Open the «+» menu. */
function openAddMenu() {
  fireEvent.click(screen.getByLabelText('Add'));
}

describe('the add menu', () => {
  it('Device opens the search for devices', async () => {
    await openApp();
    openAddMenu();
    fireEvent.click(screen.getByLabelText('Device'));
    await screen.findByText('Looking for new devices…');
  });

  it('Scene opens the new-scene form', async () => {
    await openApp();
    openAddMenu();
    fireEvent.click(screen.getByLabelText('Scene'));
    // The form is up: it asks for a name and lists the devices.
    await screen.findByLabelText('Scene name');
    expect(screen.getByText('New scene')).toBeTruthy();
  });

  it('Automation opens the choice of a kind', async () => {
    await openApp();
    openAddMenu();
    fireEvent.click(screen.getByLabelText('Automation'));
    await screen.findByText('What should trigger it?');
    expect(screen.getByLabelText('Schedule')).toBeTruthy();
    expect(screen.getByLabelText('Timer')).toBeTruthy();
    expect(screen.getByLabelText('Rule')).toBeTruthy();
  });

  it('Automation leads to the rule editor', async () => {
    await openApp();
    openAddMenu();
    fireEvent.click(screen.getByLabelText('Automation'));
    await screen.findByText('What should trigger it?');
    fireEvent.click(screen.getByLabelText('Rule'));
    // The rule editor: the WHEN block.
    await waitFor(() => expect(screen.getByText('WHEN')).toBeTruthy());
  });

  it('the dock is hidden while a branch is open', async () => {
    await openApp();
    openAddMenu();
    fireEvent.click(screen.getByLabelText('Automation'));
    await screen.findByText('What should trigger it?');
    expect(screen.queryByLabelText('Settings')).toBeNull();
  });
});
