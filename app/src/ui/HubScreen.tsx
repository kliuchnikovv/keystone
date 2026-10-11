import { useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import { describeLocation, locationRequest } from '../model/automation';
import { chipSummary } from '../model/health';
import type { HomeState } from '../protocol/store';
import { Button, ErrorText, Input } from './controls';
import { useTheme } from './theme';

/** The hub's position: what makes "at sunset" mean something. */
function LocationCard({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [editing, setEditing] = useState(false);
  const [lat, setLat] = useState('');
  const [lon, setLon] = useState('');
  const [error, setError] = useState<string | null>(null);
  const loc = home.hub?.location ?? null;

  const open = () => {
    setLat(loc ? String(loc.lat) : '');
    setLon(loc ? String(loc.lon) : '');
    setError(null);
    setEditing(true);
  };
  const save = async () => {
    const r = locationRequest(lat, lon);
    if (!r.ok) return setError(r.error);
    setError(null);
    if ((await commands.request('location.set', r.value)) !== null) setEditing(false);
  };

  return (
    <View accessibilityLabel="Location" style={[styles.card, { backgroundColor: t.card, borderColor: t.border }]}>
      <Text style={[styles.h2, { color: t.text }]}>Location</Text>
      <Text style={{ color: t.muted }}>{describeLocation(home.hub)}</Text>
      {editing ? (
        <View style={{ gap: 8, marginTop: 6 }}>
          <Input label="Latitude" value={lat} onChangeText={setLat} placeholder="50.08 (north is positive)" keyboardType="numbers-and-punctuation" />
          <Input label="Longitude" value={lon} onChangeText={setLon} placeholder="14.43 (east is positive)" keyboardType="numbers-and-punctuation" />
          <ErrorText text={error} />
          <View style={styles.row}>
            <Button kind="plain" label="Cancel" onPress={() => setEditing(false)} />
            <Button label="Save location" onPress={save} />
          </View>
        </View>
      ) : (
        <View style={styles.row}>
          {loc && <Button kind="danger" label="Clear location" onPress={() => void commands.request('location.clear')} />}
          <Button kind="plain" label={loc ? 'Change location' : 'Set location'} onPress={open} />
        </View>
      )}
    </View>
  );
}

export function HubScreen({ home, url, onChangeHub, commands }: { home: HomeState; url: string; onChangeHub(): void; commands: Commands }) {
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

      <LocationCard home={home} commands={commands} />

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
  row: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 4 },
  chipHead: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  change: { marginTop: 14, borderWidth: 1, borderRadius: 10, paddingVertical: 12, alignItems: 'center' },
});
