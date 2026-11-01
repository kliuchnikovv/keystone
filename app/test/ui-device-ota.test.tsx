// @vitest-environment jsdom
import './setup-dom';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { longPress } from './helpers';
import type { Commands } from '../src/hooks/useCommands';
import type { Attr, DeviceOtaSession, Entity } from '../src/protocol/messages';
import { initialState, type HomeState } from '../src/protocol/store';
import { DeviceSheet } from '../src/ui/DeviceSheet';
import { DevicesScreen } from '../src/ui/DevicesScreen';

afterEach(cleanup);

const SHA = 'b'.repeat(64);
const URL = 'https://example.com/lamp.ota';

const attr = (id: number, name: string, vt: Attr['vt'], value: Attr['value'], flags: Attr['flags'] = ['r', 'rep']): Attr => ({
  id,
  name,
  vt,
  value,
  flags,
});
const entity = (id: number, name: string, chip: number): Entity => ({
  id,
  name,
  chip,
  device: 'light',
  attrs: [attr(1, 'on_off', 'bool', false, ['r', 'w', 'rep'])],
});

const noCommands: Commands = {
  send: async () => {},
  isPending: () => false,
  request: async () => ({}),
  busy: false,
  message: null,
  tone: 'good',
  notify: () => {},
  dismiss: () => {},
};

/** The sheet with the device's firmware panel behind the "Update firmware" row. */
function sheet(e: Entity, over: { onStartUpdate?: (url: string, sha256: string) => Promise<boolean>; ota?: DeviceOtaSession } = {}) {
  render(
    <DeviceSheet
      entity={e}
      status="online"
      reachable
      visible
      isPending={() => false}
      onSet={() => {}}
      onClose={() => {}}
      onRename={async () => true}
      {...(over.onStartUpdate ? { onStartUpdate: over.onStartUpdate } : {})}
      {...(over.ota ? { ota: over.ota } : {})}
    />,
  );
}

describe('updating the firmware of a device', () => {
  it('the dialog checks the input and only then asks the hub', async () => {
    const onStartUpdate = vi.fn(async () => true);
    sheet(entity(0x0100, 'lamp', 1), { onStartUpdate });
    fireEvent.click(screen.getByLabelText('Update firmware lamp'));

    // A URL that is not http(s) is refused before anything is sent.
    fireEvent.change(screen.getByLabelText('Firmware file URL'), { target: { value: 'example.com/lamp.ota' } });
    fireEvent.change(screen.getByLabelText('SHA-256'), { target: { value: SHA } });
    fireEvent.click(screen.getByLabelText('Start'));
    expect(screen.getByText(/must start with http/)).toBeTruthy();
    expect(onStartUpdate).not.toHaveBeenCalled();

    // A short checksum is refused too.
    fireEvent.change(screen.getByLabelText('Firmware file URL'), { target: { value: URL } });
    fireEvent.change(screen.getByLabelText('SHA-256'), { target: { value: 'abc' } });
    fireEvent.click(screen.getByLabelText('Start'));
    expect(screen.getByText('The SHA-256 must be 64 hex digits')).toBeTruthy();
    expect(onStartUpdate).not.toHaveBeenCalled();

    // With both fields right the hub is asked.
    fireEvent.change(screen.getByLabelText('SHA-256'), { target: { value: SHA } });
    fireEvent.click(screen.getByLabelText('Start'));
    await waitFor(() => expect(onStartUpdate).toHaveBeenCalledWith(URL, SHA));
  });

  it('the dialog can be closed without starting anything', () => {
    const onStartUpdate = vi.fn(async () => true);
    sheet(entity(0x0100, 'lamp', 1), { onStartUpdate });
    fireEvent.click(screen.getByLabelText('Update firmware lamp'));
    expect(screen.getByLabelText('Firmware file URL')).toBeTruthy();
    fireEvent.click(screen.getByLabelText('Cancel firmware update'));
    expect(screen.queryByLabelText('Firmware file URL')).toBeNull();
    expect(onStartUpdate).not.toHaveBeenCalled();
  });

  it('the progress of a running update is in the sheet', () => {
    sheet(entity(0x0100, 'lamp', 1), { ota: { phase: 'transferring', status: 'ok', percent: 42 } });
    expect(screen.getByText('Installing on the device')).toBeTruthy();
    expect(screen.getByText('42 %')).toBeTruthy();
  });

  it('a failed update says what the hub said', () => {
    sheet(entity(0x0100, 'lamp', 1), { ota: { phase: 'failed', status: 'bad_checksum', percent: 0 } });
    expect(screen.getByText('Failed: The file does not match the SHA-256')).toBeTruthy();
  });

  it('a device that cannot be updated has no row for it', () => {
    sheet(entity(0x0100, 'lamp', 1));
    expect(screen.queryByLabelText('Update firmware lamp')).toBeNull();
  });
});

describe('which devices can be updated', () => {
  const home: HomeState = {
    ...initialState,
    status: 'ready',
    entities: [entity(0x0100, 'lamp', 1), entity(0x0200, 'matter lamp', 2)],
    chips: [{ id: 1, label: 'zigbee', status: 'ready', boot_id: 1, reboots: 0, telemetry: {} }],
  };

  it('only a device on the Zigbee chip gets the item', async () => {
    render(<DevicesScreen home={home} commands={noCommands} />);
    await longPress(screen.getByLabelText('Device lamp'));
    expect(await screen.findByLabelText('Update firmware lamp')).toBeTruthy();
    cleanup();

    render(<DevicesScreen home={home} commands={noCommands} />);
    await longPress(screen.getByLabelText('Device matter lamp'));
    await screen.findByLabelText('Rename matter lamp');
    expect(screen.queryByLabelText('Update firmware matter lamp')).toBeNull();
  });
});
