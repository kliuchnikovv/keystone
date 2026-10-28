// Picking a lamp's colour: a bottom sheet with the swatch, hue and saturation.
// The protocol speaks CIE x/y, so the picker works in HSV and converts on the way out.
// Brightness is not here: it is the lamp's own slider (a chromaticity carries none).
import { useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { hsvToCss, hsvToXy, xyToHsv, type Hsv } from '../model/color';
import { useTheme } from './theme';
import { Sheet } from './Sheet';
import { Slider } from './Slider';

export interface ColorSheetProps {
  visible: boolean;
  title: string;
  /** The lamp's colour right now, as the wire's [x, y] pair. */
  value: [number, number] | null;
  onCancel(): void;
  /** The chosen colour, as the wire's [x, y] pair. */
  onApply(xy: [number, number]): void;
}

export function ColorSheet({ visible, title, value, onCancel, onApply }: ColorSheetProps) {
  const t = useTheme();
  const start: Hsv = value ? xyToHsv(value[0], value[1]) : { h: 30, s: 0.8, v: 1 };
  const [colour, setColour] = useState<Hsv>(start);

  const close = () => {
    setColour(value ? xyToHsv(value[0], value[1]) : start);
    onCancel();
  };
  const apply = () => {
    onApply(hsvToXy(colour));
    onCancel();
  };

  return (
    <Sheet visible={visible} title={title} onCancel={close}>
      <View style={styles.previewRow}>
        <View style={[styles.swatch, { backgroundColor: hsvToCss(colour) }]} />
        <View style={{ flex: 1 }}>
          <Text style={{ color: t.muted, fontSize: 13 }}>
            {`Hue ${Math.round(colour.h)}° · saturation ${Math.round(colour.s * 100)} %`}
          </Text>
          <Text style={{ color: t.muted, fontSize: 11, marginTop: 2 }}>Brightness is the lamp's own slider.</Text>
        </View>
      </View>

      <Slider
        label="Hue"
        value={colour.h}
        min={0}
        max={360}
        step={5}
        onChange={(h) => setColour((c) => ({ ...c, h }))}
        format={(v) => `${Math.round(v)}°`}
        fill={hsvToCss({ ...colour, s: 1, v: 1 })}
      />
      <Slider
        label="Saturation"
        value={colour.s * 100}
        min={0}
        max={100}
        step={5}
        onChange={(s) => setColour((c) => ({ ...c, s: s / 100 }))}
        format={(v) => `${Math.round(v)} %`}
        fill={hsvToCss({ ...colour, s: 1 })}
      />

      <View style={styles.actions}>
        <Pressable accessibilityRole="button" accessibilityLabel="Cancel the colour" onPress={close} style={[styles.button, { borderColor: t.border }]}>
          <Text style={{ color: t.text, fontWeight: '600', fontSize: 15 }}>Cancel</Text>
        </Pressable>
        <Pressable accessibilityRole="button" accessibilityLabel="Apply the colour" onPress={apply} style={[styles.button, { backgroundColor: t.accent, borderColor: t.accent }]}>
          <Text style={{ color: t.onAccent, fontWeight: '600', fontSize: 15 }}>Apply</Text>
        </Pressable>
      </View>
    </Sheet>
  );
}

const styles = StyleSheet.create({
  previewRow: { flexDirection: 'row', alignItems: 'center', gap: 12, marginTop: 14 },
  swatch: { width: 46, height: 46, borderRadius: 12 },
  actions: { flexDirection: 'row', gap: 8, marginTop: 20 },
  button: { flex: 1, alignItems: 'center', paddingVertical: 12, borderRadius: 14, borderWidth: 1 },
});
