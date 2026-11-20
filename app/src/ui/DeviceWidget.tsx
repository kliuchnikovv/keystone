// A device as a card: the icon, the name, and what it is doing right now.
//
// The card *is* the switch — tapping it turns the device on or off — so there
// is no separate toggle to fumble with. A long press opens the sheet with
// everything else (brightness, colour, readings, rename, blink, remove).
//
// While the finger is down the card says so: it dims at once, and if the
// finger stays, it lights up — that is the app counting the long press.
import { useEffect, useRef, useState } from 'react';
import { Platform, Pressable, StyleSheet, Text, View } from './kit';
import { attrOf, deviceIcon, deviceTitle, primarySwitch } from '../model/devices';
import { formatValue } from '../model/format';
import { formatTemperature } from '../model/temperature';
import { stripRoomPrefix } from '../model/rooms';
import type { Entity } from '../protocol/messages';
import { Dimmer } from './Dimmer';
import { Glyph } from './Glyph';
import { useTheme } from './theme';
import { Extras, batteryOf, colorOf, headlineOf, subtitleOf } from './widgets';

export interface DeviceWidgetProps {
  entity: Entity;
  status: 'online' | 'offline' | 'unreachable';
  reachable: boolean;
  /** Turn the device on or off. */
  onToggle?(): void;
  /** Open the sheet with the device's details. */
  onOpen(): void;
  /** "unreachable" when the device cannot be reached. */
  onUnreachable?(): void;
  /** "updated 2 min ago" — when the value last changed. */
  updatedText?: string | null;
  /** The lamp's level changed — the caller sends it to the hub. */
  onBrightness?(percent: number): void;
  /** The temperature unit the app speaks. */
  temperatureUnit?: 'C' | 'F';
}

/** How long the finger has to rest before the card says "I am counting". */
const HELD_AFTER_MS = 250;

type PressPhase = 'idle' | 'down' | 'held';

/** The card's own state while it is being touched. */
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

export function DeviceWidget({
  entity,
  status,
  reachable,
  onToggle,
  onOpen,
  onUnreachable,
  updatedText,
  onBrightness,
  temperatureUnit = 'C',
}: DeviceWidgetProps) {
  const t = useTheme();
  const title = deviceTitle(entity);
  const displayName = stripRoomPrefix(entity.name) || title;
  const sw = primarySwitch(entity);
  const powered = sw?.value === true;
  const on = sw !== undefined && powered;
  // What a screen reader hears: the device's own state, and how it is doing.
  const state = sw !== undefined ? (on ? 'on' : 'off') : status;
  const spoken = status === 'online' ? state : `${state}, ${status}`;
  const press = usePressPhase();
  // A held card is counting the long press: it lights up to say so.
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

  // The card: gold when it is on, quiet when it is not.
  const lit = on || held;
  const card = [
    styles.card,
    {
      backgroundColor: lit ? t.glowFill : t.card,
      borderColor: lit ? t.glowBorder : t.border,
      borderRadius: t.cardRadius,
      opacity: reachable ? (down ? 0.88 : 1) : 0.4,
    },
  ];

  const note = status === 'offline' ? 'offline' : status === 'unreachable' ? 'unreachable' : null;

  // The icon tile: filled gold when the device is on.
  const tile = (
    <View
      accessible
      accessibilityLabel={`${title} is ${spoken}`}
      style={[styles.tile, { backgroundColor: lit ? t.accent : t.fill }]}
    >
      <Glyph name={deviceIcon(entity.device)} size={21} color={lit ? t.onAccent : t.muted} filled={on} />
    </View>
  );

  // A sensor has no switch: it shows its reading big, and the rest small.
  if (sw === undefined) {
    const headline = headlineOf(entity);
    const temp = attrOf(entity, 'temperature');
    const big =
      temp !== undefined ? formatTemperature(Number(temp.value) / 10, temperatureUnit) : headline;
    const humidity = attrOf(entity, 'humidity');
    return (
      <View style={card}>
        <Pressable
          accessibilityRole="button"
          accessibilityLabel={`Device ${title}`}
          accessibilityHint="Long press for details"
          accessibilityState={{ selected: on, disabled: !reachable }}
          onPress={toggle}
          onLongPress={onOpen}
          onPressIn={press.onPressIn}
          onPressOut={press.onPressOut}
          style={[styles.inner, styles.sensor, down && styles.pressed, held && styles.pressedHeld]}
        >
          <View style={styles.sensorTop}>
            {tile}
            {big !== null && (
              <Text style={[styles.bigReading, { color: t.text }]} numberOfLines={1}>
                {big}
              </Text>
            )}
          </View>
          <Text style={[styles.name, { color: t.text }]} numberOfLines={1}>
            {displayName}
          </Text>
          <Extras entity={entity} />
          {note !== null && <Text style={[styles.note, { color: status === 'unreachable' ? t.bad : t.muted }]}>{note}</Text>}
        </Pressable>
      </View>
    );
  }

  // A lamp with a dimmer: the card is its own dimmer — the fill is the level,
  // a tap toggles, a swipe changes the level, a long press opens the sheet.
  const brightnessAttr = attrOf(entity, 'brightness');
  if (brightnessAttr !== undefined && onToggle) {
    const level = Math.round((Number(brightnessAttr.value) / 254) * 100);
    return (
      <Dimmer
        value={level}
        on={on}
        title={displayName}
        color={colorOf(entity)}
        spoken={`${title} is ${spoken}`}
        note={note}
        onChange={(v) => onBrightness?.(v)}
        onToggle={onToggle}
        onOpen={onOpen}
      />
    );
  }

  // A switchable device: the icon, the name, and what it is doing.
  const subtitle = subtitleOf(entity);
  const colour = colorOf(entity);
  const battery = batteryOf(entity);
  const percent = attrOf(entity, 'brightness');
  const brightness =
    percent !== undefined && typeof percent.value === 'number' ? Math.round((percent.value / 254) * 100) : null;

  return (
    <View style={card}>
      <Pressable
        accessibilityRole="button"
        accessibilityLabel={`Device ${title}`}
        accessibilityHint="Long press for details"
        accessibilityState={{ selected: on, disabled: !reachable }}
        onPress={toggle}
        onLongPress={onOpen}
        onPressIn={press.onPressIn}
        onPressOut={press.onPressOut}
        style={[styles.inner, styles.row, down && styles.pressed, held && styles.pressedHeld]}
      >
        {tile}
        <View style={styles.text}>
          <Text style={[styles.name, { color: t.text }]} numberOfLines={1}>
            {displayName}
          </Text>
          {subtitle !== null && (
            <Text style={[styles.sub, { color: on ? t.accentInk : t.muted }]} numberOfLines={1}>
              {subtitle}
            </Text>
          )}
          {battery !== null && (
            <View style={styles.battery}>
              <Glyph name="battery" size={12} color={t.muted} />
              <Text style={[styles.batteryText, { color: t.muted }]}>{battery} %</Text>
            </View>
          )}
          {note !== null && <Text style={[styles.note, { color: status === 'unreachable' ? t.bad : t.muted }]}>{note}</Text>}
        </View>
        {brightness !== null && on && (
          <Text style={[styles.grip, { color: t.accentInk }]}>{brightness}%</Text>
        )}
        {colour !== null && (
          <View style={[styles.colorDot, { backgroundColor: colour, borderColor: t.card }]} />
        )}
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  card: { borderWidth: 1, padding: 12, overflow: 'hidden' },
  inner: { width: '100%' },
  pressed: { transform: [{ scale: 0.97 }] },
  pressedHeld: { transform: [{ scale: 0.95 }] },
  row: { flexDirection: 'row', alignItems: 'center', gap: 11 },
  sensor: { alignItems: 'flex-start', gap: 6 },
  sensorTop: { flexDirection: 'row', alignItems: 'center', gap: 10, alignSelf: 'stretch' },
  tile: { width: 44, height: 44, borderRadius: 14, alignItems: 'center', justifyContent: 'center' },
  text: { flex: 1, minWidth: 0, gap: 2 },
  name: { fontSize: 14.5, fontWeight: '600' },
  sub: { fontSize: 12.5, fontWeight: '500' },
  bigReading: { fontSize: 20, fontWeight: '700', letterSpacing: -0.4 },
  battery: { flexDirection: 'row', alignItems: 'center', gap: 3, marginTop: 2 },
  batteryText: { fontSize: 11, fontWeight: '500' },
  grip: { fontSize: 15, fontWeight: '700' },
  colorDot: { width: 26, height: 26, borderRadius: 13, borderWidth: 2 },
  note: { fontSize: 10.5, fontWeight: '500' },
});
