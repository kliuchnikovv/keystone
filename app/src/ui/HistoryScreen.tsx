import { useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { useHistory } from '../hooks/useHistory';
import { dayOf, describeEvent, entityName, formatEventTime, type Recorded } from '../model/history';
import type { HubClient } from '../protocol/client';
import type { HomeState } from '../protocol/store';
import { Button } from './controls';
import { useTheme } from './theme';

/**
 * What the hub has done and seen, newest first: device changes, the commands it sent, rules, schedules, timers
 * and scenes that ran, chips going quiet, restarts. Tap a row about a device to see only that device.
 */
export function HistoryScreen({ home, client, pollMs }: { home: HomeState; client: HubClient; pollMs?: number }) {
  const t = useTheme();
  const [entity, setEntity] = useState<number | null>(null);
  const h = useHistory(client, home.status, entity, pollMs);
  const now = Date.now();
  const tone = { normal: t.text, warn: t.warn, bad: t.bad };

  let lastDay: string | null | undefined;
  const rows = h.events.map((e: Recorded) => {
    const d = describeEvent(e, home);
    const day = dayOf(e, now);
    const heading = day !== lastDay && day !== null ? day : null;
    lastDay = day;
    const about = e.entity !== undefined && e.entity >= 0x0100 ? e.entity : null;
    return (
      <View key={e.seq}>
        {heading && <Text style={[styles.day, { color: t.muted }]}>{heading.toUpperCase()}</Text>}
        <Pressable
          accessibilityRole={about !== null ? 'button' : 'text'}
          accessibilityLabel={`${d.text}, ${formatEventTime(e, now)}`}
          disabled={about === null || entity !== null}
          onPress={() => about !== null && setEntity(about)}
          style={[styles.row, { borderBottomColor: t.border }]}
        >
          <Text style={styles.icon}>{d.icon || '•'}</Text>
          <Text style={[styles.text, { color: tone[d.tone] }]}>{d.text}</Text>
          <Text style={[styles.time, { color: t.muted }]}>{formatEventTime(e, now)}</Text>
        </Pressable>
      </View>
    );
  });

  return (
    <ScrollView contentContainerStyle={styles.page}>
      <Text style={[styles.h, { color: t.text }]}>History</Text>
      {entity !== null && (
        <View style={styles.filter}>
          <Text style={{ color: t.muted }}>Only {entityName(entity, home.entities)}</Text>
          <Button kind="plain" label="Show everything" onPress={() => setEntity(null)} />
        </View>
      )}
      {h.error && <Text accessibilityRole="alert" style={{ color: t.bad }}>{h.error}</Text>}
      {!h.loaded && !h.error && <Text style={{ color: t.muted }}>Loading…</Text>}
      {h.loaded && h.events.length === 0 && (
        <Text style={{ color: t.muted }}>{entity !== null ? 'Nothing recorded for this device yet.' : 'Nothing has happened since the hub started.'}</Text>
      )}
      <View accessibilityLabel="Events">{rows}</View>
      {h.hasMore && <Button kind="plain" label={h.loadingOlder ? 'Loading…' : 'Load older'} disabled={h.loadingOlder} onPress={() => void h.loadOlder()} />}
      <Text style={[styles.note, { color: t.muted }]}>
        The hub remembers its last 256 events in memory; a restart starts the list again.
      </Text>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, gap: 8 },
  h: { fontSize: 22, fontWeight: '700' },
  filter: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  day: { fontSize: 12, fontWeight: '700', letterSpacing: 1, marginTop: 12, marginBottom: 2 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingVertical: 10, borderBottomWidth: StyleSheet.hairlineWidth },
  icon: { fontSize: 18, width: 26, textAlign: 'center' },
  text: { flex: 1, fontSize: 15 },
  time: { fontSize: 12 },
  note: { fontSize: 12, marginTop: 12 },
});
