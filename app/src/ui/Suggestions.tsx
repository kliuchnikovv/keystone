// The suggested automations: a row per idea, with a round ink «+» that opens
// the rule already filled in.
import { Pressable, StyleSheet, Text, View } from './kit';
import { Glyph, type GlyphName } from './Glyph';
import type { Suggestion } from '../model/suggestions';
import { useTheme } from './theme';

export function Suggestions({ items, onPick }: { items: Suggestion[]; onPick(s: Suggestion): void }) {
  const t = useTheme();
  if (items.length === 0) return null;
  return (
    <View style={styles.list} accessibilityLabel="Suggested automations">
      {items.map((s) => (
        <View key={s.id} style={[styles.card, { backgroundColor: t.card, borderColor: t.border }]}>
          <View style={[styles.tile, { backgroundColor: t.accentSoft }]}>
            <Glyph name={glyphFor(s.id)} size={18} color={t.accentInk} />
          </View>
          <View style={styles.text}>
            <Text style={[styles.title, { color: t.text }]} numberOfLines={2}>
              {s.title}
            </Text>
            <Text style={[styles.detail, { color: t.muted }]} numberOfLines={2}>
              {s.detail}
            </Text>
          </View>
          <Pressable
            accessibilityRole="button"
            accessibilityLabel={`Add ${s.title}`}
            onPress={() => onPick(s)}
            style={[styles.add, { backgroundColor: t.primary }]}
          >
            <Text style={{ color: t.onPrimary, fontSize: 18, fontWeight: '700' }}>+</Text>
          </Pressable>
        </View>
      ))}
    </View>
  );
}

/** The icon that goes with the kind of suggestion. */
function glyphFor(id: string): GlyphName {
  if (id.startsWith('motion-')) return 'occupancy';
  if (id.startsWith('power-')) return 'bolt';
  return 'light';
}

const styles = StyleSheet.create({
  list: { gap: 8, marginTop: 10 },
  card: { flexDirection: 'row', alignItems: 'center', gap: 11, padding: 12, borderRadius: 16, borderWidth: 1 },
  tile: { width: 36, height: 36, borderRadius: 12, alignItems: 'center', justifyContent: 'center' },
  text: { flex: 1, gap: 2, minWidth: 0 },
  title: { fontSize: 13, fontWeight: '600', lineHeight: 18 },
  detail: { fontSize: 10.5, lineHeight: 14 },
  add: { width: 30, height: 30, borderRadius: 15, alignItems: 'center', justifyContent: 'center' },
});
