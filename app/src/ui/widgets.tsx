// The body of a device card: each type shows the one thing that matters for it.
//
// A plug shows its load, a lamp its level, a motion sensor whether there is
// motion, a contact whether the door is open, a thermostat the target. The
// card around them is the same for all: the shell lives in DeviceWidget.
import { Text, View } from './kit';
import { attrOf, deviceTitle, primarySwitch, readingAttrs } from '../model/devices';
import { hsvToCss, xyToHsv } from '../model/color';
import { formatValue } from '../model/format';
import { formatTemperature, type TemperatureUnit } from '../model/temperature';
import type { Attr, Entity } from '../protocol/messages';
import { Glyph } from './Glyph';
import { useTheme } from './theme';

/** The kinds of card the app draws. */
export type WidgetKind =
  | 'plug'
  | 'lamp'
  | 'colorLamp'
  | 'climate'
  | 'motion'
  | 'contact'
  | 'thermostat'
  | 'button'
  | 'presence'
  | 'cover'
  | 'lock'
  | 'generic';

/** Which card a device gets. */
export function widgetFor(entity: Entity): WidgetKind {
  switch (entity.device) {
    case 'switch':
      return 'plug';
    case 'light':
      return colorAttrOf(entity) ? 'colorLamp' : 'lamp';
    case 'sensor':
      return 'climate';
    case 'occupancy':
      return 'motion';
    case 'contact':
      return 'contact';
    case 'climate':
      return 'thermostat';
    case 'button':
      return 'button';
    case 'phone':
      return 'presence';
    case 'cover':
      return 'cover';
    case 'lock':
      return 'lock';
    default:
      return 'generic';
  }
}

function colorAttrOf(entity: Entity): Attr | undefined {
  return entity.attrs.find((a) => a.name === 'color_xy');
}

/** The load a plug draws, in watts — null when it does not say. */
export function powerOf(entity: Entity): number | null {
  const a = attrOf(entity, 'power_w');
  return a && typeof a.value === 'number' ? a.value : null;
}

/** The battery, in percent — null when there is none. */
export function batteryOf(entity: Entity): number | null {
  const a = attrOf(entity, 'battery');
  return a && typeof a.value === 'number' ? a.value : null;
}

/** The colour a lamp is set to, as a CSS colour — null when it has no colour.
 *  The wire speaks CIE x/y, so it goes through HSV on the way to a colour. */
export function colorOf(entity: Entity): string | null {
  const a = colorAttrOf(entity);
  if (!a || !Array.isArray(a.value)) return null;
  const [x, y] = a.value;
  if (typeof x !== 'number' || typeof y !== 'number') return null;
  return hsvToCss(xyToHsv(x, y));
}

/** The line under the name: what the device is doing right now. */
export function subtitleOf(entity: Entity): string | null {
  const sw = primarySwitch(entity);
  const on = sw?.value === true;
  const watt = attrOf(entity, 'power_w');
  if (watt && typeof watt.value === 'number' && on) return `${watt.value} W now`;
  const bright = attrOf(entity, 'brightness');
  if (bright && typeof bright.value === 'number' && on) return `${Math.round((bright.value / 254) * 100)} %`;
  const readings = readingAttrs(entity);
  const head = readings.first ?? readings.rest[0];
  if (head) return formatValue(head);
  if (sw !== undefined) return on ? 'On' : 'Off';
  return null;
}

/** The big reading a sensor shows: the number, not the label. */
export function headlineOf(entity: Entity): string | null {
  const temp = attrOf(entity, 'temperature');
  if (temp !== undefined) return formatTemperature(Number(temp.value) / 10);
  const occupied = attrOf(entity, 'occupied');
  if (occupied !== undefined) return occupied.value ? 'Someone' : 'Nobody';
  const contact = attrOf(entity, 'contact');
  if (contact !== undefined) return contact.value ? 'Closed' : 'Open';
  const home = attrOf(entity, 'home');
  if (home !== undefined) return home.value ? 'Home' : 'Away';
  const readings = readingAttrs(entity);
  const head = readings.first ?? readings.rest[0];
  return head ? formatValue(head) : null;
}

/** The climate reading the mockup shows: «23.4° · 58 %» on one line.
 *  Null when the device is not a temperature/humidity sensor. */
export function climateReading(entity: Entity, unit: TemperatureUnit = 'C'): string | null {
  const temp = attrOf(entity, 'temperature');
  if (temp === undefined) return null;
  const humid = attrOf(entity, 'humidity');
  const t = formatTemperature(Number(temp.value) / 10, unit);
  if (humid === undefined) return t;
  return `${t} · ${Math.round(Number(humid.value) / 10)} %`;
}

/** The small glyph + value line under a sensor's reading. */
export function SensorExtras({ entity }: { entity: Entity }) {
  const t = useTheme();
  const humid = attrOf(entity, 'humidity');
  const batt = batteryOf(entity);
  if (humid === undefined && batt === null) return null;
  return (
    <View style={extrasStyles.row}>
      {humid !== undefined && (
        <View style={extrasStyles.item}>
          <Glyph name="humidity" size={13} color={t.muted} />
          <Text style={[extrasStyles.text, { color: t.muted }]}>{formatValue(humid)}</Text>
        </View>
      )}
      {batt !== null && (
        <View style={extrasStyles.item}>
          <Glyph name="battery" size={13} color={t.muted} />
          <Text style={[extrasStyles.text, { color: t.muted }]}>{batt} %</Text>
        </View>
      )}
    </View>
  );
}

/** The small row under the name: humidity, battery. */
export function Extras({ entity }: { entity: Entity }) {
  const t = useTheme();
  const humid = attrOf(entity, 'humidity');
  const batt = batteryOf(entity);
  if (humid === undefined && batt === null) return null;
  return (
    <View style={extrasStyles.row}>
      {humid !== undefined && (
        <View style={extrasStyles.item}>
          <Glyph name="humidity" size={13} color={t.muted} />
          <Text style={[extrasStyles.text, { color: t.muted }]}>{formatValue(humid)}</Text>
        </View>
      )}
      {batt !== null && (
        <View style={extrasStyles.item}>
          <Glyph name="battery" size={13} color={t.muted} />
          <Text style={[extrasStyles.text, { color: t.muted }]}>{batt} %</Text>
        </View>
      )}
    </View>
  );
}

const extrasStyles = {
  row: { flexDirection: 'row' as const, gap: 10, marginTop: 4 },
  item: { flexDirection: 'row' as const, alignItems: 'center' as const, gap: 3 },
  text: { fontSize: 11.5, fontWeight: '500' as const },
};

/** The name a card shows: the device's own name, without the room word. */
export function cardTitle(entity: Entity): string {
  return entity.name ?? deviceTitle(entity);
}
