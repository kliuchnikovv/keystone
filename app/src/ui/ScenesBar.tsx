import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import type { HomeState } from '../protocol/store';
import { Glyph } from './Glyph';
import { radius, useTheme } from './theme';

/** One tap per scene, above the rooms. Says how many settings went out (and how many could not). */
export function ScenesBar({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  if (home.scenes.length === 0) return null;

  const run = async (name: string) => {
    const res = await commands.request('scene.apply', { name });
    if (res === null) return; // the failure toast says why
    const sent = Number(res.sent ?? 0);
    const skipped = Number(res.skipped ?? 0);
    commands.notify(skipped > 0 ? `Applied '${name}': ${sent} sent, ${skipped} skipped` : `Applied '${name}': ${sent} settings sent`);
  };

  return (
    <View style={styles.box} accessibilityLabel="Scenes">
      <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={styles.rail}>
        {home.scenes.map((s) => (
          <Pressable
            key={s.name}
            accessibilityRole="button"
            accessibilityLabel={`Apply scene ${s.name}`}
            onPress={() => void run(s.name)}
            style={({ pressed }) => [styles.pill, { backgroundColor: pressed ? t.accentSoft : t.card, borderColor: t.border }]}
          >
            <Glyph name="play" size={11} color={t.accent} />
            <Text style={[styles.pillText, { color: t.text }]}>{s.name}</Text>
          </Pressable>
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  box: { marginBottom: 8 },
  rail: { gap: 8, paddingRight: 8 },
  pill: { flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: 14, paddingVertical: 9, borderRadius: radius.pill, borderWidth: 1 },
  pillText: { fontSize: 14, fontWeight: '600' },
});
