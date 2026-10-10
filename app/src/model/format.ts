// Turning protocol values into text for people. Pure functions: no React, easy to test.
import type { AckStatus, Attr } from '../protocol/messages';
import { HubError } from '../protocol/client';

const LABELS: Record<string, string> = {
  on_off: 'Power',
  power_w: 'Power draw',
  energy_wh: 'Energy',
  brightness: 'Brightness',
  occupied: 'Presence',
  temperature: 'Temperature',
  humidity: 'Humidity',
  illuminance: 'Light level',
  battery: 'Battery',
  contact: 'Contact',
  locked: 'Lock',
  position: 'Position',
};

export function attrLabel(name: string): string {
  const known = LABELS[name];
  if (known) return known;
  const words = name.replace(/_/g, ' ');
  return words.charAt(0).toUpperCase() + words.slice(1);
}

export function formatEnergy(wh: number): string {
  return wh >= 1000 ? `${(wh / 1000).toFixed(2)} kWh` : `${wh} Wh`;
}

export function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m`;
  return `${seconds}s`;
}

/** Brightness is 0..254 on the wire (Zigbee level); people think in percent. */
export function brightnessPercent(v: number): number {
  return Math.round((v / 254) * 100);
}

export function formatValue(a: Pick<Attr, 'name' | 'value'>): string {
  const v = a.value;
  if (v === null) return '—';
  if (Array.isArray(v)) return `(${v[0]}, ${v[1]})`;
  switch (a.name) {
    case 'on_off':
      return v ? 'On' : 'Off';
    case 'power_w':
      return `${v} W`;
    case 'energy_wh':
      return formatEnergy(Number(v));
    case 'brightness':
      return `${brightnessPercent(Number(v))} %`;
    case 'occupied':
      return v ? 'Occupied' : 'Clear';
    case 'temperature':
      return `${(Number(v) / 10).toFixed(1)} °C`; // SPEC §6.2: int16 in 0.1 °C
  }
  if (typeof v === 'boolean') return v ? 'Yes' : 'No';
  return String(v);
}

/** A message for a command that completed but not successfully; null when it is fine. */
export function describeAck(status: AckStatus): string | null {
  switch (status) {
    case 'ok':
      return null;
    case 'unreachable':
      return 'The device is unreachable';
    case 'fail':
      return 'The device refused the command';
    case 'unsupported':
      return 'The device does not support this';
    case 'timeout':
      return 'The device did not answer';
  }
}

/** A message for a command/request that failed outright. */
export function describeError(e: unknown): string {
  if (e instanceof HubError) {
    switch (e.code) {
      case 'disconnected':
        return 'Not connected to the hub';
      case 'closed':
        return 'Connection closed';
      case 'timeout':
        return 'The hub did not answer in time';
      case 'read_only':
        return "This value can't be changed";
      case 'bad_args':
        return e.message && e.message !== 'bad_args' ? `Invalid value: ${e.message}` : 'Invalid value';
      case 'not_found':
        return 'That device no longer exists';
      case 'table_full':
        return 'The hub has no room for more';
      case 'unsupported':
        return 'The hub does not support this yet';
      default:
        return `The hub reported: ${e.code}`;
    }
  }
  return e instanceof Error ? e.message : 'Something went wrong';
}
