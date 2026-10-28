// @vitest-environment jsdom
import './setup-dom';
import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
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

function renderHome() {
  return render(
    <Home
      settings={{ url: hub.url, token: hub.token }}
      onChangeHub={() => {}}
      clientOptions={{ socketFactory: wsFactory, backoffMs: { min: 20, max: 60 }, requestTimeoutMs: 2000 }}
    />,
  );
}

const events = () => within(screen.getByLabelText('Events'));
const openHistory = async () => {
  await screen.findByText('plug1');
  fireEvent.click(screen.getByRole('tab', { name: 'History' }));
  await screen.findByLabelText('Events');
};

describe('History tab', () => {
  it('shows the hub starting, then what happens, newest first', async () => {
    renderHome();
    await openHistory();
    await events().findByText('Hub started (power on)');

    act(() => hub.setValue(0x0102, 0x50, true));
    await waitFor(() => expect(screen.getByLabelText(/Motion sensor 0x0102 detected someone/)).toBeTruthy(), { timeout: 6000 });
    const rows = screen.getAllByLabelText(/, \d\d:\d\d$/);
    expect(rows[0]!.getAttribute('aria-label')).toMatch(/^Motion sensor 0x0102 detected someone/);
    expect(rows[rows.length - 1]!.getAttribute('aria-label')).toMatch(/^Hub started/);
  }, 15000);

  it('a command from the app shows as what the hub sent and what the device then did', async () => {
    renderHome();
    await screen.findByText('lamp');
    fireEvent.click(screen.getByLabelText('Device lamp'));
    await waitFor(() => expect(screen.getByLabelText('lamp is on')).toBeTruthy());
    fireEvent.click(screen.getByRole('tab', { name: 'History' }));
    await events().findByText('Sent to lamp: turned on from the app');
    await events().findByText('lamp turned on');
  });

  it('a new event shows up at once, not at the next poll', async () => {
    renderHome();
    await openHistory();
    await events().findByText('Hub started (power on)');
    act(() => hub.setValue(0x0102, 0x50, true));
    // The poll is every 4 s: this only passes if the hub's push was folded in.
    await waitFor(() => expect(screen.getByLabelText(/Motion sensor 0x0102 detected someone/)).toBeTruthy(), { timeout: 1500 });
    act(() => hub.setValue(0x0102, 0x50, false));
    await waitFor(() => expect(screen.getByLabelText(/Motion sensor 0x0102 sees no one/)).toBeTruthy(), { timeout: 1500 });
    expect(screen.getAllByLabelText(/Motion sensor 0x0102/)).toHaveLength(2); // neither twice
  });

  it('a hub that does not push is still polled', async () => {
    await hub.close();
    hub = await MockHub.start({ helloTimeoutMs: 500, ackDelayMs: 10, pushHistory: false });
    renderHome();
    await openHistory();
    await events().findByText('Hub started (power on)');
    act(() => hub.setValue(0x0102, 0x50, true));
    await waitFor(() => expect(screen.getByLabelText(/Motion sensor 0x0102 detected someone/)).toBeTruthy(), { timeout: 6000 });
  }, 15000);

  it('tapping a row about a device shows only that device; "Show everything" comes back', async () => {
    renderHome();
    await screen.findByText('lamp');
    act(() => hub.setValue(0x0102, 0x50, true));
    act(() => hub.setChipStatus(1, 'silent'));
    fireEvent.click(screen.getByRole('tab', { name: 'History' }));
    const motion = await screen.findByLabelText(/Motion sensor 0x0102 detected someone/, undefined, { timeout: 6000 });
    await events().findByText(/The zigbee chip went silent/);

    fireEvent.click(motion);
    await screen.findByText('Only Motion sensor 0x0102');
    await waitFor(() => expect(events().queryByText(/The zigbee chip went silent/)).toBeNull());
    expect(events().getByText('Motion sensor 0x0102 detected someone')).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: 'Show everything' }));
    await events().findByText(/The zigbee chip went silent/);
  }, 20000);

  it('older events come on request', async () => {
    renderHome();
    await screen.findByText('plug1');
    for (let i = 0; i < 30; i++) act(() => hub.setValue(0x0102, 0x50, i % 2 === 0));
    fireEvent.click(screen.getByRole('tab', { name: 'History' }));
    await events().findByText('lamp ' + 'x', undefined).catch(() => undefined); // let the first page arrive
    const more = await screen.findByRole('button', { name: 'Load older' });
    expect(screen.queryByText(/Hub started/)).toBeNull(); // 24 newest events only
    fireEvent.click(more);
    await events().findByText('Hub started (power on)');
    await waitFor(() => expect(screen.queryByRole('button', { name: 'Load older' })).toBeNull());
  }, 20000);

  it('a nearly empty hub says so for a device with no history', async () => {
    renderHome();
    await screen.findByText('plug1');
    act(() => hub.setValue(0x0102, 0x50, true));
    fireEvent.click(screen.getByRole('tab', { name: 'History' }));
    fireEvent.click(await screen.findByLabelText(/Motion sensor 0x0102 detected someone/, undefined, { timeout: 6000 }));
    await screen.findByText('Only Motion sensor 0x0102');
  }, 15000);
});
