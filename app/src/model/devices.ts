// Presenting entities as devices: titles, glyphs, which attribute is the main switch.
import type { GlyphName } from '../ui/Glyph';
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
const DEVICE_GLYPHS: Record<string, GlyphName> = {
  light: 'light',
  switch: 'switch',
  sensor: 'sensor',
  occupancy: 'occupancy',
  contact: 'contact',
  cover: 'cover',
  lock: 'lock',
  climate: 'climate',
  button: 'button',
};

export function hexId(id: number): string {
  return `0x${id.toString(16).padStart(4, '0')}`;
}
export function deviceLabel(device: string): string {
  return DEVICE_LABELS[device] ?? device;
}
export function deviceIcon(device: string): GlyphName {
  return DEVICE_GLYPHS[device] ?? 'chip';
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
  /** Accessibility words: "<title> dimmer", "<title> brighter", and the noun for the value text. */
  down: string;
  up: string;
  noun: string;
}
/** Writable numeric attributes we know how to nudge with +/-: brightness 10 % steps, colour
 * temperature (mireds, so "−" is cooler), target temperature 0.5 °C. */
export function stepperFor(a: Attr): Stepper | null {
  if (!a.flags.includes('w')) return null;
  if (a.name === 'brightness' && a.vt === 'u8') {
    return { step: 25, min: 1, max: 254, down: 'dimmer', up: 'brighter', noun: 'brightness' };
  }
  if (a.name === 'color_temp' && a.vt === 'u16') {
    return { step: 25, min: 153, max: 500, down: 'cooler', up: 'warmer', noun: 'color temperature' };
  }
  if (a.name === 'setpoint' && a.vt === 'i16') {
    return { step: 5, min: 50, max: 350, down: 'lower', up: 'higher', noun: 'target temperature' };
  }
  return null;
}
export function stepped(a: Attr, dir: 1 | -1): number | null {
  const s = stepperFor(a);
  if (!s || typeof a.value !== 'number') return null;
  return Math.max(s.min, Math.min(s.max, a.value + dir * s.step));
}

export interface ModeOption {
  value: number;
  label: string;
}
/** A thermostat's writable mode: the choices the hub models (Heat first, it is the common one). */
export function modeOptions(a: Attr): ModeOption[] | null {
  if (a.name !== 'mode' || !a.flags.includes('w') || a.vt !== 'enum8') return null;
  return [
    { value: 4, label: 'Heat' },
    { value: 1, label: 'Auto' },
    { value: 3, label: 'Cool' },
    { value: 0, label: 'Off' },
  ];
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

export function attrOf(e: Entity, name: string): Attr | undefined {
  return e.attrs.find((a) => a.name === name);
}

/** True while the device's main switch is on — what tints its glyph chip. */
export function isPowered(e: Entity): boolean {
  return primarySwitch(e)?.value === true;
}

/** The two metering attributes of a plug, in the order the card shows them. */
export function meterAttrs(e: Entity): Attr[] {
  return ['power_w', 'energy_wh'].map((n) => attrOf(e, n)).filter((a): a is Attr => a !== undefined);
}

// What a sensor *reports*. A thermostat's `setpoint` is a target, not a reading — it stays
// in the rows so it keeps its stepper.
const READINGS = ['temperature', 'humidity', 'illuminance', 'battery', 'occupied', 'contact'];

/** A sensor's headline reading, and the rest to list under it.
 * A measurement (a number) gets the big type; a plain state ("Occupied", "Open") does not —
 * a word set at 34 pt reads as a mistake. */
export function readingAttrs(e: Entity): { first: Attr | undefined; rest: Attr[] } {
  const found = READINGS.map((n) => attrOf(e, n)).filter((a): a is Attr => a !== undefined);
  const head = found[0];
  if (head !== undefined && typeof head.value !== 'number') return { first: undefined, rest: found };
  return { first: head, rest: found.slice(1) };
}
