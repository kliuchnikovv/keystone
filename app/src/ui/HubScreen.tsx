import { useState } from 'react';
import { Linking, Platform, Pressable, ScrollView, Share, StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import { describeLocation, locationRequest } from '../model/automation';
import { chipSummary } from '../model/health';
import { locatePhone } from '../native/position';
import type { HomeState } from '../protocol/store';
import { Button, ErrorText, Input } from './controls';
import { WifiCard } from './WifiCard';
import { useTheme } from './theme';

/** The hub's position: what makes "at sunset" mean something. */
function LocationCard({ home, commands }: { home: HomeState; commands: Commands }) {
  const t = useTheme();
  const [editing, setEditing] = useState(false);
  const [lat, setLat] = useState('');
  const [lon, setLon] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [locating, setLocating] = useState(false);
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

  // The phone is normally in the same house as the hub: take its rough position instead of typing it.
  const usePhone = async () => {
    setError(null);
    setLocating(true);
    const p = await locatePhone();
    if (!p.ok) {
      setLocating(false);
      return setError(p.error);
    }
    await commands.request('location.set', { lat: p.lat, lon: p.lon });
    setLocating(false);
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
        <>
          <ErrorText text={error} />
          <View style={styles.row}>
            {loc && <Button kind="danger" label="Clear location" onPress={() => void commands.request('location.clear')} />}
            <Button kind="plain" label={loc ? 'Change location' : 'Set location'} onPress={open} />
            <Button label={locating ? 'Locating…' : 'Use this phone\u2019s location'} a11y="Use phone location" disabled={locating} onPress={() => void usePhone()} />
          </View>
        </>
      )}
    </View>
  );
}

// The setup code is from the esp-matter test DAC baked into the thread-c6 firmware.
// In a production build this would come from the hub via the protocol.
const MATTER_SETUP_CODE = '3497-0112-332';

function MatterBridgeCard({ home }: { home: HomeState }) {
  const t = useTheme();
  const tone = { good: t.good, warn: t.warn, bad: t.bad };
  // thread-c6 is chip 0x02
  const chip = home.chips.find((c) => c.id === 2);
  const s = chip ? chipSummary(chip) : null;

  const shareCode = () =>
    void Share.share({ message: MATTER_SETUP_CODE, title: 'Matter setup code' });

  const openHome = () => void Linking.openURL('x-apple-com-home://');

  return (
    <View accessibilityLabel="Apple Home bridge" style={[styles.card, { backgroundColor: t.card, borderColor: t.border }]}>
      <View style={styles.chipHead}>
        <Text style={[styles.h2, { color: t.text }]}>Apple Home bridge</Text>
        {s && <Text style={{ color: tone[s.tone], fontWeight: '600' }}>● {s.status}</Text>}
        {!chip && <Text style={{ color: t.muted }}>Not seen yet</Text>}
      </View>

      <Text style={{ color: t.muted, fontSize: 13 }}>
        Add this hub to Apple Home once — all Zigbee devices will appear inside it automatically.
      </Text>

      <Text style={[styles.step, { color: t.muted }]}>
        Home app → + → Add Accessory → More options → enter code:
      </Text>

      <Pressable
        accessibilityRole="button"
        accessibilityLabel="Copy setup code"
        onPress={shareCode}
        style={[styles.codeBox, { backgroundColor: t.bg, borderColor: t.border }]}
      >
        <Text selectable style={[styles.code, { color: t.text }]}>{MATTER_SETUP_CODE}</Text>
        <Text style={{ color: t.accent, fontSize: 13 }}>Share</Text>
      </Pressable>

      {Platform.OS === 'ios' && chip?.status === 'ready' && (
        <View style={[styles.row, { marginTop: 4 }]}>
          <Button label="Open Apple Home" onPress={openHome} />
        </View>
      )}

      {chip?.status !== 'ready' && (
        <Text style={{ color: t.muted, fontSize: 12 }}>
          Wait for the bridge chip to come online before commissioning.
        </Text>
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

      <WifiCard home={home} commands={commands} />
      <LocationCard home={home} commands={commands} />
      <MatterBridgeCard home={home} />

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
  row: { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'flex-end', gap: 8, marginTop: 4 },
  chipHead: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  change: { marginTop: 14, borderWidth: 1, borderRadius: 10, paddingVertical: 12, alignItems: 'center' },
  step: { fontSize: 13, marginTop: 4 },
  codeBox: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
             borderWidth: 1, borderRadius: 10, paddingHorizontal: 14, paddingVertical: 10, marginTop: 4 },
  code: { fontSize: 22, fontWeight: '700', letterSpacing: 2, fontVariant: ['tabular-nums'] },
});
