// Presenting entities as devices: titles, glyphs, which attribute is the main switch.
import type { GlyphName } from '../ui/Glyph';
import { brightnessPercent, formatValue } from './format';
import type { Attr, DeviceOtaPhase, DeviceOtaStatus, Entity } from '../protocol/messages';
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
/** Writable numeric attributes we know how to nudge with +/-: target temperature 0.5 °C. */
export function stepperFor(a: Attr): Stepper | null {
  if (!a.flags.includes('w')) return null;
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

export interface SliderSpec {
  min: number;
  max: number;
  step: number;
  /** Lower-case name for screen readers: "<title> brightness". */
  noun: string;
  /** The wire value as the slider shows it (percent, kelvin). */
  show(v: number): number;
  /** What the slider shows back as a wire value. */
  wire(v: number): number;
  /** How the value reads next to the slider. */
  text(v: number): string;
}
/** Attributes that deserve a slider rather than a +/- stepper: brightness in percent,
 * colour temperature in kelvin (the wire speaks mireds). */
export function sliderFor(a: Attr): SliderSpec | null {
  if (!a.flags.includes('w')) return null;
  if (a.name === 'brightness' && a.vt === 'u8') {
    return {
      min: 0,
      max: 100,
      step: 10,
      noun: 'brightness',
      show: (v) => brightnessPercent(v),
      // 0 % would switch the lamp off in Zigbee; the lowest level is 1.
      wire: (v) => Math.max(1, Math.round((v / 100) * 254)),
      text: (v) => `${Math.round(v)} %`,
    };
  }
  if (a.name === 'color_temp' && a.vt === 'u16') {
    return {
      min: 2000,
      max: 6500,
      step: 100,
      noun: 'color temperature',
      show: (v) => Math.round(1_000_000 / Math.max(1, v)),
      wire: (v) => Math.max(153, Math.min(500, Math.round(1_000_000 / Math.max(1, v)))),
      text: (v) => `${Math.round(v)} K`,
    };
  }
  return null;
}

/** The writable colour attribute of a light, if it has one. */
export function colorAttr(e: Entity): Attr | undefined {
  return e.attrs.find((a) => a.name === 'color_xy' && a.flags.includes('w') && a.vt === 'xy');
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

/** How the device looks from the outside: online, offline (its chip went quiet) or
 * unreachable (the hub itself is not answering). */
export type DeviceStatus = 'online' | 'offline' | 'unreachable';

export function deviceStatus(s: HomeState, e: Entity): DeviceStatus {
  if (s.status !== 'ready') return 'unreachable';
  const chip = s.chips.find((c) => c.id === e.chip);
  return chip === undefined || chip.status === 'ready' ? 'online' : 'offline';
}

/** Devices that live on the Zigbee chip are the ones the hub can identify and remove. */
export function canManageDevice(s: HomeState, e: Entity): boolean {
  return s.chips.some((c) => c.id === e.chip && c.label === 'zigbee');
}

/** The Zigbee co-processor. Its devices are the ones that can be given new firmware. */
export const ZIGBEE_CHIP_ID = 1;

/** Only a device on the Zigbee chip can be given new firmware. */
export function canUpdateFirmware(e: Entity): boolean {
  return e.chip === ZIGBEE_CHIP_ID;
}

const OTA_PHASE_LABELS: Record<DeviceOtaPhase, string> = {
  idle: 'Waiting',
  receiving: 'Sending the file to the hub',
  verifying: 'Verifying the file',
  notifying: 'Telling the device to update',
  transferring: 'Installing on the device',
  done: 'Done',
  failed: 'Failed',
};

/** What the hub is doing with the firmware, in words. */
export function otaPhaseLabel(phase: DeviceOtaPhase): string {
  return OTA_PHASE_LABELS[phase];
}

const OTA_STATUS_LABELS: Record<DeviceOtaStatus, string> = {
  ok: 'All good',
  busy: 'The hub is busy with another update',
  too_large: 'The file is too big for the device',
  bad_checksum: 'The file does not match the SHA-256',
  bad_format: 'The hub does not know this .ota file',
  unsupported: 'This device cannot be updated this way',
  timeout: 'The update ran out of time',
  mismatch: 'The device did not take the file',
  aborted: 'The update was called off',
};

/** Why a firmware session stopped (or that it did not). */
export function otaStatusLabel(status: DeviceOtaStatus): string {
  return OTA_STATUS_LABELS[status];
}

/** What is wrong with what the user typed for a firmware update, or null when it is fine. */
export function otaInputProblem(url: string, sha256: string): string | null {
  const u = url.trim();
  if (!u) return 'Enter the address of the .ota file';
  if (!/^https?:\/\/\S+$/i.test(u)) return 'The address must start with http:// or https://';
  const s = sha256.trim();
  if (!s) return 'Enter the SHA-256 of the file';
  if (!/^[0-9a-f]{64}$/i.test(s)) return 'The SHA-256 must be 64 hex digits';
  return null;
}

/** How long the Zigbee network stays open when the app asks to add a device. */
export const PAIR_SECONDS = 120;

/** "12 s ago", "2 min ago" — how long ago something happened (client clock). */
export function agoText(ms: number | undefined, now: number): string | null {
  if (ms === undefined) return null;
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 5) return 'just now';
  if (s < 60) return `${s} s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ago`;
  return `${Math.floor(m / 60)} h ago`;
}

export function attrOf(e: Entity, name: string): Attr | undefined {
  return e.attrs.find((a) => a.name === name);
}

/** True while the device's main switch is on — what tints its glyph chip. */
export function isPowered(e: Entity): boolean {
  return primarySwitch(e)?.value === true;
}

/** Is the device known to be off? A device without a switch (a sensor) is not off —
 * its dimmer stays usable. */
export function isKnownOff(e: Entity): boolean {
  const sw = primarySwitch(e);
  return sw !== undefined && sw.value !== true;
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

/** What a device button says under its name: the load it draws, the light level it is
 * set to, or what a sensor reads. A device that is switched off says nothing. */
export function widgetValue(e: Entity, powered: boolean): string | null {
  const sw = primarySwitch(e);
  if (sw !== undefined && !powered) return null;
  const watt = attrOf(e, 'power_w');
  if (watt) return formatValue(watt);
  const bright = attrOf(e, 'brightness');
  if (bright) return formatValue(bright);
  const readings = readingAttrs(e);
  const head = readings.first ?? readings.rest[0];
  if (head) return formatValue(head);
  return sw !== undefined ? 'On' : null;
}

export interface RoomSummary {
  /** How many of the room's switches are on. */
  on: number;
  /** How many switches the room has. */
  total: number;
  /** The room's total load in watts, when the devices report it. */
  watts: number | null;
}

/** "2 of 3 on · 42 W" — the line above the room's devices. */
export function roomSummary(devices: Entity[]): RoomSummary | null {
  const switches = devices.map(primarySwitch).filter((a): a is Attr => a !== undefined);
  if (switches.length === 0) return null;
  const on = switches.filter((a) => a.value === true).length;
  const watts = devices
    .map((e) => attrOf(e, 'power_w'))
    .filter((a): a is Attr => a !== undefined && typeof a.value === 'number')
    .reduce((sum, a) => sum + (a.value as number), 0);
  return { on, total: switches.length, watts: watts > 0 ? Math.round(watts) : null };
}

/** The wording of the summary line. */
export function summaryText(s: RoomSummary): string {
  const head = s.total === 1 ? (s.on === 1 ? 'On' : 'Off') : `${s.on} of ${s.total} on`;
  return s.watts === null ? head : `${head} · ${s.watts} W`;
}

export type ClimateKind = 'warm' | 'cool' | 'good';

export interface ClimateTile {
  label: string;
  value: string;
  kind: ClimateKind;
}

/** The room's climate as tiles: temperature, humidity, light. Only what the devices
 * actually report — a tile with a dash is worse than no tile. */
export function climateTiles(devices: Entity[]): ClimateTile[] {
  const first = (name: string): Attr | undefined => {
    for (const e of devices) {
      const a = attrOf(e, name);
      if (a !== undefined) return a;
    }
    return undefined;
  };
  const tiles: ClimateTile[] = [];
  const temp = first('temperature');
  if (temp) tiles.push({ label: 'Temp.', value: formatValue(temp), kind: 'warm' });
  const humid = first('humidity');
  if (humid) tiles.push({ label: 'Humidity', value: formatValue(humid), kind: 'cool' });
  const lux = first('illuminance');
  if (lux) tiles.push({ label: 'Light', value: formatValue(lux), kind: 'good' });
  return tiles;
}
