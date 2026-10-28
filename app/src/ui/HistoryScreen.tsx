import { useEffect, useRef, useState } from 'react';
import { Animated, Platform, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { useHistory } from '../hooks/useHistory';
import { dayOf, describeEvent, entityName, formatEventTime, type Recorded } from '../model/history';
import type { HubConnection } from '../hooks/useHub';
import type { HomeState } from '../protocol/store';
import { Glyph } from './Glyph';
import { ScreenHeader } from './ScreenHeader';
import { useTheme } from './theme';

/**
 * What the hub has done and seen, newest first: device changes, the commands it sent, rules, schedules, timers
 * and scenes that ran, chips going quiet, restarts. Tap a row about a device to see only that device.
 */
export function HistoryScreen({ home, client, pollMs }: { home: HomeState; client: HubConnection; pollMs?: number }) {
  const t = useTheme();
  const [entity, setEntity] = useState<number | null>(null);
  const h = useHistory(client, home.status, entity, pollMs);
  const now = Date.now();
  const tone = { normal: t.text, warn: t.warn, bad: t.bad };

  // Events the hub pushes arrive at the top; the newest one fades in.
  const seen = useRef<Set<number>>(new Set());
  const [fresh, setFresh] = useState<number | null>(null);
  useEffect(() => {
    const unseen = h.events.filter((e) => !seen.current.has(e.seq));
    if (unseen.length === 0) return;
    for (const e of unseen) seen.current.add(e.seq);
    setFresh(unseen[0]!.seq);
  }, [h.events]);

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
        <FadeIn animate={e.seq === fresh}>
          <Pressable
            accessibilityRole={about !== null ? 'button' : 'text'}
            accessibilityLabel={`${d.text}, ${formatEventTime(e, now)}`}
            disabled={about === null || entity !== null}
            onPress={() => about !== null && setEntity(about)}
            style={[styles.row, { borderBottomColor: t.border }]}
          >
            {/* The tone rides on the icon; the words stay calm. */}
            <View style={styles.icon}>{d.icon && <Glyph name={d.icon} size={16} color={tone[d.tone]} />}</View>
            <Text style={[styles.text, { color: t.text }]}>{d.text}</Text>
            <Text style={[styles.time, { color: t.muted }]}>{formatEventTime(e, now)}</Text>
          </Pressable>
        </FadeIn>
      </View>
    );
  });

  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ScreenHeader title="History" />
      {entity !== null && (
        <View style={[styles.filter, { backgroundColor: t.accentSoft }]}>
          <Text style={styles.filterText}>Only {entityName(entity, home.entities)}</Text>
          <Pressable accessibilityRole="button" accessibilityLabel="Show everything" onPress={() => setEntity(null)} hitSlop={6}>
            <Text style={[styles.filterAction, { color: t.accent }]}>Show everything</Text>
          </Pressable>
        </View>
      )}
      {h.error && <Text accessibilityRole="alert" style={{ color: t.bad }}>{h.error}</Text>}
      {!h.loaded && !h.error && <Text style={{ color: t.muted }}>Loading…</Text>}
      {h.loaded && h.events.length === 0 && (
        <Text style={{ color: t.muted }}>{entity !== null ? 'Nothing recorded for this device yet.' : 'Nothing has happened since the hub started.'}</Text>
      )}
      <View accessibilityLabel="Events">{rows}</View>
      {h.hasMore && (
        <View style={styles.center}>
          <Pressable
            accessibilityRole="button"
            accessibilityLabel={h.loadingOlder ? 'Loading older' : 'Load older'}
            disabled={h.loadingOlder}
            onPress={() => void h.loadOlder()}
          >
            <Text style={[styles.quiet, { color: t.accent }]}>{h.loadingOlder ? 'Loading…' : 'Load older'}</Text>
          </Pressable>
        </View>
      )}
      <Text style={[styles.note, { color: t.muted }]}>
        The hub remembers its last 256 events in memory; a restart starts the list again.
      </Text>
    </ScrollView>
  );
}

/** A row that fades in when it is the newest one the hub pushed. */
function FadeIn({ animate, children }: { animate: boolean; children: React.ReactNode }) {
  const opacity = useRef(new Animated.Value(animate ? 0 : 1)).current;
  const offset = useRef(new Animated.Value(animate ? -10 : 0)).current;
  useEffect(() => {
    if (!animate) return;
    Animated.parallel([
      Animated.timing(opacity, { toValue: 1, duration: 220, useNativeDriver: Platform.OS !== 'web' }),
      Animated.timing(offset, { toValue: 0, duration: 220, useNativeDriver: Platform.OS !== 'web' }),
    ]).start();
  }, [animate, opacity, offset]);
  return <Animated.View style={{ opacity, transform: [{ translateY: offset }] }}>{children}</Animated.View>;
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: Platform.OS === 'ios' ? 110 : 98, gap: 8 },
  h: { fontSize: 22, fontWeight: '700' },
  filter: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 12, paddingHorizontal: 11, paddingVertical: 9, borderRadius: 12 },
  filterText: { fontSize: 13 },
  filterAction: { fontSize: 12.5, fontWeight: '600' },
  day: { fontSize: 11, fontWeight: '700', letterSpacing: 1, marginTop: 12, marginBottom: 2 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingVertical: 9, borderBottomWidth: StyleSheet.hairlineWidth },
  icon: { width: 24, alignItems: 'flex-start' },
  text: { flex: 1, fontSize: 13.5, lineHeight: 18 },
  time: { fontSize: 11 },
  center: { alignItems: 'center', marginTop: 14 },
  quiet: { fontSize: 13, fontWeight: '600' },
  note: { fontSize: 11.5, marginTop: 6, textAlign: 'center', lineHeight: 17 },
});
