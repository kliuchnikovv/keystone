// The hub's affairs, one screen each.
//
// Settings is a list of doors; behind each is one thing about the hub. The
// screens share a frame: a title, a way back, and the cards that belong there.
import { type ReactNode } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from './kit';
import { Glyph } from './Glyph';
import { useTheme } from './theme';

/** The frame every hub sub-screen sits in: a title and a way back. */
export function HubSubScreen({
  title,
  onBack,
  children,
}: {
  title: string;
  onBack(): void;
  children: ReactNode;
}) {
  const t = useTheme();
  return (
    <ScrollView contentContainerStyle={styles.page}>
      <View style={styles.bar}>
        <Pressable accessibilityRole="button" accessibilityLabel="Back to Settings" onPress={onBack} style={styles.back}>
          <Glyph name="back" size={20} color={t.muted} />
        </Pressable>
        <Text style={[styles.title, { color: t.text }]} numberOfLines={1}>
          {title}
        </Text>
      </View>
      {children}
    </ScrollView>
  );
}

/** A label between the cards, the way the sections of the hub screen read. */
export function SectionLabel({ children }: { children: ReactNode }) {
  const t = useTheme();
  return <Text style={[styles.section, { color: t.muted }]}>{children}</Text>;
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: 40, gap: 10 },
  bar: { height: 44, flexDirection: 'row', alignItems: 'center' },
  back: { width: 44, height: 44, alignItems: 'center', justifyContent: 'center' },
  title: { flex: 1, fontSize: 17, fontWeight: '700', letterSpacing: -0.3, textAlign: 'center' },
  section: { fontSize: 12, fontWeight: '700', letterSpacing: 1, marginTop: 10 },
});
