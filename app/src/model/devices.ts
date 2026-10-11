// Presenting entities as devices: titles, icons, which attribute is the main switch.
import type { Attr, Entity } from '../protocol/messages';
import type { HomeState } from '../protocol/store';

const DEVICE_LABELS: Record<string, string> = {
  light: 'Light',
  switch: 'Plug',
  sensor: 'Sensor',
  occupancy: 'Motion sensor',
  contact: 'Contact sensor',
  cover: 'Cover',
  lock: 'Lock',
  climate: 'Thermostat',
  button: 'Button',
};
const DEVICE_ICONS: Record<string, string> = {
  light: '💡',
  switch: '🔌',
  sensor: '🌡️',
  occupancy: '🚶',
  contact: '🚪',
  cover: '🪟',
  lock: '🔒',
  climate: '🌡️',
  button: '🔘',
};

export function hexId(id: number): string {
  return `0x${id.toString(16).padStart(4, '0')}`;
}
export function deviceLabel(device: string): string {
  return DEVICE_LABELS[device] ?? device;
}
export function deviceIcon(device: string): string {
  return DEVICE_ICONS[device] ?? '📟';
}
export function deviceTitle(e: Entity): string {
  return e.name ?? `${deviceLabel(e.device)} ${hexId(e.id)}`;
}

/** The writable on/off attribute, if the device has one: it becomes the card's switch. */
export function primarySwitch(e: Entity): Attr | undefined {
  return e.attrs.find((a) => a.name === 'on_off' && a.flags.includes('w') && a.vt === 'bool');
}

export interface Stepper {
  step: number;
  min: number;
  max: number;
}
/** Writable numeric attributes we know how to nudge with +/-. Brightness: 10 % steps. */
export function stepperFor(a: Attr): Stepper | null {
  if (a.name === 'brightness' && a.flags.includes('w') && a.vt === 'u8') {
    return { step: 25, min: 1, max: 254 };
  }
  return null;
}
export function stepped(a: Attr, dir: 1 | -1): number | null {
  const s = stepperFor(a);
  if (!s || typeof a.value !== 'number') return null;
  return Math.max(s.min, Math.min(s.max, a.value + dir * s.step));
}

/** Named devices first (alphabetical), then unnamed ones by id. */
export function sortedDevices(entities: Entity[]): Entity[] {
  return [...entities].sort((a, b) => {
    if ((a.name === null) !== (b.name === null)) return a.name === null ? 1 : -1;
    if (a.name !== null && b.name !== null) return a.name.localeCompare(b.name);
    return a.id - b.id;
  });
}

/** A device is reachable while the chip that owns it is. */
export function isReachable(s: HomeState, e: Entity): boolean {
  const chip = s.chips.find((c) => c.id === e.chip);
  return chip === undefined || chip.status === 'ready';
}

/** Devices that live on the Zigbee chip are the ones the hub can identify and remove. */
export function canManageDevice(s: HomeState, e: Entity): boolean {
  return s.chips.some((c) => c.id === e.chip && c.label === 'zigbee');
}

/** How long the Zigbee network stays open when the app asks to add a device. */
export const PAIR_SECONDS = 120;
