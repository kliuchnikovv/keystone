// A device as a button: the icon, the name, and what it is doing right now.
// The button *is* the switch — tapping it turns the device on or off — so there is
// no separate toggle to fumble with. A long press opens the sheet with everything
// else (brightness, colour, readings, rename, blink, remove).
//
// While the finger is down the button says so: it dims at once, and if the finger
// stays, it lights up — that is the app counting the long press.
import { useEffect, useRef, useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { formatValue } from '../model/format';
import { attrOf, deviceIcon, deviceTitle, primarySwitch, readingAttrs, widgetValue, type DeviceStatus } from '../model/devices';
import { stripRoomPrefix } from '../model/rooms';
import type { DeviceShape } from '../model/grid';

import type { Entity } from '../protocol/messages';
import { Glyph } from './Glyph';
import { useTheme } from './theme';

export interface DeviceWidgetProps {
  entity: Entity;
  /** The button's shape in the grid: a square, a wide one or a tall one. */
  shape: DeviceShape;
  status: DeviceStatus;
  reachable: boolean;
  /** Turn the device on or off. */
  onToggle?(): void;
  /** Open the sheet with the device's details. */
  onOpen(): void;
  /** "unreachable" when the device cannot be reached. */
  onUnreachable?(): void;
  /** "updated 2 min ago" — when the value last changed (a tall tile has room for it). */
  updatedText?: string | null;
}

/** How long the finger has to rest before the button says "I am counting". */
const HELD_AFTER_MS = 250;

type PressPhase = 'idle' | 'down' | 'held';

/** The button's own state while it is being touched. */
function usePressPhase() {
  const [phase, setPhase] = useState<PressPhase>('idle');
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const stop = () => {
    if (timer.current) {
      clearTimeout(timer.current);
      timer.current = null;
    }
  };
  useEffect(() => stop, []);
  return {
    phase,
    onPressIn: () => {
      stop();
      setPhase('down');
      timer.current = setTimeout(() => setPhase('held'), HELD_AFTER_MS);
    },
    onPressOut: () => {
      stop();
      setPhase('idle');
    },
  };
}

export function DeviceWidget({ entity, shape, status, reachable, onToggle, onOpen, onUnreachable, updatedText }: DeviceWidgetProps) {
  const t = useTheme();
  const title = deviceTitle(entity);
  const displayName = stripRoomPrefix(entity.name) || title;
  const sw = primarySwitch(entity);
  const powered = sw?.value === true;
  const value = widgetValue(entity, powered);
  const on = sw !== undefined && powered;
  // What a screen reader hears: the device's own state, and how it is doing.
  const state = sw !== undefined ? (on ? 'on' : 'off') : status;
  const spoken = status === 'online' ? state : `${state}, ${status}`;
  const readings = readingAttrs(entity);
  const press = usePressPhase();
  // A held button is counting the long press: it lights up to say so.
  const held = press.phase === 'held';
  const down = press.phase === 'down';

  const toggle = () => {
    if (!reachable) {
      onUnreachable?.();
      return;
    }
    if (sw && onToggle) onToggle();
    else onOpen();
  };

  const shell = (background: string, border: string) => [
    styles.widget,
    {
      backgroundColor: held ? t.accentSoft : background,
      borderColor: held ? t.accent : border,
      borderWidth: held ? 2 : 1,
      borderRadius: t.cardRadius,
      opacity: reachable ? (down ? 0.88 : 1) : 0.55,
    },
  ];

  const icon = (kind: 'small' | 'mid' | 'big') => (
    <View
      accessible
      accessibilityLabel={`${title} is ${spoken}`}
      style={[
        styles.icon,
        kind === 'mid' ? styles.iconMid : kind === 'big' ? styles.iconBig : null,
        {
          backgroundColor: held ? t.accent : on ? t.accentSoft : t.card,
          borderColor: held ? t.accent : t.border,
        },
      ]}
    >
      <Glyph
        name={deviceIcon(entity.device)}
        size={kind === 'small' ? 17 : kind === 'mid' ? 19 : 22}
        color={held ? t.onAccent : on ? t.accent : t.muted}
        filled={on}
      />
    </View>
  );

  const name = (
    <Text style={[styles.name, { color: on || held ? t.accent : t.muted }]} numberOfLines={2}>
      {title}
    </Text>
  );

  const note = status === 'offline' ? 'offline' : status === 'unreachable' ? 'unreachable' : null;

  // A sensor (no switch) is a compact tile: temperature as big numbers on top,
  // humidity and battery below with their own icons. The device name and the
  // "updated …" line live only in the detail sheet.
  if (sw === undefined) {
    const temp = attrOf(entity, 'temperature');
    const humid = attrOf(entity, 'humidity');
    const batt = attrOf(entity, 'battery');
    const occupied = attrOf(entity, 'occupied');
    const contact = attrOf(entity, 'contact');
    const headline =
      temp !== undefined ? formatValue(temp)
      : occupied !== undefined ? formatValue(occupied)
      : contact !== undefined ? formatValue(contact)
      : value;
    return (
      <View style={[...shell(on ? t.accentSoft : t.fill, on ? t.accent : t.border), styles.sensorTile]}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Device ${title}`}
          accessibilityHint="Long press for details"
          accessibilityState={{ selected: on, disabled: !reachable }}
          onPress={toggle}
          onLongPress={onOpen}
          onPressIn={press.onPressIn}
          onPressOut={press.onPressOut}
          style={[styles.sensorInner, down && styles.pressed, held && styles.pressedHeld]}
        >
          <View accessible accessibilityLabel={`${title} is ${spoken}`} style={styles.sensorTop}>
            {headline !== null && headline !== undefined && (
              <Text style={[styles.sensorBig, { color: t.text }]} numberOfLines={1}>
                {headline}
              </Text>
            )}
          </View>
          {(humid !== undefined || batt !== undefined) && (
            <View style={styles.sensorRow}>
              {humid !== undefined && (
                <View style={styles.sensorSub}>
                  <Glyph name="humidity" size={13} color={t.muted} />
                  <Text style={[styles.sensorSubText, { color: t.muted }]} numberOfLines={1}>
                    {formatValue(humid)}
                  </Text>
                </View>
              )}
              {batt !== undefined && (
                <View style={styles.sensorSub}>
                  <Glyph name="battery" size={13} color={t.muted} />
                  <Text style={[styles.sensorSubText, { color: t.muted }]} numberOfLines={1}>
                    {formatValue(batt)}
                  </Text>
                </View>
              )}
            </View>
          )}
          {note !== null && <Text style={[styles.note, { color: status === 'unreachable' ? t.bad : t.muted }]}>{note}</Text>}
        </Pressable>
      </View>
    );
  }
  if (shape === 'square') {
    // A compact tile (e.g. a plug shown at sensor width): icon, name and value
    // stacked into a short row, no "updated …" line.
    return (
      <View style={[...shell(on ? t.accentSoft : t.fill, on ? t.accent : t.border), styles.compactTile]}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Device ${title}`}
          accessibilityHint="Long press for details"
          accessibilityState={{ selected: on, disabled: !reachable }}
          onPress={toggle}
          onLongPress={onOpen}
          onPressIn={press.onPressIn}
          onPressOut={press.onPressOut}
          style={[styles.compactInner, down && styles.pressed, held && styles.pressedHeld]}
        >
          <View
            accessible
            accessibilityLabel={`${title} is ${spoken}`}
            style={[
              styles.compactIcon,
              {
                backgroundColor: held ? t.accent : 'transparent',
                borderColor: held ? t.accent : 'transparent',
                borderWidth: 1,
                borderRadius: 8,
                padding: 4,
              },
            ]}
          >
            <Glyph name={deviceIcon(entity.device)} size={18} color={held ? t.onAccent : on ? t.accent : t.muted} />
          </View>
          <Text style={[styles.compactName, { color: on || held ? t.accent : t.muted }]} numberOfLines={1}>
            {displayName}
          </Text>
          {value !== null && (
            <Text style={[styles.compactValue, { color: on || held ? t.accent : t.muted }]} numberOfLines={1}>
              {value}
            </Text>
          )}
          {/* battery is shown in the readings when present */}
          {note !== null && <Text style={[styles.note, { color: status === 'unreachable' ? t.bad : t.muted }]}>{note}</Text>}
        </Pressable>
      </View>
    );
  }

  if (shape === 'wide') {
    // The room mock's button: icon on the left, name and reading on the right.
    return (
      <View style={shell(on ? t.accentSoft : t.fill, on ? t.accent : t.border)}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Device ${title}`}
          accessibilityHint="Long press for details"
          accessibilityState={{ selected: on, disabled: !reachable }}
          onPress={toggle}
          onLongPress={onOpen}
          onPressIn={press.onPressIn}
          onPressOut={press.onPressOut}
          style={[styles.wide, down && styles.pressed, held && styles.pressedHeld]}
        >
          {icon('small')}
          <View style={styles.text}>
            {name}
            {value !== null && (
              <Text style={[styles.value, { color: on || held ? t.accent : t.muted }]} numberOfLines={1}>
                {value}
              </Text>
            )}
            {/* battery is shown in the readings when present */}
            {note !== null && <Text style={[styles.note, { color: status === 'unreachable' ? t.bad : t.muted }]}>{note}</Text>}
          </View>
        </Pressable>
      </View>
    );
  }

  // Tall: a device with more than one reading — the room for all of them.
  return (
    <View style={[...shell(on ? t.accentSoft : t.fill, on ? t.accent : t.border), styles.tall]}>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel={`Device ${title}`}
        accessibilityHint="Long press for details"
        accessibilityState={{ selected: on, disabled: !reachable }}
        onPress={toggle}
        onLongPress={onOpen}
        onPressIn={press.onPressIn}
        onPressOut={press.onPressOut}
        style={[styles.tallInner, down && styles.pressed, held && styles.pressedHeld]}
      >
        <View style={styles.tallTop}>
          {icon('big')}
          {name}
        </View>
        <View style={styles.tallReadings}>
          {readings.first !== undefined && (
            <Text style={[styles.bigReading, { color: t.text }]} numberOfLines={1}>
              {formatValue(readings.first)}
            </Text>
          )}
          {readings.rest.slice(0, 2).map((a) => (
            <Text key={a.id} style={[styles.reading, { color: t.muted }]} numberOfLines={1}>
              {formatValue(a)}
            </Text>
          ))}
          {value !== null && readings.first === undefined && (
            <Text style={[styles.value, { color: on || held ? t.accent : t.muted }]} numberOfLines={1}>
              {value}
            </Text>
          )}
          {note !== null && <Text style={[styles.note, { color: status === 'unreachable' ? t.bad : t.muted }]}>{note}</Text>}
        </View>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  widget: {
    flex: 1,
    borderWidth: 1,
    padding: 12,
    overflow: 'hidden',
  },
  pressed: { transform: [{ scale: 0.97 }] },
  pressedHeld: { transform: [{ scale: 0.95 }] },
  wide: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  square: { alignItems: 'center', justifyContent: 'center', paddingVertical: 8 },
  squareInner: { alignItems: 'center', justifyContent: 'center', gap: 5, width: '100%' },
  tall: { justifyContent: 'flex-start', padding: 10 },
  tallInner: { alignItems: 'flex-start', justifyContent: 'space-between', width: '100%', flex: 1 },
  tallTop: { alignItems: 'flex-start', gap: 6, width: '100%' },
  tallReadings: { gap: 3, width: '100%' },
  icon: {
    width: 34,
    height: 34,
    borderRadius: 10,
    borderWidth: StyleSheet.hairlineWidth,
    alignItems: 'center',
    justifyContent: 'center',
  },
  iconMid: { width: 38, height: 38, borderRadius: 12 },
  readingIcon: { alignItems: 'center', justifyContent: 'center' },
  sensorTile: { alignItems: 'center', justifyContent: 'center', padding: 8 },
  sensorInner: { alignItems: 'center', justifyContent: 'center', gap: 2, width: '100%' },
  sensorTop: { alignItems: 'center', justifyContent: 'center' },
  sensorBig: { fontSize: 16, fontWeight: '700', letterSpacing: -0.5 },
  sensorRow: { flexDirection: 'row', gap: 8, marginTop: 2, justifyContent: 'center' },
  sensorSub: { flexDirection: 'row', alignItems: 'center', gap: 2 },
  sensorSubText: { fontSize: 10, fontWeight: '500' },
  compactTile: { alignItems: 'center', justifyContent: 'center', padding: 8 },
  compactInner: { flexDirection: 'row', alignItems: 'center', gap: 6, width: '100%' },
  compactIcon: { alignItems: 'center', justifyContent: 'center' },
  compactName: { fontSize: 12, fontWeight: '700', flex: 1 },
  compactValue: { fontSize: 11, fontWeight: '500' },
  iconBig: { width: 40, height: 40, borderRadius: 12 },
  text: { flex: 1, minWidth: 0, gap: 2 },
  name: { fontSize: 13, fontWeight: '700' },
  value: { fontSize: 11, fontWeight: '500' },
  bigReading: { fontSize: 18, fontWeight: '700', letterSpacing: -0.3, marginTop: 1 },
  reading: { fontSize: 11, fontWeight: '500' },
  note: { fontSize: 10.5, fontWeight: '500' },
  updated: { fontSize: 10.5, fontWeight: '500', marginTop: 1 },
});
