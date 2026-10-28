import { useState } from 'react';
import { Linking, Platform, Pressable, ScrollView, Share, StyleSheet, Text, View } from 'react-native';
import type { Commands } from '../hooks/useCommands';
import type { HubConnection } from '../hooks/useHub';
import { describeLocation, locationRequest } from '../model/automation';
import { chipRows, chipSummary } from '../model/health';
import { locatePhone } from '../native/position';
import type { HomeState } from '../protocol/store';
import { Button, ErrorText, Input } from './controls';
import { ScreenHeader } from './ScreenHeader';
import { AccessCard, FirmwareCard, HubActionsCard } from './HubCards';
import { WifiCard } from './WifiCard';
import { PALETTES, radius, useTheme, type PaletteId } from './theme';

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
    <View accessibilityLabel="Location" style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
      <Text style={[styles.h2, { color: t.text }]}>Location</Text>
      <Text style={{ color: t.muted }}>{describeLocation(home.hub)}</Text>
      {editing ? (
        <View style={{ gap: 8, marginTop: 6 }}>
          <Input label="Latitude" value={lat} onChangeText={setLat} placeholder="50.08 (north is positive)" keyboardType="numbers-and-punctuation" />
          <Input label="Longitude" value={lon} onChangeText={setLon} placeholder="14.43 (east is positive)" keyboardType="numbers-and-punctuation" />
          <ErrorText text={error} />
          <View style={styles.row}>
            <Button kind="secondary" label="Cancel" onPress={() => setEditing(false)} />
            <Button label="Save location" onPress={save} />
          </View>
        </View>
      ) : (
        <>
          <ErrorText text={error} />
          <View style={styles.row}>
            {loc && <Button kind="danger" size="compact" label="Clear location" onPress={() => void commands.request('location.clear')} />}
            <Button kind="plain" size="compact" label={loc ? 'Change location' : 'Set location'} onPress={open} />
            <Button size="compact" label={locating ? 'Locating…' : 'Use this phone\u2019s location'} a11y="Use phone location" disabled={locating} onPress={() => void usePhone()} />
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
    <View accessibilityLabel="Apple Home bridge" style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
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
          <Button size="compact" label="Open Apple Home" onPress={openHome} />
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

/** The app's own look: the palette is a setting of the phone, not of the hub. */
function AppearanceCard({ palette, onPalette }: { palette: PaletteId; onPalette?(id: PaletteId): void }) {
  const t = useTheme();
  return (
    <View accessibilityLabel="Appearance" style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
      <Text style={[styles.h2, { color: t.text }]}>Appearance</Text>
      <Text style={{ color: t.muted, fontSize: 13 }}>The palette the app wears. Ember is the dark glass look.</Text>
      <View style={styles.paletteRow}>
        {PALETTES.map((p) => {
          const on = p.id === palette;
          return (
            <Pressable
              key={p.id}
              accessibilityRole="button"
              accessibilityLabel={`Palette ${p.name}`}
              accessibilityState={{ selected: on }}
              onPress={() => onPalette?.(p.id)}
              style={[styles.palette, { borderColor: on ? t.accent : t.border, backgroundColor: on ? t.accentSoft : t.fill }]}
            >
              <Text style={{ color: on ? t.accent : t.text, fontSize: 14, fontWeight: '700' }}>{p.name}</Text>
              <Text style={{ color: t.muted, fontSize: 11 }}>{p.hint}</Text>
            </Pressable>
          );
        })}
      </View>
    </View>
  );
}

export function HubScreen({ home, url, onChangeHub, commands, client, onTokenChange, onUseRelay, palette, onPalette }: { home: HomeState; url: string; onChangeHub(): void; commands: Commands; client: HubConnection; onTokenChange?(token: string): Promise<void>; onUseRelay?(url: string): void; palette?: PaletteId; onPalette?(id: PaletteId): void }) {
  const t = useTheme();
  const hub = home.hub;
  const tone = { good: t.good, warn: t.warn, bad: t.bad };
  // One dot for the whole hub: healthy, degraded (a chip is quiet) or offline.
  const hubTone = home.status !== 'ready' ? 'bad' : home.chips.some((c) => c.status !== 'ready') ? 'warn' : 'good';
  const hubStatus = hubTone === 'bad' ? 'offline' : hubTone === 'warn' ? 'degraded' : 'healthy';
  return (
    <ScrollView contentContainerStyle={styles.page}>
      <ScreenHeader title="Hub" />
      <View style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
        <View style={styles.hubHead}>
          <View style={[styles.dot, { backgroundColor: tone[hubTone] }]} accessibilityLabel={`Hub is ${hubStatus}`} />
          <Text style={[styles.h, { color: t.text }]}>{hub?.name ?? 'Hub'}</Text>
          <Text style={{ color: tone[hubTone], fontWeight: '600', fontSize: 12 }}>{hubStatus}</Text>
        </View>
        <View style={styles.kv}>
          <Text style={{ color: t.muted }}>{url}</Text>
        </View>
        {hub && (
          <>
            <View style={styles.kv}>
              <Text style={{ color: t.muted }}>Firmware</Text>
              <Text style={{ color: t.text, fontWeight: '600' }}>{hub.fw}</Text>
            </View>
            <View style={styles.kv}>
              <Text style={{ color: t.muted }}>Clock</Text>
              <Text style={{ color: t.text, fontWeight: '600' }} numberOfLines={1}>
                {hub.synced ? `${new Date(hub.time * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })} · ${hub.tz}` : 'not synced'}
              </Text>
            </View>
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
          <View key={c.id} accessibilityLabel={`Chip ${s.title}`} style={[styles.card, { backgroundColor: t.card, borderColor: t.border, borderRadius: t.cardRadius }]}>
            <View style={styles.chipHead}>
              <Text style={[styles.h2, { color: t.text }]}>{s.title}</Text>
              <Text style={{ color: tone[s.tone], fontWeight: '600' }}>● {s.status}</Text>
            </View>
            {chipRows(c).map((r) => (
              <View key={r.label} style={styles.kv}>
                <Text style={{ color: t.muted }}>{r.label}</Text>
                <Text style={{ color: t.text, fontWeight: '600' }}>{r.value}</Text>
              </View>
            ))}
          </View>
        );
      })}

      <FirmwareCard home={home} client={client} commands={commands} />
      <AccessCard client={client} commands={commands} {...(onTokenChange ? { onTokenChange } : {})} {...(onUseRelay ? { onUseRelay } : {})} />
      <HubActionsCard commands={commands} onChangeHub={onChangeHub} />
      <AppearanceCard palette={palette ?? 'sandstone'} onPalette={onPalette} />

      <Pressable accessibilityRole="button" accessibilityLabel="Change hub" onPress={onChangeHub} style={[styles.change, { borderColor: t.border }]}>
        <Text style={{ color: t.accent, fontWeight: '600' }}>Change hub or token</Text>
      </Pressable>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  page: { padding: 16, paddingBottom: Platform.OS === 'ios' ? 110 : 98, gap: 10 },
  card: { borderWidth: 1, padding: 14, gap: 4 },
  h: { fontSize: 22, fontWeight: '700' },
  hubHead: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  dot: { width: 10, height: 10, borderRadius: 5 },
  h2: { fontSize: 17, fontWeight: '600' },
  kv: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 10, fontSize: 12.5 },
  section: { fontSize: 12, fontWeight: '700', letterSpacing: 1, marginTop: 10 },
  row: { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'flex-end', gap: 8, marginTop: 4 },
  chipHead: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  change: { marginTop: 14, borderWidth: 1, borderRadius: 12, paddingVertical: 11, paddingHorizontal: 14, alignItems: 'center' },
  step: { fontSize: 13, marginTop: 4 },
  codeBox: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
             borderWidth: 1, borderRadius: radius.field, paddingHorizontal: 14, paddingVertical: 10, marginTop: 4 },
  code: { fontSize: 22, fontWeight: '700', letterSpacing: 2, fontVariant: ['tabular-nums'] },
  paletteRow: { flexDirection: 'row', gap: 10, marginTop: 10 },
  palette: { flex: 1, borderWidth: 1, borderRadius: radius.card, paddingHorizontal: 12, paddingVertical: 10, gap: 2 },
});
