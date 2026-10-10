import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { chipSummary } from '../model/health';
import type { HomeState } from '../protocol/store';
import { useTheme } from './theme';

export function HubScreen({ home, url, onChangeHub }: { home: HomeState; url: string; onChangeHub(): void }) {
  const t = useTheme();
  const hub = home.hub;
  const tone = { good: t.good, warn: t.warn, bad: t.bad };
  return (
    <ScrollView contentContainerStyle={styles.page}>
      <View style={[styles.card, { backgroundColor: t.card, borderColor: t.border }]}>
        <Text style={[styles.h, { color: t.text }]}>{hub?.name ?? 'Hub'}</Text>
        <Text style={{ color: t.muted }}>{url}</Text>
        {hub && (
          <>
            <Text style={[styles.line, { color: t.text }]}>Firmware {hub.fw}</Text>
            <Text style={[styles.line, { color: t.text }]}>
              {hub.synced ? new Date(hub.time * 1000).toLocaleString() : 'Clock not synced'} · {hub.tz}
            </Text>
          </>
        )}
      </View>

      <Text style={[styles.section, { color: t.muted }]}>CHIPS</Text>
      {home.chips.length === 0 && <Text style={{ color: t.muted }}>No chips reported yet.</Text>}
      {home.chips.map((c) => {
        const s = chipSummary(c);
        return (
          <View key={c.id} accessibilityLabel={`Chip ${s.title}`} style={[styles.card, { backgroundColor: t.card, borderColor: t.border }]}>
            <View style={styles.chipHead}>
              <Text style={[styles.h2, { color: t.text }]}>{s.title}</Text>
              <Text style={{ color: tone[s.tone], fontWeight: '600' }}>● {s.status}</Text>
            </View>
            {s.details.length > 0 && <Text style={{ color: t.muted }}>{s.details.join(' · ')}</Text>}
          </View>
        );
      })}

      <Pressable accessibilityRole="button" accessibilityLabel="Change hub" onPress={onChangeHub} style={[styles.change, { borderColor: t.border }]}>
        <Text style={{ color: t.accent, fontWeight: '600' }}>Change hub or token</Text>
      </Pressable>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, gap: 10 },
  card: { borderRadius: 14, borderWidth: 1, padding: 14, gap: 4 },
  h: { fontSize: 22, fontWeight: '700' },
  h2: { fontSize: 17, fontWeight: '600' },
  line: { fontSize: 14, marginTop: 4 },
  section: { fontSize: 12, fontWeight: '700', letterSpacing: 1, marginTop: 10 },
  chipHead: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  change: { marginTop: 14, borderWidth: 1, borderRadius: 10, paddingVertical: 12, alignItems: 'center' },
});
