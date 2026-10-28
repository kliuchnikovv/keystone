// A slider that works everywhere: no native module, no dependency. The track measures
// itself, a drag maps the touch to a value, and screen readers get the value.
import { useRef, useState } from 'react';
import { Platform, Pressable, StyleSheet, Text, View, type LayoutChangeEvent } from 'react-native';
import { useTheme } from './theme';

/** Where a touch at `x` pixels on a track of `width` pixels lands, snapped to `step`. */
export function sliderValueAt(x: number, width: number, min: number, max: number, step: number): number {
  if (width <= 0) return min;
  const span = max - min;
  const ratio = Math.min(1, Math.max(0, x / width));
  const raw = min + ratio * span;
  const snapped = Math.round((raw - min) / step) * step + min;
  return Math.min(max, Math.max(min, Number(snapped.toFixed(4))));
}

export interface SliderProps {
  label: string;
  /** The value in the slider's own units (percent, kelvin …), not the wire value. */
  value: number;
  min: number;
  max: number;
  step?: number;
  onChange(v: number): void;
  /** Called when the gesture ends — the moment to send the command. */
  onCommit?(v: number): void;
  disabled?: boolean;
  /** How the current value reads in the label ("60 %"). */
  format?(v: number): string;
  /** A colour to paint the fill with (the colour picker uses it). */
  fill?: string;
  /** What a screen reader calls it — the device name belongs in it. */
  a11yLabel?: string;
  /** Big − and + buttons beside the track, for people who do not drag. */
  stepper?: {
    onDown(): void;
    onUp(): void;
    downLabel: string;
    upLabel: string;
  };
}

export function Slider({ label, value, min, max, step = 1, onChange, onCommit, disabled, format, fill, a11yLabel, stepper }: SliderProps) {
  const t = useTheme();
  const [width, setWidth] = useState(0);
  const widthRef = useRef(0);
  const latest = useRef(value);
  latest.current = value;

  const span = max - min;
  const ratio = span > 0 ? Math.min(1, Math.max(0, (value - min) / span)) : 0;

  const fromX = (x: number): number => sliderValueAt(x, widthRef.current, min, max, step);

  const onLayout = (e: LayoutChangeEvent) => {
    const w = e.nativeEvent.layout.width;
    widthRef.current = w;
    setWidth(w);
  };

  const nudge = (dir: 1 | -1) => {
    if (disabled) return;
    const v = sliderValueAt(latest.current + dir * step, 1, min, max, step);
    latest.current = v;
    onChange(v);
    onCommit?.(v);
  };

  const text = format ? format(value) : String(value);
  const knob = 22;
  const travel = Math.max(0, width - knob);
  const left = ratio * travel;

  // The web build reads the aria-* props; the native build reads accessibilityValue.
  const web = Platform.OS === 'web'
    ? {
        'aria-valuenow': value,
        'aria-valuemin': min,
        'aria-valuemax': max,
        'aria-valuetext': text,
        ...(disabled ? { 'aria-disabled': true } : {}),
      }
    : {};

  return (
    <View style={styles.wrap}>
      <View style={styles.head}>
        <Text style={[styles.label, { color: t.muted }]}>{label}</Text>
        <Text style={[styles.value, { color: t.text }]}>{text}</Text>
      </View>
      <View style={styles.row}>
        {stepper ? (
          <Pressable
            accessibilityRole="button"
            accessibilityLabel={stepper.downLabel}
            accessibilityState={{ disabled: !!disabled }}
            disabled={disabled}
            onPress={stepper.onDown}
            style={[styles.step, { borderColor: t.border, backgroundColor: t.accentSoft, opacity: disabled ? 0.4 : 1 }]}
          >
            <Text style={[styles.stepText, { color: t.text }]}>−</Text>
          </Pressable>
        ) : null}
        <View
          accessible
          accessibilityRole="adjustable"
          accessibilityLabel={a11yLabel ?? label}
          accessibilityState={{ disabled: !!disabled }}
          accessibilityValue={{ min, max, now: value, text }}
          accessibilityActions={[{ name: 'increment' }, { name: 'decrement' }]}
          onAccessibilityAction={(e) => nudge(e.nativeEvent.actionName === 'increment' ? 1 : -1)}
          onLayout={onLayout}
          {...web}
          style={[styles.track, { backgroundColor: t.border }, disabled && styles.disabled]}
          onStartShouldSetResponder={() => !disabled}
          onMoveShouldSetResponder={() => !disabled}
          onResponderGrant={(e) => {
            const v = fromX(e.nativeEvent.locationX);
            latest.current = v;
            onChange(v);
          }}
          onResponderMove={(e) => {
            const v = fromX(e.nativeEvent.locationX);
            if (v !== latest.current) {
              latest.current = v;
              onChange(v);
            }
          }}
          onResponderRelease={() => onCommit?.(latest.current)}
          onResponderTerminate={() => onCommit?.(latest.current)}
        >
          <View style={[styles.fill, { width: left + knob / 2, backgroundColor: fill ?? t.accent }]} />
          <View style={[styles.knob, { left, borderColor: t.bg }]} />
        </View>
        {stepper ? (
          <Pressable
            accessibilityRole="button"
            accessibilityLabel={stepper.upLabel}
            accessibilityState={{ disabled: !!disabled }}
            disabled={disabled}
            onPress={stepper.onUp}
            style={[styles.step, { borderColor: t.border, backgroundColor: t.accentSoft, opacity: disabled ? 0.4 : 1 }]}
          >
            <Text style={[styles.stepText, { color: t.text }]}>+</Text>
          </Pressable>
        ) : null}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: { marginTop: 10 },
  head: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  label: { fontSize: 13 },
  value: { fontSize: 14, fontWeight: '600' },
  row: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  track: { height: 6, borderRadius: 3, marginTop: 8, justifyContent: 'center', flex: 1 },
  fill: { position: 'absolute', left: 0, height: 6, borderRadius: 3 },
  knob: { position: 'absolute', width: 22, height: 22, borderRadius: 11, backgroundColor: '#ffffff', borderWidth: 2 },
  step: { width: 34, height: 34, borderRadius: 17, borderWidth: 1, alignItems: 'center', justifyContent: 'center' },
  stepText: { fontSize: 20, fontWeight: '600', lineHeight: 24 },
  disabled: { opacity: 0.4 },
});
