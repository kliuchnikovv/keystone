// The lamp card is its own dimmer.
//
// The fill of the card *is* the level: the left part of it glows gold up to
// the brightness, and the gold edge marks where the level ends. A tap turns
// the lamp on or off; a swipe across the card changes the level; a long press
// opens the full sheet.
//
// A drag of more than the threshold cancels the tap, so a swipe never
// switches the lamp by accident.
import { useRef, useState } from 'react';
import { Pressable, StyleSheet, Text, View, type GestureResponderEvent } from './kit';
import { Glyph } from './Glyph';
import { useTheme } from './theme';

/** How far the finger must travel before the gesture becomes a drag. */
export const DRAG_THRESHOLD = 12;

/** The level a drag of `dx` pixels over a card `width` wide means. */
export function levelAfterDrag(startValue: number, dx: number, width: number): number {
  if (width <= 0) return startValue;
  const next = startValue + (dx / width) * 100;
  return Math.max(0, Math.min(100, Math.round(next)));
}

export interface DimmerProps {
  /** The level, 0–100. */
  value: number;
  /** Is the lamp on? */
  on: boolean;
  /** The level changed — the caller sends it to the hub. */
  onChange(value: number): void;
  /** The lamp was switched on or off. */
  onToggle(): void;
  /** The full sheet: colour, scenes, rename. */
  onOpen(): void;
  /** The lamp's colour, as a CSS colour — the dot in the corner. */
  color?: string | null;
  /** The lamp's name. */
  title?: string;
  /** What a screen reader hears: the lamp's own state. */
  spoken?: string;
  /** "offline" — the lamp cannot be reached right now. */
  note?: string | null;
}

export function Dimmer({ value, on, onChange, onToggle, onOpen, color = null, title, spoken, note = null }: DimmerProps) {
  const t = useTheme();
  const [width, setWidth] = useState(0);
  // Where the drag started, and where the level was then.
  const start = useRef<{ x: number; value: number } | null>(null);
  const dragged = useRef(false);

  const level = Math.max(0, Math.min(100, Math.round(value)));

  const grant = (e: GestureResponderEvent) => {
    start.current = { x: e.nativeEvent.locationX, value: level };
    dragged.current = false;
  };

  const move = (e: GestureResponderEvent) => {
    const s = start.current;
    if (!s) return;
    const dx = e.nativeEvent.locationX - s.x;
    if (Math.abs(dx) > DRAG_THRESHOLD) dragged.current = true;
    if (!dragged.current || width <= 0) return;
    // Dragging right brightens, left dims.
    onChange(levelAfterDrag(s.value, dx, width));
  };

  const release = () => {
    start.current = null;
  };

  const tap = () => {
    if (dragged.current) return; // a swipe is not a tap
    onToggle();
  };

  return (
    <Pressable
      testID="dimmer"
      accessibilityRole="button"
      accessibilityLabel={title ? `Device ${title}` : 'Device'}
      accessibilityValue={{ min: 0, max: 100, now: level, text: `${level} %` }}
      onPress={tap}
      onLongPress={onOpen}
      onStartShouldSetResponder={() => true}
      onMoveShouldSetResponder={() => true}
      onResponderGrant={grant}
      onResponderMove={move}
      onResponderRelease={release}
      onResponderTerminate={release}
      onLayout={(e) => setWidth(e.nativeEvent.layout.width)}
      style={[styles.card, { borderColor: on ? t.glowBorder : t.border, borderRadius: t.cardRadius }]}
    >
      {/* The fill: the level, as a warm wash with a gold edge. */}
      <View pointerEvents="none" style={styles.fillLayer}>
        <View
          testID="dimmer-fill"
          style={[
            styles.fill,
            { width: `${level}%`, backgroundColor: on ? 'rgba(207,159,51,0.26)' : 'rgba(207,159,51,0.10)' },
          ]}
        />
        <View
          pointerEvents="none"
          style={[styles.edge, { left: `${level}%`, backgroundColor: t.accent, opacity: on ? 0.8 : 0.35 }]}
        />
      </View>

      <View style={styles.row} pointerEvents="none">
        <View
          accessible={spoken !== undefined}
          accessibilityLabel={spoken}
          style={[styles.tile, { backgroundColor: on ? t.accent : t.fill }]}
        >
          <Glyph name="light" size={21} color={on ? t.onAccent : t.muted} filled={on} />
        </View>
        <View style={styles.text}>
          {title !== undefined ? (
            <Text style={[styles.name, { color: t.text }]} numberOfLines={1}>
              {title}
            </Text>
          ) : null}
          <Text style={[styles.sub, { color: on ? t.accentInk : t.muted }]} numberOfLines={1}>
            {on ? 'On' : 'Off'}
          </Text>
        </View>
        <Text style={[styles.grip, { color: t.accentInk }]}>{level}%</Text>
        {color !== null ? <View style={[styles.colorDot, { backgroundColor: color, borderColor: t.card }]} /> : null}
      </View>
      {note !== null ? <Text style={[styles.note, { color: t.muted }]}>{note}</Text> : null}
    </Pressable>
  );
}

const styles = StyleSheet.create({
  card: { borderWidth: 1, padding: 12, overflow: 'hidden' },
  fillLayer: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0 },
  fill: { position: 'absolute', left: 0, top: 0, bottom: 0 },
  edge: { position: 'absolute', top: 8, bottom: 8, width: 3, borderRadius: 2 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 11 },
  tile: { width: 44, height: 44, borderRadius: 14, alignItems: 'center', justifyContent: 'center' },
  text: { flex: 1, minWidth: 0, gap: 2 },
  name: { fontSize: 14.5, fontWeight: '600' },
  sub: { fontSize: 12.5, fontWeight: '500' },
  grip: { fontSize: 15, fontWeight: '700' },
  colorDot: { width: 26, height: 26, borderRadius: 13, borderWidth: 2 },
  note: { fontSize: 10.5, fontWeight: '500', marginTop: 4 },
});
