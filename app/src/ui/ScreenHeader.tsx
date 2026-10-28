// The bar at the top of a screen: the title in the middle, a round slot on each side.
// Like the room mock: 52 px tall, the title centred, 17 px bold.
import type { ReactNode } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { radius, useTheme } from './theme';

export function ScreenHeader({ title, left, right }: { title: string; left?: ReactNode; right?: ReactNode }) {
  const t = useTheme();
  return (
    <View style={styles.bar}>
      <View style={styles.slot}>{left}</View>
      <Text style={[styles.title, { color: t.text }]} numberOfLines={1}>
        {title}
      </Text>
      <View style={styles.slot}>{right}</View>
    </View>
  );
}

/** A round button for the header's side slots. */
export function HeaderButton({ label, glyph, onPress }: { label: string; glyph?: ReactNode; onPress(): void }) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={label}
      onPress={onPress}
      style={styles.button}
    >
      {glyph ?? <Text style={{ color: t.accent, fontSize: 17, fontWeight: '600' }}>{label}</Text>}
    </Pressable>
  );
}

const styles = StyleSheet.create({
  bar: { height: 52, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8 },
  slot: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center' },
  title: { flex: 1, textAlign: 'center', fontSize: 17, fontWeight: '700', letterSpacing: -0.3 },
  button: { width: 44, height: 44, borderRadius: radius.field, alignItems: 'center', justifyContent: 'center' },
});
