// @vitest-environment jsdom
// Settings: the hub's summary and the short list of rows.
import './setup-dom';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { SettingsScreen } from '../src/ui/SettingsScreen';
import type { HomeState } from '../src/protocol/store';

afterEach(cleanup);

const home = {
  status: 'ready',
  hub: { name: 'mock-home', fw: '1.6', time: 0, tz: 'UTC', synced: true, wifi: { ssid: 'home-5G', rssi: -58, ip: '192.168.0.202' } },
  chips: [
    { id: 1, label: 'zigbee', status: 'ready' },
    { id: 2, label: 'thread', status: 'ready' },
  ],
  entities: [],
} as unknown as HomeState;

function renderSettings(over: Partial<Parameters<typeof SettingsScreen>[0]> = {}) {
  const props = {
    home,
    mode: 'light' as const,
    onOpenHub: vi.fn(),
    onOpenHistory: vi.fn(),
    onOpenWifi: vi.fn(),
    onOpenRemoteAccess: vi.fn(),
    onOpenFirmware: vi.fn(),
    onOpenLocation: vi.fn(),
    onOpenDisplay: vi.fn(),
    onOpenPlugins: vi.fn(),
    ...over,
  };
  render(<SettingsScreen {...props} />);
  return props;
}

describe('the settings screen', () => {
  it('summarises the hub on top', () => {
    renderSettings();
    expect(screen.getByText('mock-home')).toBeTruthy();
    expect(screen.getByText(/Connected · firmware 1.6/)).toBeTruthy();
  });

  it('lists the hub, history, wifi and remote access', () => {
    renderSettings();
    expect(screen.getByLabelText('Hub & health')).toBeTruthy();
    expect(screen.getByLabelText('History')).toBeTruthy();
    expect(screen.getByLabelText('Wi-Fi')).toBeTruthy();
    expect(screen.getByLabelText('Remote access')).toBeTruthy();
    expect(screen.getByLabelText('Display')).toBeTruthy();
    expect(screen.getByLabelText('Plugins')).toBeTruthy();
  });

  it('shows the Wi-Fi network and the theme the app wears', () => {
    renderSettings();
    expect(screen.getByText('home-5G')).toBeTruthy();
    expect(screen.getByText('Light · °C')).toBeTruthy();
  });

  it('the rows open their screens', () => {
    const props = renderSettings();
    fireEvent.click(screen.getByLabelText('Hub & health'));
    expect(props.onOpenHub).toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('History'));
    expect(props.onOpenHistory).toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('Display'));
    expect(props.onOpenDisplay).toHaveBeenCalled();
    fireEvent.click(screen.getByLabelText('Plugins'));
    expect(props.onOpenPlugins).toHaveBeenCalled();
  });

  it('every hub row opens its own screen, not the same one', () => {
    const props = renderSettings();
    const doors: [string, keyof typeof props][] = [
      ['Hub & health', 'onOpenHub'],
      ['History', 'onOpenHistory'],
      ['Wi-Fi', 'onOpenWifi'],
      ['Remote access', 'onOpenRemoteAccess'],
      ['Firmware updates', 'onOpenFirmware'],
      ['Location', 'onOpenLocation'],
    ];
    for (const [label, door] of doors) {
      fireEvent.click(screen.getByLabelText(label));
      expect(props[door]).toHaveBeenCalled();
      // Nothing else opened: each door is its own.
      const others = doors.filter(([, d]) => d !== door);
      for (const [, d] of others) expect(props[d]).not.toHaveBeenCalled();
      // Reset for the next door.
      for (const [, d] of doors) (props[d] as ReturnType<typeof vi.fn>).mockClear();
    }
  });

  it('the theme picker is not here any more', () => {
    renderSettings();
    expect(screen.queryByLabelText('Appearance')).toBeNull();
    expect(screen.queryByLabelText('Theme System')).toBeNull();
  });
});
